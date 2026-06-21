#include "llama_client.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace villen::chat {

LlamaClient::LlamaClient(std::string host, int port)
    : host_(std::move(host)), port_(port) {}

LlamaClient::~LlamaClient() {
    for (auto& r : reqs_)
        if (r.fd >= 0) ::close(r.fd);
}

LlamaClient::ReqId LlamaClient::start(const std::string& body, StreamSink sink) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port_));
    // The backend is always a loopback llama-server (one we spawned or were
    // pointed at via --llama-url), so a numeric IPv4 is expected; "localhost" is
    // accepted as a convenience. We deliberately don't use getaddrinfo — DNS
    // resolution blocks, and this client must never block the main loop.
    const char* hostNumeric = (host_ == "localhost") ? "127.0.0.1" : host_.c_str();
    if (::inet_pton(AF_INET, hostNumeric, &addr.sin_addr) != 1) {
        ::close(fd);
        return 0;
    }
    // Non-blocking connect: EINPROGRESS is expected and resolved in pump() via
    // SO_ERROR; a refused/unreachable backend surfaces there as backend_down.
    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    // Connection: close so the server ends the stream with a clean EOF. Accept the
    // SSE content type. The body is the caller's JSON (messages + params + stream).
    std::string req = "POST /v1/chat/completions HTTP/1.1\r\n";
    req += "Host: " + host_ + ":" + std::to_string(port_) + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Accept: text/event-stream\r\n";
    req += "Connection: close\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    req += body;

    Req r;
    r.id = nextId_++;
    r.fd = fd;
    r.outbuf = std::move(req);
    r.sink = std::move(sink);
    reqs_.push_back(std::move(r));
    return reqs_.back().id;
}

void LlamaClient::cancel(ReqId id) {
    for (auto it = reqs_.begin(); it != reqs_.end(); ++it) {
        if (it->id == id) {
            if (it->fd >= 0) ::close(it->fd);
            reqs_.erase(it);
            return;
        }
    }
}

void LlamaClient::pump() {
    if (reqs_.empty()) return;

    std::vector<pollfd> pfds;
    pfds.reserve(reqs_.size());
    for (auto& r : reqs_) {
        short ev = POLLIN;
        if (!r.connected || !r.outbuf.empty()) ev |= POLLOUT;
        pfds.push_back({r.fd, ev, 0});
    }
    if (::poll(pfds.data(), pfds.size(), 0) < 0) return;  // EINTR etc.: try later

    for (std::size_t i = 0; i < reqs_.size(); ++i) {
        Req& r = reqs_[i];
        const short re = pfds[i].revents;
        if (re & (POLLERR | POLLNVAL)) {
            fail(r, "backend_down");
            continue;
        }
        if (!r.connected && (re & (POLLOUT | POLLHUP))) {
            int err = 0;
            socklen_t len = sizeof(err);
            ::getsockopt(r.fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0) {
                fail(r, "backend_down");
                continue;
            }
            r.connected = true;
        }
        if (r.connected && !r.outbuf.empty() && (re & POLLOUT)) serviceWrite(r);
        // serviceWrite may have failed the request this tick; don't then read a
        // socket we've already given up on.
        if (!r.finished && r.connected && (re & (POLLIN | POLLHUP))) serviceRead(r);
    }

    // Reap finished/failed requests.
    for (auto it = reqs_.begin(); it != reqs_.end();) {
        if (it->finished) {
            if (it->fd >= 0) ::close(it->fd);
            it = reqs_.erase(it);
        } else {
            ++it;
        }
    }
}

void LlamaClient::collectFds(std::vector<int>& out) const {
    for (const Req& r : reqs_)
        if (r.fd >= 0) out.push_back(r.fd);
}

void LlamaClient::serviceWrite(Req& r) {
    while (!r.outbuf.empty()) {
        ssize_t n = ::send(r.fd, r.outbuf.data(), r.outbuf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            r.outbuf.erase(0, static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;  // interrupted by a signal before sending; retry, don't fail
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;  // socket buffer full; finish next pump
        } else {
            fail(r, "backend_down");
            return;
        }
    }
}

void LlamaClient::serviceRead(Req& r) {
    char buf[8192];
    for (;;) {
        ssize_t n = ::recv(r.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            for (const auto& item : r.parser.feed(std::string_view(buf, static_cast<std::size_t>(n)))) {
                handleItem(r, item);
                if (r.finished) return;
            }
        } else if (n == 0) {  // EOF: server closed the stream
            for (const auto& item : r.parser.end()) {
                handleItem(r, item);
                if (r.finished) return;
            }
            if (!r.finished) fail(r, "backend_down");  // closed before a clean end
            return;
        } else if (n < 0 && errno == EINTR) {
            continue;  // interrupted by a signal; retry the read, don't fail
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  // drained for now
        } else {
            fail(r, "backend_down");
            return;
        }
    }
}

void LlamaClient::handleItem(Req& r, const SseParser::Item& it) {
    switch (it.kind) {
        case SseParser::Kind::Headers:
            break;  // a non-200 arrives as a following Error item
        case SseParser::Kind::Data: {
            json j = json::parse(it.data, nullptr, /*allow_exceptions=*/false);
            if (j.is_discarded() || !j.is_object()) return;
            auto ch = j.find("choices");
            if (ch == j.end() || !ch->is_array() || ch->empty()) return;
            const json& c0 = (*ch)[0];
            // choices[0] from an untrusted backend may not be an object (null,
            // string, …) — guard before find(), the same type-check-before-access
            // discipline as ChatEngine::strField (defense in depth).
            if (!c0.is_object()) return;
            auto d = c0.find("delta");
            if (d != c0.end() && d->is_object()) {
                auto cont = d->find("content");
                if (cont != d->end() && cont->is_string()) {
                    std::string s = cont->get<std::string>();
                    if (!s.empty()) {
                        if (r.sink.onDelta) r.sink.onDelta(s);
                        ++r.tokens;  // ~one token per non-empty delta
                    }
                }
            }
            auto fr = c0.find("finish_reason");
            if (fr != c0.end() && fr->is_string())
                r.stopReason = (fr->get<std::string>() == "length") ? "length" : "eos";
            break;
        }
        case SseParser::Kind::Done:
            if (!r.finished) {
                if (r.sink.onDone) r.sink.onDone(r.stopReason, r.tokens);
                r.finished = true;
            }
            break;
        case SseParser::Kind::Error:
            fail(r, it.data.empty() ? "backend_down" : it.data);
            break;
    }
}

void LlamaClient::fail(Req& r, const std::string& reason) {
    if (!r.finished) {
        if (r.sink.onError) r.sink.onError(reason);
        r.finished = true;
    }
}

}  // namespace villen::chat
