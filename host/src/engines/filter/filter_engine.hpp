// Villen — FilterEngine: real-time morphology on the player edge (DESIGN-filter).
//
// The second engine on the IEngine contract, and the one that proves the slot is
// game-agnostic (DESIGN-filter §2): no turns, no seats, no shared state, no
// broadcast — each connection is a private camera *feed*. A browser streams
// downscaled JPEG frames in (onBinary); each tick the engine decodes, runs the
// operator-tuned morphology pipeline (the pure villen::filter reference, or later
// the APU), re-encodes, and answers that *one* connection (Room::sendBinary). The
// raw camera is never broadcast, never stored, never leaves the host (§10.1).
//
// Authority is the chess invariant carried over (§7): the server owns the
// pipeline. A client may request a preset; the operator (drawAdmin, §8) is the
// real authority. The pipeline is plain data (villen::filter::Pipeline), so the
// admin edits it live and every feed reflects it on its next frame.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "engine.hpp"
#include "villen/filter/pipeline.hpp"

#ifdef VILLEN_FILTER_GPU
#include <memory>

#include "gpu_backend.hpp"
#endif

namespace villen {

class FilterEngine : public IEngine {
 public:
    FilterEngine();  // probes the GPU backend; logs the renderer (§4.1)

    SeatRoster seats() const override { return {}; }  // no seats: a feed owns no seat (§7)

    void onJoin(Room&, ConnId, SeatId) override;   // push the current filterConfig
    void onLeave(Room&, ConnId, SeatId) override;  // drop the feed's pending frame
    void onMessage(Room&, ConnId, SeatId, std::string_view) override;  // control JSON
    void onBinary(Room&, ConnId, SeatId, std::string_view) override;   // a camera frame
    void onTick(Room&, std::uint64_t nowMs) override;  // decode->process->encode->reply
    std::string statusLine() const override;
    void drawAdmin() override;
    void reset() override;  // restore the default pipeline

 private:
    // The latest inbound frame for one connection, plus its rolling stats. Frames
    // are lossy by design (§5.3): a newer frame overwrites an unprocessed older
    // one, so the single loop never falls behind a fast camera.
    struct Feed {
        std::string pending;        // newest undecoded inbound frame (8B hdr + JPEG)
        bool hasPending = false;
        std::uint32_t lastSeq = 0;  // seq of the most recent inbound frame

        // Per-feed stats for the admin console (§8). Times are last-frame ms.
        std::uint64_t framesIn = 0, framesOut = 0, dropped = 0;
        std::size_t lastInBytes = 0, lastOutBytes = 0;
        double decodeMs = 0, processMs = 0, encodeMs = 0;
        std::uint64_t lastFrameMs = 0;  // wall time of the last processed frame
    };

    // Process one feed's pending frame end to end and reply to `id`.
    void processFeed(Room&, ConnId id, Feed&, std::uint64_t nowMs);
    // Run the current pipeline on a frame: the GPU backend when it is real APU
    // hardware, else the CPU reference (degrade, don't fail, §4.1).
    filter::Image runPipeline(const filter::Image& in);
    const char* backendLabel() const;  // for statusLine / drawAdmin (§8)
    // The server-authoritative filterConfig text (§5.2) for the current pipeline.
    std::string configMsg() const;
    // Re-push the config to every feed after an operator/preset edit (§8). Uses
    // the attached Room (room_), valid for the engine's whole life.
    void pushConfig();

    filter::Pipeline pipeline_ = defaultPipeline();
    std::unordered_map<ConnId, Feed> feeds_;

    // Capture/encode parameters the client is told to honour (§5.2).
    int outW_ = 320, outH_ = 240, quality_ = 70;

#ifdef VILLEN_FILTER_GPU
    std::unique_ptr<GpuBackend> gpu_;  // null when no render node / EGL (§4.1)
#endif

    static filter::Pipeline defaultPipeline();  // gradient, disk r2 (§5.2 example)
};

class FilterFactory : public IEngineFactory {
 public:
    std::unique_ptr<IEngine> create() override {
        return std::make_unique<FilterEngine>();
    }
    const char* name() const override { return "filter"; }
    // "" = the host's configured client dir; the filter view is served as
    // /filter.html from that same static root (§9) until per-engine client
    // routing lands.
    const char* clientDir() const override { return ""; }
};

}  // namespace villen
