// Villen host — entry point and the single main loop (DESIGN §5).
//
// One thread. Each iteration drains the network (ws.poll -> the active engine
// mutates its state) and ticks the active engine; with a display it instead runs
// the in-process ImGui admin loop, which pumps the same poll on the same thread.
// No locks, no worker threads (DESIGN §5).
//
// The host carries several engines as factories and runs ONE at a time
// (DESIGN-game-framework §1). With `--engine NAME` it boots straight into that
// engine (kiosk / headless / CI); the launcher UI lands in a follow-up step.
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "engines/chess/chess_engine.hpp"
#include "host.hpp"
#include "net_util.hpp"
#include "ws_server.hpp"
#ifdef VILLEN_ADMIN_UI
#include "admin_ui.hpp"
#endif

namespace {
volatile std::sig_atomic_t g_running = 1;
void onSignal(int) { g_running = 0; }

std::uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 9002;
    bool headless = false;
    int screenshotDelayMs = -1;
    const char* screenshotPath = nullptr;
    const char* engineName = nullptr;  // --engine: boot straight into this one
#ifdef VILLEN_DEFAULT_CLIENT_DIR
    std::string clientDir = VILLEN_DEFAULT_CLIENT_DIR;
#else
    std::string clientDir = "client";
#endif
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--client-dir") == 0 && i + 1 < argc)
            clientDir = argv[++i];
        else if (std::strcmp(argv[i], "--engine") == 0 && i + 1 < argc)
            engineName = argv[++i];
        else if (std::strcmp(argv[i], "--headless") == 0)
            headless = true;
        else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshotPath = argv[++i];
            screenshotDelayMs = 2500;  // settle, let a test client connect/move
        }
    }

    villen::net::WsServer ws;
    ws.setStaticRoot(clientDir);
    if (!ws.listen(port)) {
        std::fprintf(stderr, "villen: failed to bind port %u\n", port);
        return 1;
    }

    // Register the engines this binary carries. chess is the only one today; the
    // launcher will list whatever is here (DESIGN-admin-shell §2).
    std::vector<std::unique_ptr<villen::IEngineFactory>> engines;
    engines.push_back(std::make_unique<villen::ChessFactory>());

    villen::Host host(ws, std::move(engines));

    // Resolve --engine to an index (default: the first engine).
    std::size_t startIndex = 0;
    if (engineName) {
        bool found = false;
        for (std::size_t i = 0; i < host.engineCount(); ++i)
            if (std::strcmp(host.engineName(i), engineName) == 0) {
                startIndex = i;
                found = true;
                break;
            }
        if (!found)
            std::fprintf(stderr, "villen: unknown --engine '%s', using '%s'\n",
                         engineName, host.engineName(0));
    }
    // The launcher window is the default only with a display and no --engine:
    // there the operator picks an engine (DESIGN-admin-shell §4). With --engine
    // (kiosk), or headless (no launcher to pick from), boot straight into the
    // chosen engine instead.
    bool wantLauncher = false;
#ifdef VILLEN_ADMIN_UI
    bool hasDisplay = std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY");
    wantLauncher = !headless && hasDisplay && !engineName;
#endif
    if (!wantLauncher) host.startEngine(startIndex);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::printf("Villen server listening — players open in a browser:\n");
    auto ips = villen::net::localIpv4Addresses();
    if (ips.empty()) std::printf("  http://<this-host>:%u\n", port);
    for (const auto& ip : ips) std::printf("  http://%s:%u\n", ip.c_str(), port);
    std::printf("(serving client from %s; Ctrl-C to stop)\n", clientDir.c_str());
    std::fflush(stdout);

#ifdef VILLEN_ADMIN_UI
    // Only attempt the admin window when a display server is actually present
    // (the Deck in Game Mode has one); otherwise SDL would stall probing for one
    // on a headless box. The window loop pumps ws.poll() + host.tick() itself.
    if (!headless && hasDisplay &&
        villen::admin::runAdminLoop(host, ws, &g_running, screenshotDelayMs,
                                    screenshotPath)) {
        std::printf("\nvillen: shutting down\n");
        return 0;
    }
    std::printf("villen: running headless (no admin window)\n");
    std::fflush(stdout);
#else
    (void)screenshotDelayMs;
    (void)screenshotPath;
#endif

    // Headless (or the admin window was unavailable): there is no launcher, so an
    // engine must be running. Start the default if nothing is active yet.
    if (!host.running()) host.startEngine(startIndex);
    while (g_running) {
        ws.poll(100);
        host.tick(nowMs());
    }

    std::printf("\nvillen: shutting down\n");
    return 0;
}
