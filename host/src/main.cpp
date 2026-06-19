// Villen host — entry point and the single main loop (DESIGN §5).
//
// One thread. Each iteration drains the network (ws.poll -> mutate session
// state) and, once the admin UI lands in step 7, renders an ImGui frame from the
// same state. No locks, no worker threads: a chess server's throughput makes a
// cooperative poll trivially sufficient.
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "net_util.hpp"
#include "session.hpp"
#include "ws_server.hpp"

namespace {
volatile std::sig_atomic_t g_running = 1;
void onSignal(int) { g_running = 0; }
}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 9002;
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
    }

    villen::net::WsServer ws;
    ws.setStaticRoot(clientDir);
    if (!ws.listen(port)) {
        std::fprintf(stderr, "villen: failed to bind port %u\n", port);
        return 1;
    }

    villen::GameServer game(ws);
    ws.setCallbacks({
        [&](auto id) { game.onOpen(id); },
        [&](auto id, std::string_view msg) { game.onMessage(id, msg); },
        [&](auto id) { game.onClose(id); },
    });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::printf("Villen server listening — players open in a browser:\n");
    auto ips = villen::net::localIpv4Addresses();
    if (ips.empty()) std::printf("  http://<this-host>:%u\n", port);
    for (const auto& ip : ips) std::printf("  http://%s:%u\n", ip.c_str(), port);
    std::printf("(serving client from %s; Ctrl-C to stop)\n", clientDir.c_str());
    std::fflush(stdout);

    while (g_running) ws.poll(100);

    std::printf("\nvillen: shutting down\n");
    return 0;
}
