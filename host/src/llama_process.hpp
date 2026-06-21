// Villen host — managed llama-server child process (DESIGN-chat §3.A/§5).
//
// chat's "blocking" work lives entirely in another process: the host spawns
// `llama-server` (OpenAI-compatible), waits for it to come up, and restarts it if
// it dies — so a model OOM or a llama.cpp bug kills the child, not the appliance
// (crash isolation, §3.A). The host itself only ever does non-blocking
// byte-shuffling (LlamaClient); this class owns the child's lifecycle, driven
// from the single main loop (tick), no thread (§5).
//
// One model resident at a time (§5): switching models = restart the child with a
// new -m. The admin console (§9, step 6) drives this through switchModel() /
// restart() / stop(), and reads pid()/ngl()/residentKb()/switchLatencyMs() for
// the health panel.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace villen::chat {

struct LlamaSpawnConfig {
    std::string bin;            // path to the llama-server executable
    std::string model;          // -m <model.gguf> (operator-supplied, §11)
    std::string host = "127.0.0.1";
    int port = 8080;
    int ngl = 99;               // -ngl: GPU layers (offload all; tuned later §6)
    int parallel = 2;           // --parallel N: concurrent slots (§8)
    int ctxSize = 0;            // -c N: context tokens (0 = llama-server default)
    std::vector<std::string> extraArgs;
};

class LlamaProcess {
 public:
    explicit LlamaProcess(LlamaSpawnConfig cfg);
    ~LlamaProcess();  // terminate the child
    LlamaProcess(const LlamaProcess&) = delete;
    LlamaProcess& operator=(const LlamaProcess&) = delete;

    // Per-tick lifecycle (non-blocking): spawn if down, reap if exited (and
    // restart with backoff), probe /health to flip ready(). Call from onTick.
    void tick(std::uint64_t nowMs);

    // --- operator controls (admin console §9, step 6) ------------------------
    // All non-blocking and idiomatic to the one loop: each only SIGTERMs the
    // child and flips state; the next tick()s reap (WNOHANG) and respawn. So the
    // admin UI never stalls the loop waiting on a model unload/reload (§5).

    // Load a different model: set -m and restart the child (one model at a
    // time, §5). ready() goes false until the new model passes /health.
    void switchModel(std::string modelPath, std::uint64_t nowMs);
    // Restart with the *same* model — operator "Restart" (a wedged server, an
    // -ngl/context change that needs a fresh process).
    void restart(std::uint64_t nowMs);
    // Unload: terminate the child and keep it down (frees the weights' memory)
    // until the next switchModel()/restart(). The §9 "Unload" control.
    void stop(std::uint64_t nowMs);
    // -c context tokens for the *next* (re)spawn — the console sets this from the
    // operator's context-window value; it takes effect on switchModel()/restart().
    void setContextSize(int n) { cfg_.ctxSize = n; }

    bool ready() const { return ready_; }     // /health returned 200 (model up)
    bool running() const { return pid_ > 0; }
    bool paused() const { return paused_; }    // stop()ped, awaiting a load
    bool switching() const { return switchStartMs_ != 0; }  // reload in flight
    std::uint64_t switchLatencyMs() const { return lastSwitchMs_; }  // last reload
    pid_t pid() const { return pid_; }
    int port() const { return cfg_.port; }
    int ngl() const { return cfg_.ngl; }
    int parallel() const { return cfg_.parallel; }
    int ctxSize() const { return cfg_.ctxSize; }
    const std::string& model() const { return cfg_.model; }
    const std::string& lastError() const { return lastError_; }
    std::size_t residentKb() const;  // child RSS from /proc; 0 if unavailable

 private:
    LlamaSpawnConfig cfg_;
    pid_t pid_ = -1;
    bool ready_ = false;
    bool paused_ = false;             // stop()ped: don't respawn until told to
    bool restarting_ = false;         // the next reap is intentional, not a crash
    std::uint64_t nextSpawnMs_ = 0;   // backoff gate after a death
    std::uint64_t nextHealthMs_ = 0;  // throttle for health probes
    std::uint64_t switchStartMs_ = 0; // reload timer start; 0 = not switching
    std::uint64_t lastSwitchMs_ = 0;  // duration of the last completed reload
    std::string lastError_;

    void spawn(std::uint64_t nowMs);
    void reapIfExited(std::uint64_t nowMs);
    bool probeHealth();  // bounded non-blocking GET /health; true once 200 seen
};

}  // namespace villen::chat
