// Villen chat engine — incremental HTTP/1.1 + SSE parser (DESIGN-chat §3.A/§13).
//
// The host talks to `llama-server` over a non-blocking loopback socket, POSTing
// /v1/chat/completions with "stream": true and reading the Server-Sent-Events
// token deltas as they arrive (§3.A). This class is the *pure* half of that
// client: feed it the raw response bytes as they come off the socket and drain
// the parsed events. It parses the status line + headers, decodes chunked
// transfer-encoding (what llama-server uses for streaming), and splits the body
// into SSE `data:` payloads — but it owns **no socket and no JSON** (the caller
// JSON-parses each payload to pull `choices[0].delta.content`).
//
// Keeping the gnarly incremental byte-parsing pure is the whole point: it is the
// riskiest hand-rolled code in the chat backend, and here it is unit-tested in
// headless CI with canned byte sequences — no model, no GPU, no sockets (§13).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace villen::chat {

class SseParser {
 public:
    enum class Kind {
        Headers,  // status line + headers complete; `status` is the HTTP code
        Data,     // one SSE data payload (e.g. a JSON chunk); in `data`
        Done,     // stream finished cleanly ("data: [DONE]" or the final chunk)
        Error,    // malformed response / non-200 status; reason in `data`
    };

    struct Item {
        Kind kind;
        int status = 0;     // set for Headers
        std::string data;   // payload for Data; reason for Error
    };

    // Feed bytes freshly read from the socket; returns the events that completed
    // with this input, in order. Partial input is buffered for the next call.
    // Once Done or Error is emitted, further input is ignored.
    std::vector<Item> feed(std::string_view bytes);

    // The socket closed (EOF): flush any trailing event and emit a final Done if
    // the stream never sent "[DONE]" (the non-chunked / abrupt-close case).
    std::vector<Item> end();

    bool finished() const { return done_; }

 private:
    enum class Phase { Headers, Body };

    Phase phase_ = Phase::Headers;
    std::string buf_;        // unconsumed raw bytes (headers, then chunk framing)
    std::string sse_;        // decoded body bytes awaiting SSE line splitting
    std::string eventData_;  // accumulated `data:` field(s) of the current event
    bool chunked_ = false;
    bool ended_ = false;     // saw the terminating 0-chunk (logical end of body)
    bool done_ = false;      // emitted Done/Error; ignore further input

    void parseHeaders(std::vector<Item>& out);
    void decodeBody(std::vector<Item>& out);   // chunked -> sse_, or raw -> sse_
    void parseSse(std::vector<Item>& out);     // sse_ -> Data/Done events
    void flushEvent(std::vector<Item>& out);   // emit the pending event, if any
    void finish(std::vector<Item>& out);       // flush + synthesize a final Done
};

}  // namespace villen::chat
