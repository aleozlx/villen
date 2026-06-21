#include "gpu_backend.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <cctype>
#include <cstring>

namespace villen {
namespace {

// The morphology kernel (§4.2), in GLES-3.1 form. One invocation per output
// pixel: the min (erode) or max (dilate) over the structuring-element
// neighbourhood, with clamp-to-edge sampling. rgba8ui processes all channels at
// once — vectorised per-channel min/max, identical to the CPU PerChannel mode.
const char* kMorphSrc = R"(#version 310 es
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8ui, binding = 0) uniform readonly  highp uimage2D uSrc;
layout(rgba8ui, binding = 1) uniform writeonly highp uimage2D uDst;
uniform int uRadius;
uniform int uShape;   // 0 box, 1 cross, 2 disk
uniform int uErode;   // 1 erode (min), 0 dilate (max)
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  ivec2 sz = imageSize(uSrc);
  if (p.x >= sz.x || p.y >= sz.y) return;
  uvec4 acc = (uErode != 0) ? uvec4(255u) : uvec4(0u);
  for (int dy = -uRadius; dy <= uRadius; ++dy) {
    for (int dx = -uRadius; dx <= uRadius; ++dx) {
      if (uShape == 1 && dx != 0 && dy != 0) continue;
      if (uShape == 2 && (dx*dx + dy*dy) > uRadius*uRadius) continue;
      ivec2 q = clamp(p + ivec2(dx, dy), ivec2(0), sz - ivec2(1));
      uvec4 v = imageLoad(uSrc, q);
      acc = (uErode != 0) ? min(acc, v) : max(acc, v);
    }
  }
  imageStore(uDst, p, acc);
}
)";

// The combine kernel (§4.2): saturating subtract, threshold, or invert.
const char* kCombineSrc = R"(#version 310 es
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8ui, binding = 0) uniform readonly highp uimage2D uA;
layout(rgba8ui, binding = 1) uniform readonly highp uimage2D uB;
layout(rgba8ui, binding = 2) uniform writeonly highp uimage2D uDst;
uniform int uMode;  // 0 subtract(a-b clamped), 1 threshold(a>=t), 2 invert(a)
uniform int uT;
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  ivec2 sz = imageSize(uA);
  if (p.x >= sz.x || p.y >= sz.y) return;
  uvec4 a = imageLoad(uA, p);
  uvec4 r;
  if (uMode == 0) {
    uvec4 b = imageLoad(uB, p);
    r = a - min(a, b);                       // max(a-b, 0) without underflow
  } else if (uMode == 1) {
    r = uvec4(greaterThanEqual(a, uvec4(uint(uT)))) * 255u;
  } else {
    r = uvec4(255u) - a;
  }
  imageStore(uDst, p, r);
}
)";

enum { kSub = 0, kThresh = 1, kInvert = 2 };

GLuint compileCompute(const char* src) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(sh); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    glDeleteShader(sh);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(prog); return 0; }
    return prog;
}

bool looksSoftware(const std::string& renderer) {
    std::string r;
    r.reserve(renderer.size());
    for (char c : renderer) r.push_back(static_cast<char>(std::tolower(c)));
    return r.find("llvmpipe") != std::string::npos ||
           r.find("softpipe") != std::string::npos ||
           r.find("swrast") != std::string::npos;
}

}  // namespace

std::unique_ptr<GpuBackend> GpuBackend::tryCreate(std::string* why) {
    std::unique_ptr<GpuBackend> b(new GpuBackend());
    if (!b->init(why)) return nullptr;
    return b;
}

bool GpuBackend::init(std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                           EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY) return fail("no surfaceless EGL display");
    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min)) return fail("eglInitialize failed");
    dpy_ = dpy;

    if (!eglBindAPI(EGL_OPENGL_ES_API)) return fail("eglBindAPI(ES) failed");
    const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                              EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                              EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1)
        return fail("no ES3 EGL config");

    const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
                              EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) return fail("eglCreateContext(ES 3.1) failed");
    ctx_ = ctx;

    if (!makeCurrent()) return fail("eglMakeCurrent failed");

    const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    renderer_ = r ? r : "unknown";
    software_ = looksSoftware(renderer_);

    progMorph_ = compileCompute(kMorphSrc);
    progCombine_ = compileCompute(kCombineSrc);
    if (!progMorph_ || !progCombine_) return fail("compute shader compile/link failed");

    uMorphRadius_ = glGetUniformLocation(progMorph_, "uRadius");
    uMorphShape_ = glGetUniformLocation(progMorph_, "uShape");
    uMorphErode_ = glGetUniformLocation(progMorph_, "uErode");
    uCombMode_ = glGetUniformLocation(progCombine_, "uMode");
    uCombT_ = glGetUniformLocation(progCombine_, "uT");

    glGenFramebuffers(1, &fbo_);
    return true;
}

GpuBackend::~GpuBackend() {
    if (dpy_) {
        makeCurrent();
        destroyTextures();
        if (fbo_) glDeleteFramebuffers(1, &fbo_);
        if (progMorph_) glDeleteProgram(progMorph_);
        if (progCombine_) glDeleteProgram(progCombine_);
        eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx_) eglDestroyContext(dpy_, ctx_);
        eglTerminate(dpy_);
    }
}

bool GpuBackend::makeCurrent() {
    return eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx_);
}

void GpuBackend::destroyTextures() {
    if (!pool_.empty()) glDeleteTextures(static_cast<GLsizei>(pool_.size()), pool_.data());
    pool_.clear();
}

bool GpuBackend::ensureTextures(int w, int h) {
    if (w == w_ && h == h_ && !pool_.empty()) return true;
    destroyTextures();
    // Six rgba8ui work textures: a gradient's worst live set is {cur, dilate,
    // erode, dst} = 4; six leaves margin for the invert toggle's extra hop.
    pool_.resize(6);
    glGenTextures(6, pool_.data());
    for (unsigned t : pool_) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8UI, w, h);  // immutable: image-store-able
    }
    w_ = w;
    h_ = h;
    return glGetError() == GL_NO_ERROR;
}

unsigned GpuBackend::acquire(std::initializer_list<unsigned> busy) {
    for (unsigned t : pool_) {
        bool taken = false;
        for (unsigned b : busy)
            if (b == t) { taken = true; break; }
        if (!taken) return t;
    }
    return pool_.back();  // unreachable with a pool of 6 and <=5 busy
}

void GpuBackend::morph(unsigned src, unsigned dst, const filter::StructElem& se,
                       bool erode) {
    glUseProgram(progMorph_);
    glBindImageTexture(0, src, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8UI);
    glBindImageTexture(1, dst, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8UI);
    glUniform1i(uMorphRadius_, se.radius);
    glUniform1i(uMorphShape_, static_cast<int>(se.shape));
    glUniform1i(uMorphErode_, erode ? 1 : 0);
    glDispatchCompute((w_ + 15) / 16, (h_ + 15) / 16, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
}

void GpuBackend::combine(unsigned a, unsigned b, unsigned dst, int mode, int t) {
    glUseProgram(progCombine_);
    glBindImageTexture(0, a, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8UI);
    glBindImageTexture(1, b, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8UI);
    glBindImageTexture(2, dst, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8UI);
    glUniform1i(uCombMode_, mode);
    glUniform1i(uCombT_, t);
    glDispatchCompute((w_ + 15) / 16, (h_ + 15) / 16, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
}

filter::Image GpuBackend::process(const filter::Image& in, const filter::Pipeline& p) {
    filter::Image fail;  // empty == "fell over, use the CPU reference"
    if (in.channels != 3 || in.empty()) return fail;
    if (!makeCurrent() || !ensureTextures(in.width, in.height)) return fail;

    // Upload RGB -> RGBA8 (alpha padded; it rides through the per-channel math and
    // is dropped on readback).
    const std::size_t n = static_cast<std::size_t>(in.width) * in.height;
    std::vector<unsigned char> rgba(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        rgba[i * 4 + 0] = in.px[i * 3 + 0];
        rgba[i * 4 + 1] = in.px[i * 3 + 1];
        rgba[i * 4 + 2] = in.px[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
    unsigned cur = pool_[0];
    glBindTexture(GL_TEXTURE_2D, cur);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in.width, in.height, GL_RGBA_INTEGER,
                    GL_UNSIGNED_BYTE, rgba.data());

    for (const filter::Stage& s : p.stages) {
        const filter::StructElem& se = s.se;
        unsigned dst = 0;
        switch (s.op) {
            case filter::Op::Erode:
                dst = acquire({cur}); morph(cur, dst, se, true); break;
            case filter::Op::Dilate:
                dst = acquire({cur}); morph(cur, dst, se, false); break;
            case filter::Op::Open: {
                unsigned t = acquire({cur}); morph(cur, t, se, true);
                dst = acquire({cur, t}); morph(t, dst, se, false); break;
            }
            case filter::Op::Close: {
                unsigned t = acquire({cur}); morph(cur, t, se, false);
                dst = acquire({cur, t}); morph(t, dst, se, true); break;
            }
            case filter::Op::Gradient: {
                unsigned d = acquire({cur}); morph(cur, d, se, false);
                unsigned e = acquire({cur, d}); morph(cur, e, se, true);
                dst = acquire({cur, d, e}); combine(d, e, dst, kSub, 0); break;
            }
            case filter::Op::TopHat: {
                unsigned t = acquire({cur}); morph(cur, t, se, true);
                unsigned o = acquire({cur, t}); morph(t, o, se, false);  // open(cur)
                dst = acquire({cur, o}); combine(cur, o, dst, kSub, 0); break;
            }
            case filter::Op::BlackHat: {
                unsigned t = acquire({cur}); morph(cur, t, se, false);
                unsigned c = acquire({cur, t}); morph(t, c, se, true);   // close(cur)
                dst = acquire({cur, c}); combine(c, cur, dst, kSub, 0); break;
            }
            case filter::Op::Threshold:
                dst = acquire({cur}); combine(cur, cur, dst, kThresh, s.t); break;
        }
        if (s.invert) {
            unsigned iv = acquire({dst});
            combine(dst, dst, iv, kInvert, 0);
            dst = iv;
        }
        cur = dst;
    }

    // Read the final texture back through an FBO (GLES has no glGetTexImage).
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cur, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return fail;
    }
    std::vector<unsigned char> out(n * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, in.width, in.height, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR) return fail;

    filter::Image result(in.width, in.height, 3);
    for (std::size_t i = 0; i < n; ++i) {
        result.px[i * 3 + 0] = out[i * 4 + 0];
        result.px[i * 3 + 1] = out[i * 4 + 1];
        result.px[i * 3 + 2] = out[i * 4 + 2];
    }
    return result;
}

}  // namespace villen
