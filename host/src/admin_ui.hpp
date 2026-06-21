// Villen host — the in-process Dear ImGui admin shell (DESIGN §2, admin-shell).
//
// This is NOT a client of the server: it *is* the server, with a face. It reads
// and mutates the Host (its active engine + Room) directly, on the same thread,
// with no socket or IPC between them — privilege is structural, not networked
// (DESIGN §9.4). The loop here pumps ws.poll() and the ImGui frame together
// (DESIGN §5). It draws the shared chrome (roster, join QR, connection count) and
// hosts the active engine's own drawAdmin() body inside it (admin-shell §8).
#pragma once

#include <csignal>

namespace villen {
class Host;
namespace net { class WsServer; }
}  // namespace villen

namespace villen::admin {

// Open the admin window and run the single render+network loop until the window
// is closed or *running becomes 0. Returns false immediately if no display / SDL
// video backend is available, so the caller can fall back to a headless loop.
//
// For automated verification only: if screenshotPath is set, the loop runs for
// screenshotDelayMs, writes a PPM screenshot, and exits.
bool runAdminLoop(Host& host, net::WsServer& ws,
                  const volatile std::sig_atomic_t* running,
                  int screenshotDelayMs = -1,
                  const char* screenshotPath = nullptr);

}  // namespace villen::admin
