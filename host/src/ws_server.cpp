#include "ws_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace villen::net {
namespace {

// --- SHA-1 (just enough for the WebSocket accept hash) ----------------------
struct Sha1 {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                          0xC3D2E1F0u};
    static std::uint32_t rol(std::uint32_t v, int b) {
        return (v << b) | (v >> (32 - b));
    }
    void block(const unsigned char* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) |
                   p[i * 4 + 3];
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<unsigned char, 20> digest(const std::string& msg) {
        std::string m = msg;
        std::uint64_t bits = static_cast<std::uint64_t>(m.size()) * 8;
        m.push_back('\x80');
        while (m.size() % 64 != 56) m.push_back('\0');
        for (int i = 7; i >= 0; --i) m.push_back(static_cast<char>(bits >> (i * 8)));
        for (std::size_t i = 0; i < m.size(); i += 64)
            block(reinterpret_cast<const unsigned char*>(m.data() + i));
        std::array<unsigned char, 20> out{};
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = h[i] >> 24;
            out[i * 4 + 1] = h[i] >> 16;
            out[i * 4 + 2] = h[i] >> 8;
            out[i * 4 + 3] = h[i];
        }
        return out;
    }
};

std::string base64(const unsigned char* data, std::size_t n) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < n; i += 3) {
        std::uint32_t v = data[i] << 16;
        if (i + 1 < n) v |= data[i + 1] << 8;
        if (i + 2 < n) v |= data[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? tbl[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? tbl[v & 63] : '=');
    }
    return out;
}

std::string acceptKey(const std::string& clientKey) {
    static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 sha;
    auto d = sha.digest(clientKey + kGuid);
    return base64(d.data(), d.size());
}

// Case-insensitive header lookup over a raw HTTP request.
std::string headerValue(const std::string& req, const std::string& name) {
    std::string lower = req;
    for (char& c : lower) c = static_cast<char>(::tolower(c));
    std::string key = name;
    for (char& c : key) c = static_cast<char>(::tolower(c));
    auto pos = lower.find(key + ":");
    if (pos == std::string::npos) return {};
    pos += key.size() + 1;
    auto end = req.find("\r\n", pos);
    std::string val = req.substr(pos, end - pos);
    std::size_t a = val.find_first_not_of(" \t");
    std::size_t b = val.find_last_not_of(" \t");
    return a == std::string::npos ? std::string{} : val.substr(a, b - a + 1);
}

constexpr int kOpText = 0x1;
constexpr int kOpBinary = 0x2;
constexpr int kOpClose = 0x8;
constexpr int kOpPing = 0x9;
constexpr int kOpPong = 0xA;

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::string contentType(const std::string& path) {
    auto ends = [&](const char* ext) {
        std::size_t n = std::strlen(ext);
        return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js")) return "text/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".json")) return "application/json";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".ico")) return "image/x-icon";
    return "application/octet-stream";
}

}  // namespace

WsServer::~WsServer() {
    for (auto& [id, c] : conns_) ::close(c.fd);
    if (listenFd_ >= 0) ::close(listenFd_);
}

bool WsServer::listen(std::uint16_t port) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;
    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::listen(listenFd_, 16) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    setNonBlocking(listenFd_);
    // Read the actually-bound port back rather than trusting the argument: with
    // port 0 the OS assigns an ephemeral one, and callers (the admin join URL, the
    // integration tests) need port() to report what was bound, not the 0 asked for.
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        port_ = ntohs(bound.sin_port);
    else
        port_ = port;
    return true;
}

void WsServer::poll(int timeoutMs, const std::vector<int>& extraReadFds) {
    if (listenFd_ < 0) return;

    std::vector<pollfd> pfds;
    pfds.push_back({listenFd_, POLLIN, 0});
    std::vector<ConnId> order;
    for (auto& [id, c] : conns_) {
        short events = POLLIN;
        if (!c.outbuf.empty()) events |= POLLOUT;
        pfds.push_back({c.fd, events, 0});
        order.push_back(id);
    }
    // Foreign fds (an engine's inference socket): watched only so their readiness
    // ends the block early. They sit after the conn fds, past `order.size()`, so
    // the per-conn loop below never touches them — the owner reads them in onTick.
    for (int fd : extraReadFds)
        if (fd >= 0) pfds.push_back({fd, POLLIN, 0});

    int n = ::poll(pfds.data(), pfds.size(), timeoutMs);
    if (n <= 0) return;

    if (pfds[0].revents & POLLIN) acceptNew();

    for (std::size_t i = 0; i < order.size(); ++i) {
        ConnId id = order[i];
        short re = pfds[i + 1].revents;
        auto it = conns_.find(id);
        if (it == conns_.end()) continue;
        if (re & (POLLERR | POLLHUP | POLLNVAL)) { drop(id); continue; }
        if (re & POLLOUT) flush(it->second);
        if (re & POLLIN) onReadable(id);
    }

    // Reap connections whose close handshake has fully drained.
    std::vector<ConnId> dead;
    for (auto& [id, c] : conns_)
        if (c.closing && c.outbuf.empty()) dead.push_back(id);
    for (ConnId id : dead) drop(id);
}

void WsServer::acceptNew() {
    for (;;) {
        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) break;  // EAGAIN: no more pending
        setNonBlocking(fd);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ConnId id = nextId_++;
        conns_[id].fd = fd;
    }
}

void WsServer::onReadable(ConnId id) {
    auto it = conns_.find(id);
    if (it == conns_.end()) return;
    Conn& c = it->second;
    char buf[4096];
    for (;;) {
        ssize_t r = ::recv(c.fd, buf, sizeof(buf), 0);
        if (r > 0) {
            c.inbuf.append(buf, static_cast<std::size_t>(r));
        } else if (r == 0) {
            drop(id);
            return;
        } else {
            break;  // EAGAIN
        }
    }

    if (!c.handshakeDone) {
        if (!tryHandshake(id)) return;
    }
    parseFrames(id);
}

bool WsServer::tryHandshake(ConnId id) {
    Conn& c = conns_[id];
    auto end = c.inbuf.find("\r\n\r\n");
    if (end == std::string::npos) return false;  // headers incomplete
    std::string req = c.inbuf.substr(0, end + 4);
    c.inbuf.erase(0, end + 4);

    std::string key = headerValue(req, "Sec-WebSocket-Key");
    if (key.empty()) {
        // Not a WebSocket upgrade: treat as a plain HTTP request and (if a
        // static root is set) serve the browser client, then close.
        serveHttp(id, req);
        return false;
    }

    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + acceptKey(key) + "\r\n\r\n";
    c.outbuf += resp;
    c.handshakeDone = true;
    flush(c);
    if (cb_.onOpen) cb_.onOpen(id);
    return true;
}

void WsServer::serveHttp(ConnId id, const std::string& request) {
    Conn& c = conns_[id];
    auto respond = [&](const std::string& status, const std::string& ctype,
                       const std::string& body) {
        c.outbuf += "HTTP/1.1 " + status + "\r\n" +
                    "Content-Type: " + ctype + "\r\n" +
                    "Content-Length: " + std::to_string(body.size()) + "\r\n" +
                    "Connection: close\r\nCache-Control: no-cache\r\n\r\n" + body;
        c.closing = true;  // reaped once the response has drained
        flush(c);
    };

    if (staticRoot_.empty()) {
        respond("426 Upgrade Required", "text/plain",
                "This endpoint speaks WebSocket only.\n");
        return;
    }

    std::string method, path;
    {
        std::istringstream ls(request);
        ls >> method >> path;
    }
    if (method != "GET") {
        respond("405 Method Not Allowed", "text/plain", "405\n");
        return;
    }
    if (auto q = path.find('?'); q != std::string::npos) path.resize(q);
    if (path.empty() || path == "/") path = "/index.html";
    else if (path.back() == '/') path += "index.html";  // directory -> its index
    if (path.find("..") != std::string::npos) {  // refuse path traversal
        respond("400 Bad Request", "text/plain", "400\n");
        return;
    }

    // A directory requested without a trailing slash (e.g. /chat) must 301 to
    // /chat/ so the browser resolves the page's relative assets (style.css,
    // chat.js) against the directory, not its parent — and so the directory isn't
    // opened as a file. Only true directories redirect; missing paths fall to 404.
    struct stat st {};
    if (::stat((staticRoot_ + path).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        c.outbuf += "HTTP/1.1 301 Moved Permanently\r\nLocation: " + path +
                    "/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        c.closing = true;
        flush(c);
        return;
    }

    std::ifstream f(staticRoot_ + path, std::ios::binary);
    if (!f) {
        respond("404 Not Found", "text/plain", "Not found: " + path + "\n");
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    respond("200 OK", contentType(path), ss.str());
}

void WsServer::parseFrames(ConnId id) {
    for (;;) {
        auto it = conns_.find(id);
        if (it == conns_.end()) return;
        Conn& c = it->second;
        const auto& b = c.inbuf;
        if (b.size() < 2) return;

        const unsigned char* p = reinterpret_cast<const unsigned char*>(b.data());
        bool fin = p[0] & 0x80;
        int opcode = p[0] & 0x0F;
        bool masked = p[1] & 0x80;
        std::uint64_t len = p[1] & 0x7F;
        std::size_t off = 2;

        if (len == 126) {
            if (b.size() < off + 2) return;
            len = (p[off] << 8) | p[off + 1];
            off += 2;
        } else if (len == 127) {
            if (b.size() < off + 8) return;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | p[off + i];
            off += 8;
        }
        // Clients MUST mask; refuse anything implausibly large (LAN/chess).
        if (!masked || len > (1u << 20)) { drop(id); return; }

        if (b.size() < off + 4 + len) return;  // frame incomplete
        const unsigned char* mask = p + off;
        off += 4;

        std::string payload;
        payload.resize(len);
        for (std::uint64_t i = 0; i < len; ++i)
            payload[i] = static_cast<char>(p[off + i] ^ mask[i & 3]);
        c.inbuf.erase(0, off + len);

        if (opcode == kOpClose) {
            queueFrame(c, kOpClose, "");
            c.closing = true;
            return;
        }
        if (opcode == kOpPing) { queueFrame(c, kOpPong, payload); continue; }
        if (opcode == kOpPong) { continue; }

        // Data frames, with continuation/fragmentation handling.
        if (opcode == 0) {
            c.fragment += payload;
        } else {  // kOpText / kOpBinary
            c.fragment = payload;
            c.fragmentOpcode = opcode;
        }
        if (fin) {
            std::string msg = std::move(c.fragment);
            c.fragment.clear();
            int op = c.fragmentOpcode;
            c.fragmentOpcode = 0;
            if (op == kOpText && cb_.onMessage) cb_.onMessage(id, msg);
            // binary frames are not part of the player protocol; ignored.
        }
    }
}

void WsServer::queueFrame(Conn& c, int opcode, std::string_view payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));  // FIN + opcode
    std::size_t n = payload.size();
    if (n < 126) {
        frame.push_back(static_cast<char>(n));
    } else if (n <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back(static_cast<char>((n >> 8) & 0xFF));
        frame.push_back(static_cast<char>(n & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));
    }
    // Server-to-client frames are never masked.
    frame.append(payload.data(), payload.size());
    c.outbuf += frame;
    flush(c);
}

void WsServer::flush(Conn& c) {
    while (!c.outbuf.empty()) {
        ssize_t w = ::send(c.fd, c.outbuf.data(), c.outbuf.size(), MSG_NOSIGNAL);
        if (w > 0) {
            c.outbuf.erase(0, static_cast<std::size_t>(w));
        } else {
            break;  // EAGAIN or error; POLLOUT will retry, errors reaped later
        }
    }
}

void WsServer::send(ConnId id, std::string_view text) {
    auto it = conns_.find(id);
    if (it == conns_.end() || !it->second.handshakeDone) return;
    queueFrame(it->second, kOpText, text);
}

void WsServer::broadcast(std::string_view text) {
    for (auto& [id, c] : conns_)
        if (c.handshakeDone && !c.closing) queueFrame(c, kOpText, text);
}

void WsServer::close(ConnId id) {
    auto it = conns_.find(id);
    if (it == conns_.end()) return;
    queueFrame(it->second, kOpClose, "");
    it->second.closing = true;
}

void WsServer::drop(ConnId id) {
    auto it = conns_.find(id);
    if (it == conns_.end()) return;
    bool wasOpen = it->second.handshakeDone;
    ::close(it->second.fd);
    conns_.erase(it);
    if (wasOpen && cb_.onClose) cb_.onClose(id);
}

std::size_t WsServer::connectionCount() const {
    std::size_t n = 0;
    for (auto& [id, c] : conns_)
        if (c.handshakeDone) ++n;
    return n;
}

}  // namespace villen::net
