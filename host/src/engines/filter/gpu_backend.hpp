// Villen — the filter GPU backend: surfaceless EGL + GL-compute morphology
// (DESIGN-filter §4). The *acceleration* of the pure CPU reference, not its
// definition (§2): it must produce byte-identical output for integer operators
// (§4.3), which is how it stays verifiable rather than trusted.
//
// It owns its OWN headless GPU context — surfaceless EGL on the DRM render node,
// independent of the admin SDL2/GL window — so filter processes frames identically
// whether the admin window is up (Game Mode) or not (SSH), the host's "serves
// players headless" property (§4.1). Two contexts, one thread: process() makes the
// EGL context current around its dispatches; the admin loop makes the SDL context
// current for its frame (§6).
//
// The header deliberately includes no EGL/GL types (opaque void*/unsigned), so the
// engine can include it without dragging GL into every TU.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "villen/filter/pipeline.hpp"

namespace villen {

class GpuBackend {
 public:
    // Create a surfaceless EGL + GLES-3.1 compute context. Returns nullptr (and
    // sets *why) when there is no render node / EGL / compute support — the caller
    // then runs the CPU reference instead (degrade, don't fail, §4.1/§12).
    static std::unique_ptr<GpuBackend> tryCreate(std::string* why = nullptr);

    ~GpuBackend();
    GpuBackend(const GpuBackend&) = delete;
    GpuBackend& operator=(const GpuBackend&) = delete;

    const std::string& renderer() const { return renderer_; }
    // True when the context is a software rasteriser (llvmpipe / swrast): the APU
    // thesis silently failed, so the caller should warn and prefer the CPU
    // reference (§4.1).
    bool software() const { return software_; }

    // Run the pipeline on a PerChannel RGB image (the morphology the demo uses).
    // The EGL context is made current around the dispatches. Returns an empty
    // Image on any GL error so the caller can fall back to the CPU reference.
    filter::Image process(const filter::Image& in, const filter::Pipeline& p);

 private:
    GpuBackend() = default;

    bool init(std::string* why);
    bool makeCurrent();
    bool ensureTextures(int w, int h);   // (re)allocate the rgba8ui work pool
    void destroyTextures();
    unsigned acquire(std::initializer_list<unsigned> busy);  // a pool tex != busy
    void morph(unsigned src, unsigned dst, const filter::StructElem&, bool erode);
    void combine(unsigned a, unsigned b, unsigned dst, int mode, int t);

    // EGL handles (void* to keep EGL out of the header).
    void* dpy_ = nullptr;   // EGLDisplay
    void* ctx_ = nullptr;   // EGLContext

    unsigned progMorph_ = 0, progCombine_ = 0;  // GLuint compute programs
    int uMorphRadius_ = -1, uMorphShape_ = -1, uMorphErode_ = -1;
    int uCombMode_ = -1, uCombT_ = -1;

    std::vector<unsigned> pool_;  // rgba8ui work textures, size w_×h_
    unsigned fbo_ = 0;            // for integer-texture readback
    int w_ = 0, h_ = 0;

    std::string renderer_;
    bool software_ = false;
};

}  // namespace villen
