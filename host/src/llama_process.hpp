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
// new -m. This MVP spawns one model; switch/-ngl tuning land with the admin
// console (§9, step 6).
#pragma once

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

    bool ready() const { return ready_; }     // /health returned 200 (model up)
    bool running() const { return pid_ > 0; }
    int port() const { return cfg_.port; }
    const std::string& lastError() const { return lastError_; }

 private:
    LlamaSpawnConfig cfg_;
    pid_t pid_ = -1;
    bool ready_ = false;
    std::uint64_t nextSpawnMs_ = 0;   // backoff gate after a death
    std::uint64_t nextHealthMs_ = 0;  // throttle for health probes
    std::string lastError_;

    void spawn(std::uint64_t nowMs);
    void reapIfExited(std::uint64_t nowMs);
    bool probeHealth();  // bounded non-blocking GET /health; true once 200 seen
};

}  // namespace villen::chat
