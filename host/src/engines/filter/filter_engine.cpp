#include "filter_engine.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include "room.hpp"
#include "villen/filter/presets.hpp"

// stb headers, declaration-only — the implementations are compiled once in
// stb_impl.cpp (DESIGN-filter §12).
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#ifdef VILLEN_ADMIN_UI
#include "imgui.h"
#endif

using json = nlohmann::json;

namespace villen {
namespace {

using clock_t = std::chrono::steady_clock;
double msBetween(clock_t::time_point a, clock_t::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Read a string field from untrusted client JSON without throwing: a value of the
// wrong JSON type (or a missing key) yields "" rather than a json::type_error that
// would crash the server (DoS) — mirrors chess_engine.cpp / envelope.cpp.
std::string strField(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// --- the 8-byte media header (§5.2): [u32 seq][u16 w][u16 h], little-endian ---
std::uint32_t readU32LE(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return u[0] | (u[1] << 8) | (u[2] << 16) | (std::uint32_t(u[3]) << 24);
}
void appendU32LE(std::string& s, std::uint32_t v) {
    s.push_back(char(v & 0xFF));
    s.push_back(char((v >> 8) & 0xFF));
    s.push_back(char((v >> 16) & 0xFF));
    s.push_back(char((v >> 24) & 0xFF));
}
void appendU16LE(std::string& s, std::uint16_t v) {
    s.push_back(char(v & 0xFF));
    s.push_back(char((v >> 8) & 0xFF));
}

// --- pipeline <-> wire names (§5.2) -----------------------------------------
const char* opName(filter::Op op) {
    switch (op) {
        case filter::Op::Erode: return "erode";
        case filter::Op::Dilate: return "dilate";
        case filter::Op::Open: return "open";
        case filter::Op::Close: return "close";
        case filter::Op::Gradient: return "gradient";
        case filter::Op::TopHat: return "tophat";
        case filter::Op::BlackHat: return "blackhat";
        case filter::Op::Threshold: return "threshold";
    }
    return "erode";
}
const char* seName(filter::SE se) {
    switch (se) {
        case filter::SE::Box: return "box";
        case filter::SE::Cross: return "cross";
        case filter::SE::Disk: return "disk";
    }
    return "box";
}
const char* colorName(filter::Color c) {
    switch (c) {
        case filter::Color::PerChannel: return "perChannel";
        case filter::Color::Gray: return "gray";
        case filter::Color::Luma: return "luma";
    }
    return "perChannel";
}

// A hostile client controls the JPEG it sends, so the *decoded* dimensions are
// untrusted: a 20000x20000 frame would allocate ~1.1 GB and bury the single loop
// in morphology work (DoS). Cap decoded frames well above any real capture size
// (the server asks for 320x240, §5.2) and drop anything larger.
constexpr int kMaxFrameDim = 2048;

// Encode an Image to a JPEG byte string via stb (no stdio). Empty on failure.
std::string encodeJpeg(const filter::Image& img, int quality) {
    if (img.empty()) return {};  // a failed pipeline yields no pixels: don't feed
                                 // stb a null buffer (it would crash, not no-op)
    std::string out;
    auto sink = [](void* ctx, void* data, int size) {
        auto* s = static_cast<std::string*>(ctx);
        s->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
    };
    int ok = stbi_write_jpg_to_func(sink, &out, img.width, img.height,
                                    img.channels, img.px.data(), quality);
    if (!ok) out.clear();
    return out;
}

}  // namespace

FilterEngine::FilterEngine() {
#ifdef VILLEN_FILTER_GPU
    // Own a headless surfaceless-EGL compute context, independent of the admin
    // window (§4.1). Degrade to the CPU reference if there's no GPU.
    std::string why;
    gpu_ = GpuBackend::tryCreate(&why);
    if (gpu_ && gpu_->software()) {
        std::fprintf(stderr,
                     "filter: WARNING GPU is a software rasteriser (%s) — the APU "
                     "thesis failed; using the CPU reference (§4.1)\n",
                     gpu_->renderer().c_str());
    } else if (gpu_) {
        std::fprintf(stderr, "filter: GPU backend ready: %s\n",
                     gpu_->renderer().c_str());
    } else {
        std::fprintf(stderr, "filter: no GPU backend (%s) — CPU reference\n",
                     why.c_str());
    }
#endif
}

filter::Pipeline FilterEngine::defaultPipeline() {
    return filter::presets::gradient(2);  // the §5.2 example: gradient, disk r2
}

filter::Image FilterEngine::runPipeline(const filter::Image& in) {
#ifdef VILLEN_FILTER_GPU
    // Use the APU only when it is real hardware; a software rasteriser is slower
    // and no more correct than the reference (§4.1). PerChannel only for now (the
    // GPU kernels are per-channel; other colour modes run on the CPU reference).
    if (gpu_ && !gpu_->software() && pipeline_.color == filter::Color::PerChannel) {
        filter::Image out = gpu_->process(in, pipeline_);
        if (!out.empty()) return out;  // empty => GL error: fall through to CPU
    }
#endif
    return filter::process(in, pipeline_);
}

const char* FilterEngine::backendLabel() const {
#ifdef VILLEN_FILTER_GPU
    if (gpu_) return gpu_->software() ? "GPU (software!)" : "GPU (APU)";
#endif
    return "CPU reference";
}

std::string FilterEngine::configMsg() const {
    json arr = json::array();
    for (const filter::Stage& s : pipeline_.stages) {
        json st = {{"op", opName(s.op)}};
        if (s.op == filter::Op::Threshold) {
            st["t"] = s.t;
        } else {
            st["se"] = seName(s.se.shape);
            st["r"] = s.se.radius;
        }
        if (s.invert) st["invert"] = true;
        arr.push_back(std::move(st));
    }
    json cfg = {{"type", "filterConfig"},
                {"outW", outW_},
                {"outH", outH_},
                {"format", "jpeg"},
                {"quality", quality_},
                {"color", colorName(pipeline_.color)},
                {"pipeline", std::move(arr)}};
    return cfg.dump();
}

void FilterEngine::pushConfig() {
    if (room_) room_->broadcast(configMsg());  // shared session: every feed (§7)
}

void FilterEngine::onJoin(Room& room, ConnId id, SeatId) {
    feeds_[id];  // a feed exists from join, so stats show before the first frame
    room.send(id, configMsg());  // the joiner learns capture size + the pipeline
}

void FilterEngine::onLeave(Room&, ConnId id, SeatId) {
    feeds_.erase(id);  // frames are never retained past a feed's life (§10.1)
}

void FilterEngine::onMessage(Room&, ConnId, SeatId, std::string_view text) {
    // The only client->server control today is an optional preset request; the
    // server stays the authority (§5.2/§7). Unknown types are ignored.
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return;
    // strField never throws on an unexpected JSON type, so a malformed payload
    // (e.g. {"type":123}) is ignored, not fatal.
    if (strField(j, "type") != "requestPreset") return;

    filter::Pipeline p = filter::presets::byName(strField(j, "preset"));
    if (!p.stages.empty()) {  // honour a known preset; ignore an unknown one
        pipeline_ = std::move(p);
        pushConfig();
    }
}

void FilterEngine::onBinary(Room&, ConnId id, SeatId, std::string_view bytes) {
    // A valid frame is an 8-byte header + JPEG; anything shorter is malformed.
    // Rejecting it here also keeps assign() off a possibly-null empty-view pointer.
    if (bytes.size() < 8) return;
    // Drop-to-latest (§5.3): a new frame overwrites any unprocessed older one, so
    // a fast camera can never make the loop fall behind. Dropping is correct.
    Feed& f = feeds_[id];
    if (f.hasPending) ++f.dropped;
    f.pending.assign(bytes.data(), bytes.size());
    f.hasPending = true;
    ++f.framesIn;
    f.lastInBytes = bytes.size();
    if (bytes.size() >= 8) f.lastSeq = readU32LE(bytes.data());
}

void FilterEngine::onTick(Room& room, std::uint64_t nowMs) {
    // At most one frame per feed per tick — the per-iteration work is bounded by
    // the feed count, not by how hard any client pushes (§5.3, §6).
    for (auto& [id, feed] : feeds_)
        if (feed.hasPending) processFeed(room, id, feed, nowMs);
}

void FilterEngine::processFeed(Room& room, ConnId id, Feed& feed, std::uint64_t nowMs) {
    std::string frame = std::move(feed.pending);
    feed.pending.clear();
    feed.hasPending = false;
    if (frame.size() < 8) return;  // missing header -> drop

    std::uint32_t seq = readU32LE(frame.data());
    const auto* jpeg = reinterpret_cast<const unsigned char*>(frame.data()) + 8;
    int jlen = static_cast<int>(frame.size() - 8);

    clock_t::time_point t0 = clock_t::now();
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load_from_memory(jpeg, jlen, &w, &h, &ch, 3);
    if (!px) return;  // undecodable -> drop this frame, keep the feed alive
    // The decoded size is untrusted (§10): refuse an oversized frame before it can
    // allocate gigabytes or stall the loop, and free stb's buffer either way.
    if (w <= 0 || h <= 0 || w > kMaxFrameDim || h > kMaxFrameDim) {
        stbi_image_free(px);
        return;
    }
    clock_t::time_point t1 = clock_t::now();

    filter::Image img(w, h, 3);
    std::memcpy(img.px.data(), px, static_cast<std::size_t>(w) * h * 3);
    stbi_image_free(px);

    filter::Image out = runPipeline(img);  // APU when real, else CPU reference
    clock_t::time_point t2 = clock_t::now();

    std::string jpegOut = encodeJpeg(out, quality_);
    clock_t::time_point t3 = clock_t::now();
    if (jpegOut.empty()) return;

    // Reply: echo the source seq so the client can drop stale results and measure
    // round-trip (§5.2); advertise the actual processed dimensions.
    std::string reply;
    reply.reserve(8 + jpegOut.size());
    appendU32LE(reply, seq);
    appendU16LE(reply, static_cast<std::uint16_t>(out.width));
    appendU16LE(reply, static_cast<std::uint16_t>(out.height));
    reply.append(jpegOut);
    room.sendBinary(id, reply);  // private to this feed, never broadcast (§10.1)

    ++feed.framesOut;
    feed.lastOutBytes = jpegOut.size();
    feed.decodeMs = msBetween(t0, t1);
    feed.processMs = msBetween(t1, t2);
    feed.encodeMs = msBetween(t2, t3);
    feed.lastFrameMs = nowMs;
}

std::string FilterEngine::statusLine() const {
    std::string s = "filter (";
    s += backendLabel();
    s += ") - " + std::to_string(feeds_.size()) + " feed(s), ";
    s += std::to_string(pipeline_.stages.size()) + " stage(s)";
    return s;
}

void FilterEngine::reset() {
    pipeline_ = defaultPipeline();
    pushConfig();
}

// The operator pipeline console (§8). Only the engine's own body — the shell
// draws the chrome and join QR (admin-shell §8). ASCII-only (default ImGui font).
void FilterEngine::drawAdmin() {
#ifdef VILLEN_ADMIN_UI
    bool changed = false;

    // The operator must be able to *see* it is the real APU and not llvmpipe
    // (§4.1/§8): show the backend label and the GL_RENDERER string.
    ImGui::Text("Backend: %s", backendLabel());
#ifdef VILLEN_FILTER_GPU
    if (gpu_) {
        ImVec4 col = gpu_->software() ? ImVec4(0.95f, 0.4f, 0.4f, 1.0f)
                                      : ImVec4(0.5f, 0.86f, 0.5f, 1.0f);
        ImGui::TextColored(col, "%s", gpu_->renderer().c_str());
    }
#endif
    ImGui::TextUnformatted(statusLine().c_str());
    ImGui::Spacing();

    // Presets (§8): one-click pipelines, server-authoritative.
    ImGui::SeparatorText("Presets");
    if (ImGui::Button("Gradient")) { pipeline_ = filter::presets::gradient(2); changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Open disk")) { pipeline_ = filter::presets::openDisk(3); changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Edge + threshold")) {
        pipeline_ = filter::presets::edgeThreshold(2, 64);
        changed = true;
    }

    // Output parameters the client is told to honour (§5.2).
    ImGui::SeparatorText("Output");
    static const char* kColors[] = {"perChannel", "gray", "luma"};
    int colour = static_cast<int>(pipeline_.color);
    if (ImGui::Combo("colour", &colour, kColors, IM_ARRAYSIZE(kColors))) {
        pipeline_.color = static_cast<filter::Color>(colour);
        changed = true;
    }
    if (ImGui::SliderInt("quality", &quality_, 10, 95)) changed = true;

    // Pipeline editor (§8): ordered op list; add / remove / reorder; per-stage
    // op + SE + radius (+ threshold + invert).
    ImGui::SeparatorText("Pipeline");
    static const char* kOps[] = {"erode",    "dilate",   "open",     "close",
                                 "gradient", "tophat",   "blackhat", "threshold"};
    static const char* kShapes[] = {"box", "cross", "disk"};

    int removeAt = -1, swapWith = -1, swapFrom = -1;
    for (int i = 0; i < static_cast<int>(pipeline_.stages.size()); ++i) {
        filter::Stage& st = pipeline_.stages[i];
        ImGui::PushID(i);
        ImGui::Text("%d.", i + 1);
        ImGui::SameLine();

        ImGui::SetNextItemWidth(110);
        int op = static_cast<int>(st.op);
        if (ImGui::Combo("##op", &op, kOps, IM_ARRAYSIZE(kOps))) {
            st.op = static_cast<filter::Op>(op);
            changed = true;
        }

        if (st.op == filter::Op::Threshold) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            int t = st.t;
            if (ImGui::SliderInt("t", &t, 0, 255)) { st.t = static_cast<std::uint8_t>(t); changed = true; }
        } else {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            int shape = static_cast<int>(st.se.shape);
            if (ImGui::Combo("##se", &shape, kShapes, IM_ARRAYSIZE(kShapes))) {
                st.se.shape = static_cast<filter::SE>(shape);
                changed = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            if (ImGui::SliderInt("r", &st.se.radius, 0, 8)) changed = true;
        }

        ImGui::SameLine();
        if (ImGui::Checkbox("inv", &st.invert)) changed = true;

        ImGui::SameLine();
        if (ImGui::SmallButton("^") && i > 0) { swapFrom = i; swapWith = i - 1; }
        ImGui::SameLine();
        if (ImGui::SmallButton("v") && i + 1 < static_cast<int>(pipeline_.stages.size())) {
            swapFrom = i;
            swapWith = i + 1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = i;
        ImGui::PopID();
    }

    if (ImGui::Button("+ stage")) {
        pipeline_.stages.push_back({filter::Op::Erode, {filter::SE::Box, 1}, 128, false});
        changed = true;
    }
    if (removeAt >= 0) {
        pipeline_.stages.erase(pipeline_.stages.begin() + removeAt);
        changed = true;
    }
    if (swapWith >= 0) {
        std::swap(pipeline_.stages[swapFrom], pipeline_.stages[swapWith]);
        changed = true;
    }

    // Per-feed live stats (§8): operator sees the codec cost and frame rate.
    ImGui::SeparatorText("Feeds");
    if (ImGui::BeginTable("feeds", 7,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (const char* c : {"conn", "in", "out", "drop", "kB", "dec/proc/enc ms", ""})
            ImGui::TableSetupColumn(c);
        ImGui::TableHeadersRow();
        for (const auto& [id, f] : feeds_) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%llu", (unsigned long long)id);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%llu", (unsigned long long)f.framesIn);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%llu", (unsigned long long)f.framesOut);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%llu", (unsigned long long)f.dropped);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%zu/%zu", f.lastInBytes / 1024, f.lastOutBytes / 1024);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.1f/%.1f/%.1f", f.decodeMs, f.processMs, f.encodeMs);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset pipeline")) reset();

    if (changed) pushConfig();  // every feed reflects the edit on its next frame
#endif
}

}  // namespace villen
