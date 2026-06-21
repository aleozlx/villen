// Villen host — entry point and the single main loop (DESIGN §5).
//
// One thread. Each iteration drains the network (ws.poll -> the active engine
// mutates its state) and ticks the active engine; with a display it instead runs
// the in-process ImGui admin loop, which pumps the same poll on the same thread.
// No locks, no worker threads (DESIGN §5).
//
// The host carries several engines as factories and runs ONE at a time
// (DESIGN-game-framework §1). With a display it opens the admin-shell launcher to
// pick an engine; `--engine NAME` (or headless, which has no launcher) boots
// straight into one instead (kiosk / CI).
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "engines/chat/chat_engine.hpp"
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

// SIGUSR1 = "switch to the next model" — a headless operator control for chat (the
// Game-Mode operator uses the admin combo instead). Handled in the main loop, not
// here, so it stays async-signal-safe (just sets a flag).
volatile std::sig_atomic_t g_cycleModel = 0;
void onCycleModel(int) { g_cycleModel = 1; }

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
    villen::ChatBackendConfig chatCfg;  // --engine chat backend (DESIGN-chat §3.A)
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
        else if (std::strcmp(argv[i], "--llama-url") == 0 && i + 1 < argc) {
            // HOST:PORT of a running (or stubbed) llama-server for --engine chat.
            std::string url = argv[++i];
            auto colon = url.rfind(':');
            if (colon != std::string::npos) {
                chatCfg.llamaHost = url.substr(0, colon);
                chatCfg.llamaPort = std::atoi(url.c_str() + colon + 1);
            }
        } else if (std::strcmp(argv[i], "--chat-stub") == 0) {
            chatCfg.stub = true;  // in-host echo generator, no inference (dev/CI)
        } else if (std::strcmp(argv[i], "--llama-bin") == 0 && i + 1 < argc) {
            chatCfg.llamaBin = argv[++i];  // spawn & manage this llama-server (§3.A)
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            chatCfg.model = argv[++i];     // -m GGUF for the spawned server (§11)
        } else if (std::strcmp(argv[i], "--models-dir") == 0 && i + 1 < argc) {
            chatCfg.modelsDir = argv[++i]; // scan for switchable GGUFs (§5)
        } else if (std::strcmp(argv[i], "--llama-ngl") == 0 && i + 1 < argc) {
            chatCfg.ngl = std::atoi(argv[++i]);       // GPU layers (§6)
        } else if (std::strcmp(argv[i], "--llama-parallel") == 0 && i + 1 < argc) {
            chatCfg.parallel = std::atoi(argv[++i]);  // concurrent slots (§8)
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
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
    engines.push_back(std::make_unique<villen::ChatFactory>(chatCfg));

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
    std::signal(SIGUSR1, onCycleModel);  // `kill -USR1` cycles the chat model (headless)

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
    std::vector<int> pollFds;  // engine fds folded into the wait set each iteration
    while (g_running) {
        if (g_cycleModel) {
            g_cycleModel = 0;
            // Chat-specific operator action; other engines have no model to cycle.
            if (auto* chat = dynamic_cast<villen::ChatEngine*>(host.active()))
                chat->cycleModel();
        }
        // Fold the active engine's fds (a streaming inference socket) into poll so
        // an inbound token ends the 100ms block at once and the tick drains it —
        // without this the loop only checks the socket on the next timeout.
        pollFds.clear();
        host.collectPollFds(pollFds);
        ws.poll(100, pollFds);
        host.tick(nowMs());
    }

    std::printf("\nvillen: shutting down\n");
    return 0;
}
