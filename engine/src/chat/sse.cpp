#include "villen/chat/sse.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace villen::chat {

namespace {

// Case-insensitive substring search, for header-name matching.
bool containsCI(const std::string& hay, const std::string& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != hay.end();
}

}  // namespace

std::vector<SseParser::Item> SseParser::feed(std::string_view bytes) {
    std::vector<Item> out;
    if (done_) return out;
    buf_.append(bytes.data(), bytes.size());

    if (phase_ == Phase::Headers) parseHeaders(out);
    if (phase_ == Phase::Body && !done_) {
        decodeBody(out);
        parseSse(out);
        if (ended_ && !done_) finish(out);  // 0-chunk reached without a [DONE]
    }
    return out;
}

std::vector<SseParser::Item> SseParser::end() {
    std::vector<Item> out;
    if (done_) return out;
    if (phase_ == Phase::Body) {
        if (!chunked_) {  // raw stream: any remaining bytes are body
            sse_ += buf_;
            buf_.clear();
        }
        parseSse(out);
        finish(out);
    }
    return out;
}

void SseParser::parseHeaders(std::vector<Item>& out) {
    auto end = buf_.find("\r\n\r\n");
    if (end == std::string::npos) return;  // headers not complete yet

    std::string head = buf_.substr(0, end);
    buf_.erase(0, end + 4);

    // Status line: "HTTP/1.1 200 OK".
    int status = 0;
    auto sp = head.find(' ');
    if (sp != std::string::npos) status = std::atoi(head.c_str() + sp + 1);
    chunked_ = containsCI(head, "transfer-encoding: chunked");

    out.push_back({Kind::Headers, status, {}});
    phase_ = Phase::Body;
    if (status != 200) {
        out.push_back({Kind::Error, status, "http_" + std::to_string(status)});
        done_ = true;
    }
}

void SseParser::decodeBody(std::vector<Item>&) {
    if (!chunked_) {
        // Not chunked: the body is the raw SSE stream until the socket closes.
        sse_ += buf_;
        buf_.clear();
        return;
    }
    // Chunked transfer-encoding: <hex-size>\r\n<data>\r\n ... 0\r\n\r\n.
    for (;;) {
        auto crlf = buf_.find("\r\n");
        if (crlf == std::string::npos) return;  // size line incomplete
        // Hex size, ignoring any ";chunk-ext" suffix.
        std::size_t size =
            static_cast<std::size_t>(std::strtoul(buf_.substr(0, crlf).c_str(), nullptr, 16));
        if (size == 0) {  // last chunk; trailers (if any) are ignored
            buf_.clear();
            ended_ = true;
            return;  // feed()/parseSse handle flushing the final event
        }
        // A malicious/corrupt chunk size could be enormous; test it by
        // subtraction on the buffer size (the known-bounded side) so the
        // "+ size" never overflows std::size_t and bypasses the check.
        if (buf_.size() < crlf + 4 || buf_.size() - (crlf + 4) < size)
            return;  // full chunk not arrived
        sse_.append(buf_, crlf + 2, size);
        buf_.erase(0, crlf + size + 4);  // drop size line, data, trailing CRLF
    }
}

void SseParser::parseSse(std::vector<Item>& out) {
    // Split sse_ into complete lines; keep a trailing partial line buffered.
    std::size_t pos = 0;
    for (;;) {
        auto nl = sse_.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = sse_.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.empty()) {  // blank line terminates the current event
            flushEvent(out);
            if (done_) break;
            continue;
        }
        if (line.rfind("data:", 0) == 0) {
            std::string payload = line.substr(5);
            if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
            if (!eventData_.empty()) eventData_ += '\n';
            eventData_ += payload;
        }
        // event:/id:/retry:/":comment" fields are unused by llama-server.
    }
    sse_.erase(0, pos);
}

void SseParser::flushEvent(std::vector<Item>& out) {
    if (eventData_.empty()) return;
    if (eventData_ == "[DONE]") {
        out.push_back({Kind::Done, 0, {}});
        done_ = true;
    } else {
        out.push_back({Kind::Data, 0, std::move(eventData_)});
    }
    eventData_.clear();
}

void SseParser::finish(std::vector<Item>& out) {
    if (done_) return;
    flushEvent(out);  // a trailing event not terminated by a blank line
    if (!done_) {
        out.push_back({Kind::Done, 0, {}});
        done_ = true;
    }
}

}  // namespace villen::chat
