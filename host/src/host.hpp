// Villen — Host: the multi-engine, single-active lifecycle (DESIGN-admin-shell
// §3/§4).
//
// One binary carries several engines as factories; the Host runs exactly ONE at a
// time. The launcher state is "none active". Switching constructs the next engine
// and tears the previous one down through its base destructor — the next engine
// must start from a clean slate (admin-shell §4). No IPC: the active engine is an
// in-process object and only one ever runs (framework §6).
//
// The Host owns the ws<->engine routing: it rewires the WsServer callbacks to the
// active Room on start, and to "announce only" replies at the launcher. It also
// announces the active engine to clients so a connected page can load the right
// view (admin-shell §5). All on the one thread, no locks (DESIGN §5).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine.hpp"
#include "room.hpp"
#include "ws_server.hpp"

namespace villen {

class Host {
 public:
    Host(net::WsServer& ws, std::vector<std::unique_ptr<IEngineFactory>> engines);
    ~Host();

    // The launcher menu.
    std::size_t engineCount() const { return engines_.size(); }
    const char* engineName(std::size_t i) const { return engines_[i]->name(); }

    // Lifecycle (driven by the launcher, or once at boot in headless/kiosk).
    void startEngine(std::size_t index);  // construct + activate engine[index]
    void stopEngine();                     // tear down -> launcher (none active)

    // Per-frame, so real-time engines advance even with no input (framework §5.1).
    void tick(std::uint64_t nowMs);

    // Gather the active engine's pollable fds (if any) so the main loop can fold
    // them into ws.poll() — a streaming engine then wakes the loop on a token
    // rather than waiting out the poll timeout. No-op at the launcher.
    void collectPollFds(std::vector<int>& out);

    // State for the admin shell.
    bool running() const { return active_ != nullptr; }
    std::size_t activeIndex() const { return activeIndex_; }
    IEngine* active() const { return active_.get(); }
    Room* room() const { return room_.get(); }

 private:
    net::WsServer& ws_;
    std::vector<std::unique_ptr<IEngineFactory>> engines_;
    std::unique_ptr<IEngine> active_;  // nullptr at the launcher
    std::unique_ptr<Room> room_;       // one Room per active engine
    std::size_t activeIndex_ = static_cast<std::size_t>(-1);

    const char* activeName() const;  // factory name, or "" at the launcher
    void announce(ConnId id);        // tell one client which engine is active
    void announceAll();              // ... and every connected client
    void installCallbacks();         // route ws -> active room (or announce-only)
};

}  // namespace villen
