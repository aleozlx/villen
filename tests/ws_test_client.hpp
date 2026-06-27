// A minimal RFC 6455 WebSocket client for the host integration tests — the C++
// stand-in for the browser player. It speaks exactly what the real client does:
// an HTTP upgrade, then masked text frames out, unmasked frames in. It is NOT a
// general-purpose client (no TLS, no permessage-deflate, no outbound
// fragmentation) — just enough to drive villen::net::WsServer over loopback and
// read back the JSON the engine emits.
//
// Threading model (see integration_tests.cpp): the server runs its poll loop on
// its own thread; a test owns one WsClient per simulated player on the test
// thread and never touches the server objects directly. The only shared state is
// the kernel socket, so no locking is needed here.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace villen::test {

class WsClient {
 public:
    WsClient() = default;
    ~WsClient() { disconnect(); }
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // Connect to 127.0.0.1:port and complete the WebSocket upgrade. Returns false
    // on any socket or handshake failure.
    bool connect(std::uint16_t port) {
        disconnect();  // close any prior socket so a re-connect can't leak the fd
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            disconnect();
            return false;
        }

        // A fixed key is fine: the server only requires Sec-WebSocket-Key to be
        // present (it computes the accept), and we don't verify the accept back.
        static const char* kRequest =
            "GET / HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        if (!sendAll(kRequest, std::strlen(kRequest))) {
            disconnect();
            return false;
        }
        if (!readHandshakeResponse()) {
            disconnect();
            return false;
        }
        return true;
    }

    // Send one masked text frame — what a browser sends. Browsers MUST mask, and
    // the server drops any unmasked client frame, so masking is mandatory here.
    void sendText(std::string_view text) {
        if (fd_ < 0) return;
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | 0x1));  // FIN + text opcode

        const std::size_t n = text.size();
        const unsigned char maskBit = 0x80;
        if (n < 126) {
            frame.push_back(static_cast<char>(maskBit | n));
        } else if (n <= 0xFFFF) {
            frame.push_back(static_cast<char>(maskBit | 126));
            frame.push_back(static_cast<char>((n >> 8) & 0xFF));
            frame.push_back(static_cast<char>(n & 0xFF));
        } else {
            frame.push_back(static_cast<char>(maskBit | 127));
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));
        }

        const unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
        frame.append(reinterpret_cast<const char*>(mask), 4);
        for (std::size_t i = 0; i < n; ++i)
            frame.push_back(static_cast<char>(text[i] ^ mask[i & 3]));

        sendAll(frame.data(), frame.size());
    }

    // Collect every text message available, returning once no further frame
    // arrives. Waits up to firstMs for the first frame (a server reply may be a
    // few ms out), then only idleMs between frames (a server flushes a whole
    // batch — joined+state+sessionUpdate — back to back, so the inter-frame gap
    // is ~0; the idle wait only ends the batch). Mirrors the Python `drain`.
    std::vector<std::string> drain(int firstMs = 1500, int idleMs = 150) {
        std::vector<std::string> out;
        int timeout = firstMs;
        for (;;) {
            std::string payload;
            bool isText = false;
            if (!recvFrame(timeout, payload, isText)) break;  // timeout / closed
            if (isText) out.push_back(std::move(payload));
            timeout = idleMs;
        }
        return out;
    }

    // Collect every text message that arrives within `totalMs` of wall-clock,
    // regardless of the gap between frames. drain() ends on an idle gap, which
    // never comes for an engine that broadcasts continuously (snake ticks ~10 Hz
    // whether or not anyone acts); this fixed window is the right tool there.
    std::vector<std::string> collect(int totalMs) {
        std::vector<std::string> out;
        timeval start{};
        ::gettimeofday(&start, nullptr);
        auto elapsedMs = [&]() {
            timeval now{};
            ::gettimeofday(&now, nullptr);
            return static_cast<int>((now.tv_sec - start.tv_sec) * 1000 +
                                    (now.tv_usec - start.tv_usec) / 1000);
        };
        for (;;) {
            int remaining = totalMs - elapsedMs();
            if (remaining <= 0) break;
            std::string payload;
            bool isText = false;
            if (!recvFrame(remaining, payload, isText)) break;  // timeout / closed
            if (isText) out.push_back(std::move(payload));
        }
        return out;
    }

    // Close the TCP socket: the server sees the player drop (recv 0 -> onClose).
    void disconnect() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        rx_.clear();
    }

    bool connected() const { return fd_ >= 0; }

 private:
    int fd_ = -1;
    std::string rx_;  // bytes read from the socket but not yet consumed as frames

    bool sendAll(const char* data, std::size_t n) {
        std::size_t sent = 0;
        while (sent < n) {
            ssize_t w = ::send(fd_, data + sent, n - sent, MSG_NOSIGNAL);
            if (w <= 0) return false;
            sent += static_cast<std::size_t>(w);
        }
        return true;
    }

    void setRecvTimeout(int ms) {
        timeval tv{};
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    // Read the 101 response, stopping exactly at the end of the headers so any
    // frame bytes the server piggybacked (the engine-announce) stay in rx_.
    bool readHandshakeResponse() {
        setRecvTimeout(2000);
        std::string buf;
        char tmp[1024];
        for (;;) {
            ssize_t r = ::recv(fd_, tmp, sizeof(tmp), 0);
            if (r <= 0) return false;
            buf.append(tmp, static_cast<std::size_t>(r));
            auto end = buf.find("\r\n\r\n");
            if (end != std::string::npos) {
                rx_.assign(buf, end + 4, std::string::npos);  // leftover = frames
                return buf.compare(0, 12, "HTTP/1.1 101") == 0;
            }
            if (buf.size() > 16384) return false;  // runaway / not an HTTP reply
        }
    }

    // Ensure rx_ holds at least n bytes, pulling from the socket as needed.
    bool ensure(std::size_t n, int timeoutMs) {
        setRecvTimeout(timeoutMs);
        char tmp[4096];
        while (rx_.size() < n) {
            ssize_t r = ::recv(fd_, tmp, sizeof(tmp), 0);
            if (r <= 0) return false;  // 0 = peer closed, <0 = timeout/error
            rx_.append(tmp, static_cast<std::size_t>(r));
        }
        return true;
    }

    // Parse one frame off rx_. The server never fragments its outgoing messages
    // (queueFrame emits a single FIN frame), so continuation handling isn't
    // needed. Control frames (close/ping/pong) come back with isText=false and
    // are simply skipped by drain().
    bool recvFrame(int timeoutMs, std::string& out, bool& isText) {
        if (!ensure(2, timeoutMs)) return false;
        auto byte = [&](std::size_t i) {
            return static_cast<unsigned char>(rx_[i]);
        };
        const int opcode = byte(0) & 0x0F;
        const bool masked = byte(1) & 0x80;  // server-to-client is unmasked
        std::uint64_t len = byte(1) & 0x7F;
        std::size_t off = 2;
        if (len == 126) {
            if (!ensure(off + 2, timeoutMs)) return false;
            len = (static_cast<std::uint64_t>(byte(2)) << 8) | byte(3);
            off = 4;
        } else if (len == 127) {
            if (!ensure(off + 8, timeoutMs)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | byte(2 + i);
            off = 10;
        }
        // len is read straight off the wire; cap it (the server's own 1 MiB
        // limit) so a corrupt/oversized length can't overflow off + maskLen + len
        // and slip past the ensure() bounds check below into an OOB mask loop.
        if (len > (1u << 20)) return false;
        const std::size_t maskLen = masked ? 4 : 0;
        if (!ensure(off + maskLen + len, timeoutMs)) return false;

        unsigned char mask[4] = {0, 0, 0, 0};
        for (std::size_t i = 0; i < maskLen; ++i) mask[i] = byte(off + i);
        const std::size_t dataOff = off + maskLen;

        std::string payload(len, '\0');
        for (std::uint64_t i = 0; i < len; ++i)
            payload[i] = static_cast<char>(byte(dataOff + i) ^ mask[i & 3]);
        rx_.erase(0, dataOff + len);

        isText = (opcode == 0x1);
        out = std::move(payload);
        return true;
    }
};

}  // namespace villen::test
