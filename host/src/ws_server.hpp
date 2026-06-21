// Villen host — the WebSocket player edge (the ONLY network boundary, §6/§9.5).
//
// A small, self-contained RFC 6455 server built on non-blocking POSIX sockets.
// It is driven cooperatively from the host's single main loop via poll(): there
// is no background thread and no shared state, exactly the model DESIGN §5
// mandates ("ws.poll(); ... drawAdminUI(); render();"). The design names
// uWebSockets as the production transport; this implementation keeps the same
// poll-shaped seam so µWS can be dropped in behind it later without touching the
// session or engine layers. Performance is a non-issue at LAN/chess volume.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace villen::net {

class WsServer {
public:
    using ConnId = std::uint64_t;

    struct Callbacks {
        std::function<void(ConnId)> onOpen;
        std::function<void(ConnId, std::string_view)> onMessage;  // one text frame
        // One binary frame. The player protocol is text (chess); media engines
        // (filter, §5.1) route their opaque JPEG frames here. Unset by default,
        // so binary frames are simply ignored for engines that don't want them.
        std::function<void(ConnId, std::string_view)> onBinary;
        std::function<void(ConnId)> onClose;
    };

    WsServer() = default;
    ~WsServer();
    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // Serve the browser player client from this directory for plain HTTP GETs on
    // the same port (so players just open ws://host:port's http equivalent).
    // WebSocket upgrades on the same port are unaffected.
    void setStaticRoot(std::string dir) { staticRoot_ = std::move(dir); }

    // Bind and listen on all interfaces. Returns false on failure.
    bool listen(std::uint16_t port);

    // Drain socket events: accept, read+parse frames (-> onMessage), flush queued
    // writes, reap closed conns (-> onClose). Blocks up to timeoutMs for activity
    // (0 = non-blocking poll, suitable for a 60 fps render loop).
    //
    // extraReadFds are fds the server does NOT own (e.g. an engine's inference
    // socket): they are added to the wait set for POLLIN so their readiness ends
    // the block early, but the server never reads them — the owner drains them
    // right after, in the same loop iteration. Lets a streaming engine wake the
    // loop on a token without the server having to know what the fd is.
    void poll(int timeoutMs, const std::vector<int>& extraReadFds = {});

    // Queue a UTF-8 text frame to one connection / all open connections.
    void send(ConnId id, std::string_view text);
    void broadcast(std::string_view text);

    // Queue a binary frame to one connection (the media path, §5.1). There is no
    // binary broadcast: media is per-connection and must never broadcast (§10.1).
    void sendBinary(ConnId id, std::string_view bytes);

    void close(ConnId id);

    std::size_t connectionCount() const;
    std::uint16_t port() const { return port_; }

private:
    struct Conn {
        int fd = -1;
        bool handshakeDone = false;
        std::string inbuf;        // raw bytes awaiting handshake / frame parse
        std::string outbuf;       // bytes pending write
        std::string fragment;     // accumulated payload across continuation frames
        int fragmentOpcode = 0;   // opcode of the message being fragmented
        bool closing = false;     // close frame sent / requested; reap when flushed
    };

    int listenFd_ = -1;
    std::uint16_t port_ = 0;
    ConnId nextId_ = 1;
    std::unordered_map<ConnId, Conn> conns_;
    Callbacks cb_;
    std::string staticRoot_;

    void acceptNew();
    void onReadable(ConnId id);
    void flush(Conn& c);
    bool tryHandshake(ConnId id);   // returns true once handshake completes
    void serveHttp(ConnId id, const std::string& request);  // static file / 404
    void parseFrames(ConnId id);    // consume complete frames from inbuf
    void queueFrame(Conn& c, int opcode, std::string_view payload);
    void drop(ConnId id);           // close fd + fire onClose
};

}  // namespace villen::net
