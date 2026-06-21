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
#include <cstring>

namespace villen::chat {

namespace {
constexpr std::uint64_t kRespawnBackoffMs = 1000;  // after a crash, wait before retry
constexpr std::uint64_t kHealthIntervalMs = 300;   // probe cadence during startup
}  // namespace

LlamaProcess::LlamaProcess(LlamaSpawnConfig cfg) : cfg_(std::move(cfg)) {}

LlamaProcess::~LlamaProcess() {
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        // Reap so we don't leave a zombie; SIGTERM is enough for llama-server.
        int status = 0;
        ::waitpid(pid_, &status, 0);
    }
}

void LlamaProcess::setModel(std::string modelPath) {
    if (modelPath.empty() || modelPath == cfg_.model) return;
    cfg_.model = std::move(modelPath);
    ready_ = false;
    if (pid_ > 0) {
        // Signal the old child; reapIfExited() collects it non-blocking and the
        // next tick respawns with the new -m. Until then, don't probe the dying
        // server's /health (it may still answer 200 for a beat).
        ::kill(pid_, SIGTERM);
        draining_ = true;
        lastError_ = "switching model";
    } else {
        nextSpawnMs_ = 0;  // not running — spawn the new model promptly
        lastError_ = "starting";
    }
}

void LlamaProcess::tick(std::uint64_t nowMs) {
    reapIfExited(nowMs);
    if (pid_ <= 0) {
        if (nowMs >= nextSpawnMs_) spawn(nowMs);
        return;
    }
    if (draining_) return;  // old child is being killed for a switch; don't probe it
    if (!ready_ && nowMs >= nextHealthMs_) {
        nextHealthMs_ = nowMs + kHealthIntervalMs;
        if (probeHealth()) {
            ready_ = true;
            lastError_.clear();
        }
    }
}

void LlamaProcess::spawn(std::uint64_t nowMs) {
    // Build argv: llama-server -m <model> --host H --port P -ngl N --parallel N.
    std::vector<std::string> args = {cfg_.bin};
    if (!cfg_.model.empty()) { args.push_back("-m"); args.push_back(cfg_.model); }
    args.push_back("--host");      args.push_back(cfg_.host);
    args.push_back("--port");      args.push_back(std::to_string(cfg_.port));
    args.push_back("-ngl");        args.push_back(std::to_string(cfg_.ngl));
    args.push_back("--parallel");  args.push_back(std::to_string(cfg_.parallel));
    for (const auto& a : cfg_.extraArgs) args.push_back(a);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
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
        ::execvp(argv[0], argv.data());
        _exit(127);  // exec failed
    }
    pid_ = pid;
    ready_ = false;
    nextHealthMs_ = nowMs;  // probe right away
    lastError_ = "starting";
}

void LlamaProcess::reapIfExited(std::uint64_t nowMs) {
    if (pid_ <= 0) return;
    int status = 0;
    pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == 0) return;            // still running
    if (r < 0 && errno == ECHILD) {  // already reaped somehow
        pid_ = -1;
        ready_ = false;
        draining_ = false;
        return;
    }
    // Child exited or was signalled — surface it and schedule a restart (§3.A
    // crash isolation: the appliance keeps running, the child comes back). A
    // drain (deliberate kill for a model switch) isn't a crash: keep the
    // "switching" message and respawn promptly, no backoff.
    if (draining_) {
        lastError_ = "loading model";
        nextSpawnMs_ = nowMs;
    } else if (WIFSIGNALED(status)) {
        lastError_ = "killed by signal " + std::to_string(WTERMSIG(status));
        nextSpawnMs_ = nowMs + kRespawnBackoffMs;
    } else {
        lastError_ = "exited (code " + std::to_string(WEXITSTATUS(status)) + ")";
        nextSpawnMs_ = nowMs + kRespawnBackoffMs;
    }
    pid_ = -1;
    ready_ = false;
    draining_ = false;
}

bool LlamaProcess::probeHealth() {
    // Bounded, non-blocking GET /health. Brief (<~60ms worst case) and only while
    // the model is loading; once ready we stop probing. A fully-async probe is a
    // later refinement; this keeps the loop responsive without a state machine.
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(cfg_.port));
    ::inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr);

    bool ok = false;
    auto cleanup = [&]() { ::close(fd); return ok; };

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 &&
        errno != EINPROGRESS)
        return cleanup();

    pollfd pf{fd, POLLOUT, 0};
    if (::poll(&pf, 1, 30) <= 0 || !(pf.revents & POLLOUT)) return cleanup();
    int err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) return cleanup();

    std::string req = "GET /health HTTP/1.0\r\nHost: " + cfg_.host + "\r\n\r\n";
    if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) < 0) return cleanup();

    pf.events = POLLIN;
    if (::poll(&pf, 1, 30) <= 0 || !(pf.revents & POLLIN)) return cleanup();
    char buf[128];
    ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        ok = std::strstr(buf, " 200") != nullptr;  // "HTTP/1.1 200 OK"
    }
    return cleanup();
}

}  // namespace villen::chat
