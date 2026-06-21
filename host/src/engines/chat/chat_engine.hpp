// Villen — ChatEngine: local LLM chat on the IEngine contract (DESIGN-chat.md).
//
// Chat is the engine that proves the axis chess and filter both ducked: work
// that genuinely blocks for seconds (§3). The decided backend is a subprocess
// `llama-server` whose streaming socket is read non-blocking from the host's one
// poll loop (§3.A) — see LlamaClient. Three backend modes:
//   - Llama (--llama-url HOST:PORT): real streaming via LlamaClient.
//   - Stub  (--chat-stub): echoes "you said: …" token-by-token on the onTick
//     clock — the build-order step-2 spine, zero inference/GPU, kept for dev/CI.
//   - Down  (neither): chatSend reports backend_down cleanly (degrade, don't
//     fail, §13); chatConfig advertises ready:false.
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
#include <utility>
#include <vector>

#include "engine.hpp"
#include "llama_client.hpp"
#include "llama_process.hpp"
#include "villen/chat/conversation.hpp"
#include "villen/chat/prompt.hpp"

namespace villen {

// How --engine chat reaches an inference backend (set from CLI, passed via the
// factory). Default is Down: no backend wired.
//   - llamaBin set       → spawn & manage llama-server (LlamaProcess), connect to it.
//   - llamaPort set only  → connect to an already-running llama-server (or stub).
//   - stub                → in-host echo generator (no inference).
//   - none                → Down (chatSend -> backend_down).
struct ChatBackendConfig {
    std::string llamaHost = "127.0.0.1";
    int llamaPort = 0;        // connect/spawn port (spawn defaults to 8080)
    bool stub = false;        // --chat-stub → in-host echo generator
    std::string llamaBin;     // --llama-bin → spawn this llama-server
    std::string model;        // -m model for the spawned server (§11)
    int ngl = 99;             // -ngl GPU layers (§6)
    int parallel = 2;         // --parallel slots (§8)
    // Operator-supplied model id → GGUF path map (§11; weights are not shipped),
    // from repeated --model-path id=path. The admin console's Load/Switch (§9)
    // resolves the selected model id to a path here and restarts llama-server
    // with it. The first entry (or `model`) is the model loaded at startup.
    std::vector<std::pair<std::string, std::string>> modelPaths;
};

class ChatEngine : public IEngine {
 public:
    explicit ChatEngine(ChatBackendConfig cfg = {});

    SeatRoster seats() const override { return {}; }  // per-connection; no seats
    void onJoin(Room&, ConnId, SeatId) override;      // push chatConfig
    void onLeave(Room&, ConnId, SeatId) override;     // drop the conn's state (§11)
    void onMessage(Room&, ConnId, SeatId, std::string_view) override;
    void onTick(Room&, std::uint64_t nowMs) override; // pump SSE + drive the stub
    std::string statusLine() const override;
    void drawAdmin() override;
    void reset() override;                             // stop all + clear

 private:
    enum class Mode { Down, Stub, Llama };

    // Server-authoritative model + sampling params (§7 authority / §9). The model
    // id and contextMax are pushed to clients as chatConfig; the sampling params
    // and system prompt stay host-side (the client may request nothing it can't
    // already type, §7) and feed the request body in Llama mode.
    std::string model_ = "qwen2.5-7b-instruct";  // Qwen first (Apache-2.0, §14)
    std::string systemPrompt_;
    int contextMax_ = 8192;
    float temperature_ = 0.7f;
    float topP_ = 0.95f;
    int topK_ = 40;
    int maxTokens_ = 512;
    float repeatPenalty_ = 1.1f;

    // Admin console (§9) state: the model the operator has picked in the combo but
    // not yet committed (Load/Switch applies it), and the last load's feedback.
    std::string pendingModel_ = model_;
    std::string loadError_;
    // Operator-supplied model id → GGUF path (from ChatBackendConfig::modelPaths);
    // empty path / missing id ⇒ that model can't be loaded (no weights shipped, §11).
    std::unordered_map<std::string, std::string> modelPaths_;

    Mode mode_ = Mode::Down;
    std::unique_ptr<chat::LlamaClient> llama_;     // set in Llama mode
    std::unique_ptr<chat::LlamaProcess> process_;  // set when we spawn llama-server

    // Per-connection, per-convId conversation state — private, in RAM only (§11).
    std::unordered_map<ConnId,
                       std::unordered_map<std::string, chat::Conversation>>
        convs_;

    // One in-flight generation per (conn, convId): turn-of-generation (§7). In
    // Stub mode `tokens` is the pre-split echo reply, emitted on the onTick clock;
    // in Llama mode `reqId` is the LlamaClient request whose sink fills `acc`.
    struct Gen {
        ConnId conn = 0;
        std::string convId;
        int msgId = 0;
        std::string acc;             // reply so far; appended to the conv on done
        std::uint64_t startMs = 0;   // first-token time, for tok/s
        int emitted = 0;             // deltas streamed so far (live admin tok/s, §9)
        bool stub = false;
        // Stub timer:
        std::vector<std::string> tokens;
        std::size_t next = 0;
        std::uint64_t nextMs = 0;
        // Llama:
        chat::LlamaClient::ReqId reqId = 0;
    };
    std::vector<Gen> gens_;
    int nextMsgId_ = 1;
    std::uint64_t nowMs_ = 0;  // last tick clock, so sinks/onMessage stamp tok/s

    static constexpr std::uint64_t kStubIntervalMs = 60;  // ~16 tok/s, Deck-ish

    chat::Conversation& conv(ConnId, const std::string& convId);
    Gen* genFor(ConnId, const std::string& convId);
    // Remove the engine's record of a generation. `abort` also cancels the live
    // LlamaClient request — use it ONLY outside LlamaClient::pump() (chatStop,
    // onLeave, reset); a sink-driven removal must pass abort=false (the request is
    // already finishing inside pump, cancelling it there would reenter the pump).
    void removeGen(ConnId, const std::string& convId, bool abort);

    void startStub(ConnId, const std::string& convId, const std::string& userText);
    void startLlama(Room&, ConnId, const std::string& convId);
    std::string requestBody(const chat::Conversation&) const;  // /v1/chat body
    std::string configJson() const;                            // chatConfig (§7)

    // GGUF path for a model id, or "" if the operator configured none (§11).
    std::string pathForModel(const std::string& id) const;
    // Operator "Load/Switch" (§9): commit pendingModel_ as the active model, push
    // chatConfig, and (in spawn mode) restart llama-server with its GGUF.
    void loadPendingModel();
    // Operator "Stop all" (§9): abort every in-flight generation (keep the
    // conversations); reset() additionally clears the conversations ("new game").
    void stopAll();
    void drawAdmin_ModelPanel();   // §9 model select/switch + llama-server health
    void drawAdmin_ParamsPanel();  // §9 generation params + system-prompt editor
    void drawAdmin_StatsPanel();   // §9 live stats (metadata only — privacy, §11)
};

class ChatFactory : public IEngineFactory {
 public:
    explicit ChatFactory(ChatBackendConfig cfg = {}) : cfg_(std::move(cfg)) {}
    std::unique_ptr<IEngine> create() override {
        return std::make_unique<ChatEngine>(cfg_);
    }
    const char* name() const override { return "chat"; }
    // The chat browser view lives in client/chat/ (served at /chat/). Per-engine
    // client routing isn't auto-wired yet, so this is the forward-compatible
    // default; for now run with the host's client/ root and open /chat/.
    const char* clientDir() const override { return "chat"; }

 private:
    ChatBackendConfig cfg_;
};

}  // namespace villen
