// Villen — ChatEngine: local LLM chat on the IEngine contract (DESIGN-chat.md).
//
// Chat is the engine that proves the axis chess and filter both ducked: work
// that genuinely blocks for seconds (§3). The real backend is a subprocess
// `llama-server` whose streaming socket joins the host's poll() set (§3.A) — but
// this file is **build-order step 2** (§14): the full streaming spine wired to a
// STUB generator that echoes "you said: …" token-by-token on the onTick timer,
// with zero inference and zero GPU. It proves chatSend → chatDelta* → chatDone
// end to end before any model exists; step 3 swaps the stub for the SSE client.
//
// Chat is per-connection, never broadcast, never persisted (§7/§11): each
// connection owns its conversations, keyed by ConnId — so this engine declares no
// seats and relies on Room's symmetric spectator onLeave to release a dropped
// connection's state. The deterministic conversation/templating core it builds on
// is the pure villen_chat library (engine/chat/, §2).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine.hpp"
#include "villen/chat/conversation.hpp"
#include "villen/chat/prompt.hpp"

namespace villen {

class ChatEngine : public IEngine {
 public:
    SeatRoster seats() const override { return {}; }  // per-connection; no seats
    void onJoin(Room&, ConnId, SeatId) override;      // push chatConfig
    void onLeave(Room&, ConnId, SeatId) override;     // drop the conn's state (§11)
    void onMessage(Room&, ConnId, SeatId, std::string_view) override;
    void onTick(Room&, std::uint64_t nowMs) override; // drive the stub stream
    std::string statusLine() const override;
    void drawAdmin() override;
    void reset() override;                             // stop all + clear

 private:
    // Server-authoritative model + sampling params (§7 authority / §9). The model
    // id and contextMax are pushed to clients as chatConfig; the sampling params
    // and system prompt stay host-side (the client may request nothing it can't
    // already type, §7) and feed the real backend at step 3.
    std::string model_ = "qwen2.5-7b-instruct";  // Qwen first (Apache-2.0, §14)
    std::string systemPrompt_;
    int contextMax_ = 8192;
    float temperature_ = 0.7f;
    float topP_ = 0.95f;
    int maxTokens_ = 512;

    // Per-connection, per-convId conversation state — private, in RAM only (§11).
    std::unordered_map<ConnId,
                       std::unordered_map<std::string, chat::Conversation>>
        convs_;

    // One in-flight generation per (conn, convId): turn-of-generation (§7). Step 2
    // is a stub — `tokens` is the pre-split echo reply, emitted on the onTick clock.
    struct Gen {
        ConnId conn = 0;
        std::string convId;
        int msgId = 0;
        std::vector<std::string> tokens;
        std::size_t next = 0;        // index of the next token to emit
        std::uint64_t nextMs = 0;    // earliest time to emit it
        std::uint64_t startMs = 0;   // first-emit time, for tok/s
        std::string acc;             // reply so far; appended to the conv on done
    };
    std::vector<Gen> gens_;
    int nextMsgId_ = 1;
    std::uint64_t nowMs_ = 0;  // last tick clock, so onMessage can stamp tok/s

    static constexpr std::uint64_t kStubIntervalMs = 60;  // ~16 tok/s, Deck-ish

    chat::Conversation& conv(ConnId, const std::string& convId);
    Gen* genFor(ConnId, const std::string& convId);
    void cancel(ConnId, const std::string& convId);  // drop in-flight gen, silent
    std::string configJson() const;                  // the chatConfig frame (§7)
};

class ChatFactory : public IEngineFactory {
 public:
    std::unique_ptr<IEngine> create() override {
        return std::make_unique<ChatEngine>();
    }
    const char* name() const override { return "chat"; }
    // The chat browser view lives in client/chat/ (served at /chat/). Per-engine
    // client routing isn't auto-wired yet, so this is the forward-compatible
    // default; for now run with the host's client/ root and open /chat/.
    const char* clientDir() const override { return "chat"; }
};

}  // namespace villen
