// Villen host — non-blocking HTTP/1.1 + SSE client for llama-server (DESIGN-chat
// §3.A/§13).
//
// The host streams completions from a local `llama-server` (OpenAI-compatible)
// by POSTing /v1/chat/completions with "stream": true and reading the SSE token
// deltas as they arrive. The response socket is **non-blocking** and pumped from
// the host's single main loop (onTick) — no thread, no blocking call, exactly the
// §3.A model ("the subprocess's stdout/socket is just another fd in poll()").
// This is deliberately **not** libcurl, whose blocking calls would stall the loop
// (§16). The gnarly byte-parsing lives in the pure, unit-tested villen::chat::
// SseParser; this class adds only sockets + the JSON pull of delta.content.
//
// One streaming request == one TCP connection (mapped to one conversation, §8);
// llama-server's own slots/continuous-batching interleave them on the resident
// model. The inference boundary is a neutral "conversation+params -> token
// stream" seam (§12.1) — chat and a future chess LLM-mover share it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "villen/chat/sse.hpp"

namespace villen::chat {

// Callbacks for one streaming generation. Fired over subsequent pump() calls; the
// caller's closures route deltas to the right WS connection.
struct StreamSink {
    std::function<void(std::string_view delta)> onDelta;
    std::function<void(const std::string& stopReason, int tokens)> onDone;
    std::function<void(const std::string& reason)> onError;  // backend_down, http_*
};

class LlamaClient {
 public:
    using ReqId = std::uint64_t;

    LlamaClient(std::string host, int port);
    ~LlamaClient();
    LlamaClient(const LlamaClient&) = delete;
    LlamaClient& operator=(const LlamaClient&) = delete;

    // POST /v1/chat/completions with `body` (the caller builds the JSON: messages
    // + sampling params + "stream": true). Returns a ReqId whose sink fires over
    // later pump() calls; 0 if the socket couldn't even be created.
    ReqId start(const std::string& body, StreamSink sink);

    // Abandon a request (e.g. chatStop, or the WS connection dropped). No sink
    // callback fires; the socket is closed.
    void cancel(ReqId);

    // Non-blocking I/O for every active request: finish connects, flush the
    // request, read available bytes, parse, and fire sink callbacks. Call once per
    // main-loop iteration (onTick). Cheap when there are no active requests.
    void pump();

    std::size_t activeCount() const { return reqs_.size(); }

    // Append every active request's socket so the host can fold them into its
    // poll() wait set — then a token arriving wakes the loop immediately and the
    // next pump() drains it, instead of waiting out the poll timeout.
    void collectFds(std::vector<int>& out) const;

 private:
    struct Req {
        ReqId id = 0;
        int fd = -1;
        bool connected = false;
        std::string outbuf;  // request bytes still to send
        SseParser parser;
        StreamSink sink;
        int tokens = 0;
        std::string stopReason = "eos";
        bool finished = false;  // terminal sink callback fired; reap this pump
    };

    std::string host_;
    int port_;
    ReqId nextId_ = 1;
    std::vector<Req> reqs_;

    void serviceWrite(Req&);
    void serviceRead(Req&);
    void handleItem(Req&, const SseParser::Item&);
    void fail(Req&, const std::string& reason);
};

}  // namespace villen::chat
