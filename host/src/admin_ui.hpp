// Villen host — the in-process Dear ImGui admin UI (DESIGN §2, step 7).
//
// This is NOT a client of the server: it *is* the server, with a face. It reads
// and mutates GameServer state directly, on the same thread, with no socket or
// IPC between them — privilege is structural (in-process), not networked (§9.4).
// The loop here pumps ws.poll() and the ImGui frame together (§5).
#pragma once

#include <csignal>

namespace villen {
class GameServer;
namespace net { class WsServer; }
}  // namespace villen

namespace villen::admin {

// Open the admin window and run the single render+network loop until the window
// is closed or *running becomes 0. Returns false immediately if no display /
// SDL video backend is available, so the caller can fall back to a headless
// server loop.
//
// For automated verification only: if screenshotPath is set, the loop runs for
// screenshotDelayMs, writes a PPM screenshot, and exits.
bool runAdminLoop(GameServer& game, net::WsServer& ws,
                  const volatile std::sig_atomic_t* running,
                  int screenshotDelayMs = -1,
                  const char* screenshotPath = nullptr);

}  // namespace villen::admin
