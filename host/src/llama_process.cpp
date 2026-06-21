#include "llama_process.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace villen::chat {

namespace {
constexpr std::uint64_t kRespawnBackoffMs = 1000;      // after a crash, wait before retry
constexpr std::uint64_t kHealthIntervalMs = 300;       // probe cadence during startup
constexpr std::uint64_t kHealthProbeTimeoutMs = 2000;  // abort a single wedged probe
}  // namespace

LlamaProcess::LlamaProcess(LlamaSpawnConfig cfg) : cfg_(std::move(cfg)) {}

LlamaProcess::~LlamaProcess() {
    if (healthFd_ >= 0) {
        ::close(healthFd_);  // never leak the in-flight probe socket
    }
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        // Reap so we don't leave a zombie; SIGTERM is enough for llama-server.
        // Loop on EINTR so a signal interrupting waitpid mid-shutdown can't leave
        // the child unreaped.
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            // interrupted before the child was reaped — retry
        }
    }
}

void LlamaProcess::tick(std::uint64_t nowMs) {
    reapIfExited(nowMs);
    if (pid_ <= 0) {
        // paused_ holds the child down after an operator Unload (§9) until a
        // switchModel()/restart() asks for it back — don't auto-respawn then.
        if (!paused_ && nowMs >= nextSpawnMs_) {
            spawn(nowMs);
        }
        return;
    }
    if (!ready_) {
        advanceHealthProbe(nowMs);  // non-blocking GET /health, spread across ticks
    }
}

void LlamaProcess::switchModel(std::string modelPath, std::uint64_t nowMs) {
    cfg_.model = std::move(modelPath);
    restart(nowMs);
}

void LlamaProcess::restart(std::uint64_t nowMs) {
    // Ask the child to exit; the WNOHANG reap in tick() collects it, then a fresh
    // spawn picks up cfg_ (a new -m, -c, …). Non-blocking: we never waitpid() here.
    // Only arm restarting_ when there's actually a child to reap — otherwise the
    // flag would dangle and mask the *next* genuine crash as "intentional".
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        restarting_ = true;  // so reapIfExited doesn't log this as a crash
    } else {
        nextSpawnMs_ = nowMs;  // already down — spawn the new model promptly
    }
    paused_ = false;
    ready_ = false;
    resetHealthProbe(nowMs);  // any in-flight probe targets the dying server
    switchStartMs_ = nowMs;   // start the unload→reload latency timer
    lastError_ = "reloading";
}

void LlamaProcess::stop(std::uint64_t nowMs) {
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        restarting_ = true;  // an intentional exit, not a crash
    }
    paused_ = true;
    ready_ = false;
    resetHealthProbe(nowMs);
    switchStartMs_ = 0;  // not reloading — staying down
    lastError_ = "unloaded";
}

void LlamaProcess::spawn(std::uint64_t nowMs) {
    // Build argv: llama-server -m <model> --host H --port P -ngl N --parallel N.
    std::vector<std::string> args = {cfg_.bin};
    if (!cfg_.model.empty()) {
        args.push_back("-m");
        args.push_back(cfg_.model);
    }
    args.push_back("--host");
    args.push_back(cfg_.host);
    args.push_back("--port");
    args.push_back(std::to_string(cfg_.port));
    args.push_back("-ngl");
    args.push_back(std::to_string(cfg_.ngl));
    args.push_back("--parallel");
    args.push_back(std::to_string(cfg_.parallel));
    if (cfg_.ctxSize > 0) {
        args.push_back("-c");
        args.push_back(std::to_string(cfg_.ctxSize));
    }
    for (const auto& a : cfg_.extraArgs) {
        args.push_back(a);
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        lastError_ = "fork failed";
        nextSpawnMs_ = nowMs + kRespawnBackoffMs;
        return;
    }
    if (pid == 0) {  // child
        // New process group so a stray child doesn't take signals meant for us.
        ::setpgid(0, 0);
        // Close inherited fds (the WsServer listen socket, live LlamaClient
        // sockets, …) before exec so llama-server holds none of them: otherwise
        // the child keeps the host's port bound and a host restart can't rebind.
        // Keep 0/1/2 so the child's stdout/stderr logging is still visible.
        long maxFd = ::sysconf(_SC_OPEN_MAX);
        if (maxFd < 0) {
            maxFd = 1024;
        }
        for (int fd = 3; fd < static_cast<int>(maxFd); ++fd) {
            ::close(fd);
        }
        ::execvp(argv[0], argv.data());
        _exit(127);  // exec failed
    }
    pid_ = pid;
    ready_ = false;
    nextHealthMs_ = nowMs;  // probe right away
    lastError_ = "starting";
}

void LlamaProcess::reapIfExited(std::uint64_t nowMs) {
    if (pid_ <= 0) {
        return;
    }
    int status = 0;
    pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == 0) {
        return;  // still running
    }
    if (r < 0) {
        // waitpid was interrupted (EINTR) or otherwise failed: `status` is only
        // valid for r>0, so do NOT read it or mark the child dead — it's still
        // running, retry next tick. Only ECHILD means it's already gone (reaped
        // elsewhere); clear pid_ in that case.
        if (errno == ECHILD) {
            pid_ = -1;
            ready_ = false;
            resetHealthProbe(nowMs);
        }
        return;
    }
    pid_ = -1;
    ready_ = false;
    resetHealthProbe(nowMs);  // the server we were probing is gone
    if (restarting_) {
        // An operator switch/restart/unload SIGTERMed it — not a crash. Respawn
        // promptly (no crash backoff); keep the "reloading"/"unloaded" message.
        restarting_ = false;
        nextSpawnMs_ = nowMs;
        return;
    }
    // Child exited or was signalled on its own — surface it and schedule a
    // restart (§3.A crash isolation: the appliance keeps running, the child
    // comes back). A crash mid-reload still ends the latency timer.
    switchStartMs_ = 0;
    if (WIFSIGNALED(status)) {
        lastError_ = "killed by signal " + std::to_string(WTERMSIG(status));
    } else {
        lastError_ = "exited (code " + std::to_string(WEXITSTATUS(status)) + ")";
    }
    nextSpawnMs_ = nowMs + kRespawnBackoffMs;
}

std::size_t LlamaProcess::residentKb() const {
    // Child RSS for the §9 "memory used" line. /proc/<pid>/statm field 2 is the
    // resident set in pages; ×page_size gives bytes. Linux-only (the Deck) — any
    // read failure yields 0 (the panel shows "n/a"). Cheap: a ~tens-of-bytes read.
    if (pid_ <= 0) {
        return 0;
    }
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/statm", static_cast<int>(pid_));
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        return 0;
    }
    unsigned long total = 0, resident = 0;
    int got = std::fscanf(f, "%lu %lu", &total, &resident);
    std::fclose(f);
    if (got < 2) {
        return 0;
    }
    long pageKb = ::sysconf(_SC_PAGESIZE) / 1024;
    if (pageKb <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(resident) * static_cast<std::size_t>(pageKb);
}

void LlamaProcess::collectPollFds(std::vector<int>& out) const {
    // While a probe is in flight, hand its socket to the host poll() set so the loop
    // wakes on the /health response (or a refused connection's POLLERR, which poll
    // reports on any watched fd) instead of waiting out the ~100 ms tick timeout —
    // the loop reads nothing here, it just uses the fd as a wakeup hint. Connect
    // completion on loopback is instant, so watching POLLIN is enough.
    if (healthFd_ >= 0) {
        out.push_back(healthFd_);
    }
}

void LlamaProcess::resetHealthProbe(std::uint64_t nowMs) {
    if (healthFd_ >= 0) {
        ::close(healthFd_);
        healthFd_ = -1;
    }
    probe_ = Probe::Idle;
    nextHealthMs_ = nowMs + kHealthIntervalMs;  // throttle the next attempt
}

void LlamaProcess::beginHealthProbe(std::uint64_t nowMs) {
    // Open a non-blocking socket and start connect(); the state machine in
    // advanceHealthProbe() carries it through send → read across later ticks.
    probeDeadlineMs_ = nowMs + kHealthProbeTimeoutMs;  // backstop for a wedged attempt
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        nextHealthMs_ = nowMs + kHealthIntervalMs;
        return;
    }
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(cfg_.port));
    // Numeric loopback is expected (we spawned the server); accept "localhost" as a
    // convenience, mirroring LlamaClient. Without this a non-numeric host would
    // inet_pton to 0.0.0.0 and the probe would falsely report "not ready" forever.
    const char* hostNumeric = (cfg_.host == "localhost") ? "127.0.0.1" : cfg_.host.c_str();
    if (::inet_pton(AF_INET, hostNumeric, &addr.sin_addr) != 1) {
        ::close(fd);
        nextHealthMs_ = nowMs + kHealthIntervalMs;
        return;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 &&
        errno != EINPROGRESS) {
        ::close(fd);
        nextHealthMs_ = nowMs + kHealthIntervalMs;
        return;
    }
    healthFd_ = fd;
    probe_ = Probe::Connecting;
}

void LlamaProcess::advanceHealthProbe(std::uint64_t nowMs) {
    // Fully-async GET /health (§18): each step polls with a 0 ms timeout so tick()
    // never blocks. Only runs while !ready_ (the caller's guard); once a model is up
    // we stop probing entirely.
    if (healthFd_ < 0) {
        if (nowMs >= nextHealthMs_) {
            beginHealthProbe(nowMs);  // start a fresh attempt; connect progresses next tick
        }
        return;
    }
    if (nowMs >= probeDeadlineMs_) {
        resetHealthProbe(nowMs);  // attempt wedged (half-open / no response) — retry later
        return;
    }

    if (probe_ == Probe::Connecting) {
        pollfd pf{healthFd_, POLLOUT, 0};
        if (::poll(&pf, 1, 0) <= 0 || !(pf.revents & (POLLOUT | POLLERR | POLLHUP))) {
            return;  // connect still pending — try next tick
        }
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(healthFd_, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {  // connection refused — server not accepting yet
            resetHealthProbe(nowMs);
            return;
        }
        std::string req = "GET /health HTTP/1.0\r\nHost: " + cfg_.host + "\r\n\r\n";
        if (::send(healthFd_, req.data(), req.size(), MSG_NOSIGNAL) < 0) {
            resetHealthProbe(nowMs);
            return;
        }
        probe_ = Probe::Reading;
        return;
    }

    if (probe_ == Probe::Reading) {
        pollfd pf{healthFd_, POLLIN, 0};
        if (::poll(&pf, 1, 0) <= 0 || !(pf.revents & (POLLIN | POLLERR | POLLHUP))) {
            return;  // no response yet — try next tick
        }
        char buf[128];
        ssize_t n = ::recv(healthFd_, buf, sizeof(buf) - 1, 0);
        bool ok = false;
        if (n > 0) {
            buf[n] = '\0';
            ok = std::strstr(buf, " 200") != nullptr;  // "HTTP/1.1 200 OK"
        }
        if (ok) {
            ready_ = true;
            lastError_.clear();
            if (switchStartMs_ != 0) {  // a reload just finished — record its latency
                lastSwitchMs_ = nowMs - switchStartMs_;
                switchStartMs_ = 0;
            }
        }
        resetHealthProbe(nowMs);  // close the fd; if !ok the next attempt retries
    }
}

}  // namespace villen::chat
