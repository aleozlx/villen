// Pure-core tests for the `chat` engine (DESIGN-chat.md §2/§14 step 1).
//
// Two deterministic halves get the same CI treatment as chess: the conversation
// state machine (append/reset/cap) and the per-model prompt-templating table.
// There is no oracle for *generation* (§2), so nothing here touches a model or a
// GPU — exact data in, exact strings out. Acceptance criterion §1.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "villen/chat/conversation.hpp"
#include "villen/chat/prompt.hpp"
#include "villen/chat/sse.hpp"

using namespace villen::chat;

// --- Conversation state ------------------------------------------------------

TEST_CASE("conversation append / messages ordering") {
    Conversation c;
    c.setSystem("You are helpful.");
    c.addUser("Hi");
    c.addAssistant("Hello!");
    c.addUser("Bye");

    CHECK(c.hasSystem());
    CHECK(c.size() == 3);  // dialogue only — system is held separately

    auto msgs = c.messages();
    REQUIRE(msgs.size() == 4);  // system + 3 dialogue turns, in order
    CHECK(msgs[0] == Turn{Role::System, "You are helpful."});
    CHECK(msgs[1] == Turn{Role::User, "Hi"});
    CHECK(msgs[2] == Turn{Role::Assistant, "Hello!"});
    CHECK(msgs[3] == Turn{Role::User, "Bye"});
}

TEST_CASE("append(System) routes to setSystem, not the dialogue") {
    Conversation c;
    c.append(Role::System, "sys");
    c.append(Role::User, "u");
    CHECK(c.system() == "sys");
    CHECK(c.size() == 1);
    CHECK(c.turns()[0] == Turn{Role::User, "u"});
}

TEST_CASE("reset keeps the operator system prompt; clearSystem wipes it") {
    Conversation c;
    c.setSystem("S");
    c.addUser("u");
    c.addAssistant("a");

    c.reset();  // chatReset (§7): drop dialogue, keep operator state
    CHECK(c.empty());
    CHECK(c.system() == "S");

    c.addUser("u2");
    c.reset(/*clearSystem=*/true);
    CHECK(c.empty());
    CHECK_FALSE(c.hasSystem());
}

TEST_CASE("empty system is not emitted") {
    Conversation c;
    c.addUser("u");
    CHECK_FALSE(c.hasSystem());
    CHECK(c.messages().size() == 1);  // no leading system message
    c.setSystem("");
    CHECK_FALSE(c.hasSystem());
}

// --- Context cap (§5) --------------------------------------------------------

TEST_CASE("token estimate is ceil(chars / kCharsPerToken)") {
    CHECK(Conversation::estimateTokens("") == 0);
    CHECK(Conversation::estimateTokens("a") == 1);
    CHECK(Conversation::estimateTokens("abcd") == 1);   // 4/4
    CHECK(Conversation::estimateTokens("abcde") == 2);  // ceil(5/4)
}

TEST_CASE("estimatedTokens sums content plus per-message overhead") {
    Conversation c;
    c.setSystem("abcd");  // 1 token + overhead
    c.addUser("abcd");    // 1 token + overhead
    const auto k = Conversation::kPerMessageOverhead;
    CHECK(c.estimatedTokens() == (1 + k) + (1 + k));
}

TEST_CASE("capToTokens is a no-op within budget") {
    Conversation c;
    c.addUser("u");
    CHECK(c.capToTokens(10'000) == 0);
    CHECK(c.size() == 1);
}

TEST_CASE("capToTokens drops oldest turns, keeps the most recent and the system") {
    Conversation c;
    c.setSystem("sys");
    c.addUser("11111111");  // 8 chars -> 2 tokens (+overhead)
    c.addAssistant("22222222");
    c.addUser("33333333");  // the live query — must survive

    const auto before = c.estimatedTokens();
    std::size_t dropped = c.capToTokens(before / 2);
    CHECK(dropped >= 1);
    CHECK(c.hasSystem());  // system is preserved
    REQUIRE(c.size() >= 1);
    CHECK(c.turns().back() == Turn{Role::User, "33333333"});  // newest kept
    CHECK(c.estimatedTokens() <= before);
}

TEST_CASE("capToTokens never drops the last remaining dialogue turn") {
    Conversation c;
    c.addUser("a very long single user turn that exceeds any tiny budget");
    CHECK(c.capToTokens(1) == 0);  // can't go below one turn
    CHECK(c.size() == 1);
}

TEST_CASE("capToTokens leaves the dialogue opening on a user turn") {
    Conversation c;
    // Three pairs; trimming the front would otherwise leave an assistant first.
    c.addUser("aaaaaaaa");
    c.addAssistant("bbbbbbbb");
    c.addUser("cccccccc");
    c.addAssistant("dddddddd");
    c.addUser("eeeeeeee");

    c.capToTokens(c.estimatedTokens() / 3);
    REQUIRE(c.size() >= 1);
    CHECK(c.turns().front().role == Role::User);
}

// --- Model registry (§4/§7) --------------------------------------------------

TEST_CASE("known models map to the right family") {
    REQUIRE(knownModels().size() == 3);
    CHECK(findModel("qwen2.5-7b-instruct")->family == ModelFamily::ChatML);
    CHECK(findModel("llama-3.1-8b-instruct")->family == ModelFamily::Llama3);
    CHECK(findModel("mistral-7b-instruct")->family == ModelFamily::Mistral);
    CHECK(findModel("nope") == nullptr);
    // Qwen2.5 is wired first (Apache-2.0, §14).
    CHECK(knownModels().front().id == "qwen2.5-7b-instruct");
}

TEST_CASE("stop tokens per family (§4)") {
    CHECK(stopTokens(ModelFamily::Llama3) == std::vector<std::string>{"<|eot_id|>"});
    CHECK(stopTokens(ModelFamily::ChatML) == std::vector<std::string>{"<|im_end|>"});
    CHECK(stopTokens(ModelFamily::Mistral) == std::vector<std::string>{"</s>"});
}

TEST_CASE("GGUF filenames map to the right model (§5 model registry)") {
    // The conventional bartowski/Q4_K_M names the host scans on the Deck.
    REQUIRE(matchModelByFilename("Qwen2.5-7B-Instruct-Q4_K_M.gguf") != nullptr);
    CHECK(matchModelByFilename("Qwen2.5-7B-Instruct-Q4_K_M.gguf")->id == "qwen2.5-7b-instruct");
    CHECK(matchModelByFilename("Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf")->id ==
          "llama-3.1-8b-instruct");
    CHECK(matchModelByFilename("Mistral-7B-Instruct-v0.3-Q4_K_M.gguf")->id ==
          "mistral-7b-instruct");
    // Case-insensitive, and a different quant still matches.
    CHECK(matchModelByFilename("qwen2.5-7b-instruct-q8_0.gguf")->id == "qwen2.5-7b-instruct");
    // No false positives on unrelated GGUFs.
    CHECK(matchModelByFilename("phi-3-mini-4k-instruct-q4.gguf") == nullptr);
    CHECK(matchModelByFilename("random.gguf") == nullptr);
}

// --- Prompt templating — exact rendered strings (§4) -------------------------
// These pin the fallback raw-prompt path. The primary path lets llama-server
// apply the GGUF template; this is the override for GGUFs whose template is
// missing/wrong (§4) and the guard against a leaked special token (acceptance §4).

TEST_CASE("Llama-3 single turn with system") {
    Conversation c;
    c.setSystem("S");
    c.addUser("U");
    CHECK(renderPrompt(ModelFamily::Llama3, c) ==
          "<|begin_of_text|>"
          "<|start_header_id|>system<|end_header_id|>\n\nS<|eot_id|>"
          "<|start_header_id|>user<|end_header_id|>\n\nU<|eot_id|>"
          "<|start_header_id|>assistant<|end_header_id|>\n\n");
}

TEST_CASE("Llama-3 without system omits the system block") {
    Conversation c;
    c.addUser("U");
    CHECK(renderPrompt(ModelFamily::Llama3, c) ==
          "<|begin_of_text|>"
          "<|start_header_id|>user<|end_header_id|>\n\nU<|eot_id|>"
          "<|start_header_id|>assistant<|end_header_id|>\n\n");
}

TEST_CASE("ChatML single turn with system") {
    Conversation c;
    c.setSystem("S");
    c.addUser("U");
    CHECK(renderPrompt(ModelFamily::ChatML, c) ==
          "<|im_start|>system\nS<|im_end|>\n"
          "<|im_start|>user\nU<|im_end|>\n"
          "<|im_start|>assistant\n");
}

TEST_CASE("Mistral folds system into the first user turn") {
    Conversation c;
    c.setSystem("S");
    c.addUser("U");
    CHECK(renderPrompt(ModelFamily::Mistral, c) == "<s>[INST] S\n\nU [/INST]");
}

TEST_CASE("Mistral without system") {
    Conversation c;
    c.addUser("U");
    CHECK(renderPrompt(ModelFamily::Mistral, c) == "<s>[INST] U [/INST]");
}

TEST_CASE("multi-turn renders correctly across families") {
    Conversation c;
    c.setSystem("You are helpful.");
    c.addUser("Hi");
    c.addAssistant("Hello!");
    c.addUser("Bye");

    CHECK(renderPrompt(ModelFamily::Llama3, c) ==
          "<|begin_of_text|>"
          "<|start_header_id|>system<|end_header_id|>\n\nYou are helpful.<|eot_id|>"
          "<|start_header_id|>user<|end_header_id|>\n\nHi<|eot_id|>"
          "<|start_header_id|>assistant<|end_header_id|>\n\nHello!<|eot_id|>"
          "<|start_header_id|>user<|end_header_id|>\n\nBye<|eot_id|>"
          "<|start_header_id|>assistant<|end_header_id|>\n\n");

    CHECK(renderPrompt(ModelFamily::ChatML, c) ==
          "<|im_start|>system\nYou are helpful.<|im_end|>\n"
          "<|im_start|>user\nHi<|im_end|>\n"
          "<|im_start|>assistant\nHello!<|im_end|>\n"
          "<|im_start|>user\nBye<|im_end|>\n"
          "<|im_start|>assistant\n");

    CHECK(renderPrompt(ModelFamily::Mistral, c) ==
          "<s>[INST] You are helpful.\n\nHi [/INST]Hello!</s>[INST] Bye [/INST]");
}

// The reply must never carry a trailing stop token — that's templating's job to
// fence, and the acceptance §4 promise ("no leaked <|im_end|>/<|eot_id|>/</s>").
TEST_CASE("rendered generation prompt ends ready for the assistant turn") {
    Conversation c;
    c.addUser("hi");
    // None of the renderings end *with* the stop token (they end at the open
    // assistant turn); the model's output is what gets stop-fenced downstream.
    for (auto f : {ModelFamily::Llama3, ModelFamily::ChatML, ModelFamily::Mistral}) {
        const std::string p = renderPrompt(f, c);
        const std::string& stop = stopTokens(f).front();
        CHECK(p.rfind(stop) != p.size() - stop.size());
    }
}

// --- SSE / chunked response parser (§3.A/§13) --------------------------------
// The riskiest hand-rolled code in the chat backend (incremental HTTP headers +
// chunked decode + SSE framing) is pure, so it is pinned here against canned byte
// sequences — no socket, no model, no GPU. The host's LlamaClient wraps this.

namespace {

// A single chunked-transfer chunk: "<hex-size>\r\n<data>\r\n".
std::string chunk(const std::string& s) {
    char hex[24];
    std::snprintf(hex, sizeof hex, "%zx", s.size());
    return std::string(hex) + "\r\n" + s + "\r\n";
}

const char* kHdr =
    "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
    "Transfer-Encoding: chunked\r\n\r\n";
const char* kLastChunk = "0\r\n\r\n";

// Feed `wire` to a fresh parser — all at once, or one byte at a time to exercise
// the incremental buffering — and collect every emitted item.
std::vector<SseParser::Item> drain(const std::string& wire, bool byteWise) {
    SseParser p;
    std::vector<SseParser::Item> all;
    if (byteWise) {
        for (char c : wire) {
            auto items = p.feed(std::string_view(&c, 1));
            all.insert(all.end(), items.begin(), items.end());
        }
    } else {
        all = p.feed(wire);
    }
    return all;
}

std::vector<std::string> dataPayloads(const std::vector<SseParser::Item>& items) {
    std::vector<std::string> out;
    for (const auto& it : items) {
        if (it.kind == SseParser::Kind::Data) {
            out.push_back(it.data);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("SSE: chunked llama-server stream -> headers, data events, done") {
    const std::string wire = std::string(kHdr) + chunk("data: {\"d\":\"He\"}\n\n") +
                             chunk("data: {\"d\":\"llo\"}\n\n") + chunk("data: [DONE]\n\n") +
                             kLastChunk;

    for (bool byteWise : {false, true}) {  // same result whole or byte-by-byte
        auto items = drain(wire, byteWise);
        REQUIRE(items.size() >= 4);
        CHECK(items.front().kind == SseParser::Kind::Headers);
        CHECK(items.front().status == 200);
        auto data = dataPayloads(items);
        REQUIRE(data.size() == 2);
        CHECK(data[0] == "{\"d\":\"He\"}");  // leading space after "data:" stripped
        CHECK(data[1] == "{\"d\":\"llo\"}");
        CHECK(items.back().kind == SseParser::Kind::Done);
        // Exactly one terminal event (the [DONE], not a duplicate from the 0-chunk).
        int dones = 0;
        for (const auto& it : items) {
            dones += (it.kind == SseParser::Kind::Done);
        }
        CHECK(dones == 1);
    }
}

TEST_CASE("SSE: stream without [DONE] is ended by the terminating 0-chunk") {
    const std::string wire = std::string(kHdr) + chunk("data: {\"d\":\"x\"}\n\n") + kLastChunk;
    auto items = drain(wire, false);
    auto data = dataPayloads(items);
    REQUIRE(data.size() == 1);
    CHECK(data[0] == "{\"d\":\"x\"}");
    CHECK(items.back().kind == SseParser::Kind::Done);
}

TEST_CASE("SSE: non-chunked stream is finished by socket close (end())") {
    SseParser p;
    auto a = p.feed(
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"
        "data: {\"d\":\"y\"}\n\n");
    auto b = p.end();  // EOF
    std::vector<SseParser::Item> all = a;
    all.insert(all.end(), b.begin(), b.end());
    auto data = dataPayloads(all);
    REQUIRE(data.size() == 1);
    CHECK(data[0] == "{\"d\":\"y\"}");
    CHECK(all.back().kind == SseParser::Kind::Done);
}

TEST_CASE("SSE: non-200 status yields an error event") {
    auto items = drain("HTTP/1.1 503 Service Unavailable\r\n\r\n", false);
    REQUIRE(items.size() >= 2);
    CHECK(items[0].kind == SseParser::Kind::Headers);
    CHECK(items[0].status == 503);
    CHECK(items[1].kind == SseParser::Kind::Error);
}

TEST_CASE("SSE: a data line split across chunk boundaries reassembles") {
    // The "data: {...}" line is split mid-payload across two chunks.
    const std::string wire = std::string(kHdr) + chunk("data: {\"d\":\"par") + chunk("t\"}\n\n") +
                             chunk("data: [DONE]\n\n") + kLastChunk;
    auto items = drain(wire, false);
    auto data = dataPayloads(items);
    REQUIRE(data.size() == 1);
    CHECK(data[0] == "{\"d\":\"part\"}");
}

TEST_CASE("SSE: an overflowing chunk size cannot bypass the bounds check") {
    // A corrupt/hostile chunk advertises an astronomical size (SIZE_MAX in hex).
    // The parser must treat it as "not fully arrived" and keep buffering — never
    // let `crlf + 2 + size + 2` wrap and read past the buffer (DoS / OOB read).
    const std::string wire = std::string(kHdr) + "ffffffffffffffff\r\n" +
                             "data: {\"d\":\"x\"}\n\n";  // some real bytes follow
    auto items = drain(wire, /*byteWise=*/false);
    REQUIRE(!items.empty());
    CHECK(items.front().kind == SseParser::Kind::Headers);
    CHECK(dataPayloads(items).empty());  // the impossible chunk is never satisfied
    int dones = 0;
    for (const auto& it : items) {
        dones += (it.kind == SseParser::Kind::Done);
    }
    CHECK(dones == 0);
}
