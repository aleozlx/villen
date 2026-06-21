#include "chat_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "room.hpp"

#ifdef VILLEN_ADMIN_UI
#include <cfloat>  // FLT_MIN — the ImGui "stretch to width" sentinel

#include "imgui.h"
#include "imgui_stdlib.h"  // ImGui::InputTextMultiline(std::string&) — system prompt
#endif

using json = nlohmann::json;

namespace villen {
namespace {

// Read a string field from untrusted client JSON without throwing: a wrong JSON
// type (or a missing key) yields "" rather than a json::type_error that would
// crash the single-thread server (DoS) — same discipline as chess/envelope.
std::string strField(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// --- §7 server -> client frames ---------------------------------------------

std::string deltaMsg(const std::string& convId, int msgId, const std::string& delta) {
    json msg = {{"type", "chatDelta"}, {"convId", convId}, {"msgId", msgId},
                {"delta", delta}};
    return msg.dump();
}

std::string doneMsg(const std::string& convId, int msgId, const char* stopReason,
                    int tokens, double tps) {
    json msg = {{"type", "chatDone"}, {"convId", convId}, {"msgId", msgId},
                {"stopReason", stopReason}, {"tokens", tokens}, {"tps", tps}};
    return msg.dump();
}

std::string errorMsg(const std::string& convId, const char* reason) {
    json msg = {{"type", "chatError"}, {"convId", convId}, {"reason", reason}};
    return msg.dump();
}

// Split a reply into stream tokens, each carrying its trailing space, so the
// client reconstructs the text by simple concatenation. The Stub generator's
// stand-in for the real tokenizer.
std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> toks;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = s.find(' ', i);
        if (j == std::string::npos) {
            toks.push_back(s.substr(i));
            break;
        }
        toks.push_back(s.substr(i, j - i + 1));  // include the space
        i = j + 1;
    }
    return toks;
}

double tokensPerSec(int emitted, std::uint64_t startMs, std::uint64_t nowMs) {
    if (startMs == 0 || nowMs <= startMs) return 0.0;
    return static_cast<double>(emitted) * 1000.0 / static_cast<double>(nowMs - startMs);
}

}  // namespace

ChatEngine::ChatEngine(ChatBackendConfig cfg) {
    // The operator-supplied model id → GGUF map (§11): the console's Load/Switch
    // resolves a selected id to a path here. Fold the legacy single --model path
    // in under the startup model id so it stays switchable-back-to.
    for (const auto& kv : cfg.modelPaths) {
        modelPaths_[kv.first] = kv.second;
    }
    if (!cfg.model.empty() && !modelPaths_.count(model_)) {
        modelPaths_[model_] = cfg.model;
    }
    pendingModel_ = model_;

    if (!cfg.llamaBin.empty()) {
        // Spawn-and-manage: the host owns the llama-server child (§3.A).
        mode_ = Mode::Llama;
        int port = cfg.llamaPort > 0 ? cfg.llamaPort : 8080;
        chat::LlamaSpawnConfig sp;
        sp.bin = cfg.llamaBin;
        sp.model = cfg.model;
        sp.host = cfg.llamaHost;
        sp.port = port;
        sp.ngl = cfg.ngl;
        sp.parallel = cfg.parallel;
        sp.ctxSize = contextMax_;  // -c: bound KV memory to the host's context cap (§5)
        process_ = std::make_unique<chat::LlamaProcess>(std::move(sp));
        llama_ = std::make_unique<chat::LlamaClient>(cfg.llamaHost, port);
    } else if (cfg.llamaPort > 0) {
        // Connect to an already-running llama-server (or a stub, for tests).
        mode_ = Mode::Llama;
        llama_ = std::make_unique<chat::LlamaClient>(cfg.llamaHost, cfg.llamaPort);
    } else if (cfg.stub) {
        mode_ = Mode::Stub;
    } else {
        mode_ = Mode::Down;
    }
}

chat::Conversation& ChatEngine::conv(ConnId conn, const std::string& convId) {
    return convs_[conn][convId];
}

ChatEngine::Gen* ChatEngine::genFor(ConnId conn, const std::string& convId) {
    for (auto& g : gens_) {
        if (g.conn == conn && g.convId == convId) {
            return &g;
        }
    }
    return nullptr;
}

void ChatEngine::removeGen(ConnId conn, const std::string& convId, bool abort) {
    gens_.erase(std::remove_if(gens_.begin(), gens_.end(),
                               [&](const Gen& g) {
                                   if (g.conn != conn || g.convId != convId) return false;
                                   if (abort && g.reqId && llama_) llama_->cancel(g.reqId);
                                   return true;
                               }),
                gens_.end());
}

std::string ChatEngine::configJson() const {
    json models = json::array();
    for (const auto& m : chat::knownModels()) models.push_back(m.id);
    // ready reflects the backend: a spawned server must pass /health first; a
    // connect-only backend is optimistic (errors surface as backend_down); Down
    // and Stub-less is never ready.
    bool ready = mode_ == Mode::Stub ||
                 (mode_ == Mode::Llama && (!process_ || process_->ready()));
    json msg = {{"type", "chatConfig"},
                {"model", model_},
                {"models", std::move(models)},
                {"contextMax", contextMax_},
                {"ready", ready}};
    return msg.dump();
}

std::string ChatEngine::requestBody(const chat::Conversation& c) const {
    // Primary path (§4): send structured messages; llama-server applies the GGUF's
    // own chat template. The host never touches raw special-token strings.
    json messages = json::array();
    for (const auto& t : c.messages())
        messages.push_back({{"role", chat::roleName(t.role)}, {"content", t.content}});
    json body = {{"model", model_},
                 {"messages", std::move(messages)},
                 {"stream", true},
                 {"temperature", temperature_},
                 {"top_p", topP_},
                 {"top_k", topK_},
                 {"max_tokens", maxTokens_},
                 {"repeat_penalty", repeatPenalty_}};
    return body.dump();
}

void ChatEngine::onJoin(Room& room, ConnId conn, SeatId) {
    // Every connection learns the active model + readiness on join (§7).
    room.send(conn, configJson());
}

void ChatEngine::onLeave(Room&, ConnId conn, SeatId) {
    // Privacy (§11): a dropped connection's conversations and any in-flight
    // generation vanish with it. Abort the live LlamaClient request(s) too — this
    // is a network event (poll loop), not inside LlamaClient::pump(), so it's safe.
    convs_.erase(conn);
    gens_.erase(std::remove_if(gens_.begin(), gens_.end(),
                               [&](const Gen& g) {
                                   if (g.conn != conn) return false;
                                   if (g.reqId && llama_) llama_->cancel(g.reqId);
                                   return true;
                               }),
                gens_.end());
}

void ChatEngine::onMessage(Room& room, ConnId conn, SeatId, std::string_view text) {
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("type")) {
        room.send(conn, errorMsg("", "bad_message"));
        return;
    }
    // Every field type-checked (strField never throws), so a malformed payload
    // (e.g. {"type":123} or a non-string text) is rejected, not fatal.
    const std::string type = strField(j, "type");
    const std::string convId = strField(j, "convId");

    if (type == "chatSend") {
        const std::string userText = strField(j, "text");
        if (convId.empty() || userText.empty()) {
            room.send(conn, errorMsg(convId, "bad_message"));
            return;
        }
        if (mode_ == Mode::Down) {
            room.send(conn, errorMsg(convId, "backend_down"));
            return;
        }
        // Turn-of-generation: one in-flight reply per conversation (§7/§8). A
        // second send while generating is rejected (queueing is a step-8 concern).
        if (genFor(conn, convId)) {
            room.send(conn, errorMsg(convId, "model_busy"));
            return;
        }
        // A spawned llama-server still loading its model isn't ready yet (§9).
        if (mode_ == Mode::Llama && process_ && !process_->ready()) {
            room.send(conn, errorMsg(convId, "backend_down"));
            return;
        }
        chat::Conversation& c = conv(conn, convId);
        c.setSystem(systemPrompt_);
        c.addUser(userText);
        c.capToTokens(static_cast<std::size_t>(contextMax_));

        if (mode_ == Mode::Stub)
            startStub(conn, convId, userText);
        else
            startLlama(room, conn, convId);
        return;
    }

    if (type == "chatReset") {
        removeGen(conn, convId, /*abort=*/true);
        auto it = convs_.find(conn);
        if (it != convs_.end()) {
            auto cit = it->second.find(convId);
            if (cit != it->second.end()) cit->second.reset();  // keep system prompt
        }
        return;
    }

    if (type == "chatStop") {
        if (Gen* g = genFor(conn, convId)) {
            if (!g->acc.empty()) conv(conn, convId).addAssistant(g->acc);
            room.send(conn, doneMsg(convId, g->msgId, "stopped", g->emitted,
                                    tokensPerSec(g->emitted, g->startMs, nowMs_)));
            removeGen(conn, convId, /*abort=*/true);
        }
        return;
    }

    room.send(conn, errorMsg(convId, "bad_message"));
}

void ChatEngine::startStub(ConnId conn, const std::string& convId,
                           const std::string& userText) {
    Gen g;
    g.conn = conn;
    g.convId = convId;
    g.msgId = nextMsgId_++;
    g.stub = true;
    g.tokens = tokenize("you said: " + userText);  // STUB reply (§14 step 2)
    g.nextMs = 0;   // first token on the next tick → low time-to-first-token
    g.startMs = 0;  // stamped on first emit
    gens_.push_back(std::move(g));
}

void ChatEngine::startLlama(Room& room, ConnId conn, const std::string& convId) {
    Gen g;
    g.conn = conn;
    g.convId = convId;
    g.msgId = nextMsgId_++;
    g.startMs = nowMs_;
    const int msgId = g.msgId;
    gens_.push_back(std::move(g));

    // Sinks fire later, from LlamaClient::pump() in onTick. They route deltas to
    // this connection and finalize via removeGen(abort=false): the request is
    // already finishing inside pump, so we must NOT cancel it there (reentrancy).
    // These fire from pump() in onTick. room_ is normally valid then, but it is
    // nulled by detach() at engine teardown — guard so a sink that pump() runs
    // after detach can't dereference a dead Room.
    chat::StreamSink sink;
    sink.onDelta = [this, conn, convId, msgId](std::string_view d) {
        if (Gen* g = genFor(conn, convId)) {
            g->acc.append(d);
            ++g->emitted;  // live tok/s in the admin stats panel (§9)
        }
        if (room_) room_->send(conn, deltaMsg(convId, msgId, std::string(d)));
    };
    sink.onDone = [this, conn, convId, msgId](const std::string& reason, int tokens) {
        double tps = 0.0;
        if (Gen* g = genFor(conn, convId)) {
            tps = tokensPerSec(tokens, g->startMs, nowMs_);
            conv(conn, convId).addAssistant(g->acc);
        }
        if (room_) room_->send(conn, doneMsg(convId, msgId, reason.c_str(), tokens, tps));
        removeGen(conn, convId, /*abort=*/false);
    };
    sink.onError = [this, conn, convId, msgId](const std::string& reason) {
        (void)msgId;
        if (room_) room_->send(conn, errorMsg(convId, reason.c_str()));
        removeGen(conn, convId, /*abort=*/false);
    };

    chat::LlamaClient::ReqId rid = llama_->start(requestBody(conv(conn, convId)), std::move(sink));
    if (rid == 0) {  // couldn't even open a socket
        room.send(conn, errorMsg(convId, "backend_down"));
        removeGen(conn, convId, /*abort=*/false);
        return;
    }
    if (Gen* g = genFor(conn, convId)) g->reqId = rid;
}

void ChatEngine::onTick(Room& room, std::uint64_t nowMs) {
    nowMs_ = nowMs;
    if (process_) process_->tick(nowMs);  // spawn/health/restart the child (§3.A)
    if (llama_) llama_->pump();            // drives Llama-mode sinks (may remove gens)

    // Stub-mode token timer (Llama gens are driven by pump, not here).
    for (std::size_t i = 0; i < gens_.size();) {
        Gen& g = gens_[i];
        if (!g.stub) {
            ++i;
            continue;
        }
        if (g.startMs == 0) g.startMs = nowMs;

        while (g.next < g.tokens.size() && nowMs >= g.nextMs) {
            const std::string& tok = g.tokens[g.next];
            room.send(g.conn, deltaMsg(g.convId, g.msgId, tok));
            g.acc += tok;
            ++g.next;
            ++g.emitted;  // mirrors the Llama sink so the admin stats panel is uniform
            g.nextMs = nowMs + kStubIntervalMs;
        }

        if (g.next >= g.tokens.size()) {
            conv(g.conn, g.convId).addAssistant(g.acc);
            room.send(g.conn, doneMsg(g.convId, g.msgId, "eos", static_cast<int>(g.next),
                                      tokensPerSec(static_cast<int>(g.next), g.startMs, nowMs)));
            gens_.erase(gens_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

std::string ChatEngine::statusLine() const {
    const char* backend = mode_ == Mode::Llama ? "llama" : (mode_ == Mode::Stub ? "stub" : "down");
    std::string health;
    if (process_)
        health = process_->ready() ? ", up"
                                   : (std::string(", ") + process_->lastError());
    std::size_t convCount = 0;
    for (const auto& kv : convs_) convCount += kv.second.size();
    return model_ + " (" + backend + health + ") - " + std::to_string(convCount) +
           " conversations, " + std::to_string(gens_.size()) + " generating";
}

std::string ChatEngine::pathForModel(const std::string& id) const {
    auto it = modelPaths_.find(id);
    return it != modelPaths_.end() ? it->second : std::string();
}

void ChatEngine::loadPendingModel() {
    loadError_.clear();
    if (process_) {
        // Spawn mode: actually reload llama-server with the selected GGUF (§5: one
        // model at a time, switch = restart the child). Without a configured path
        // we can't load the weights (operator-supplied, §11) — refuse and bail
        // WITHOUT relabelling: the child is still running the old model, so
        // clients must not be told the switch happened.
        std::string path = pathForModel(pendingModel_);
        if (path.empty()) {
            loadError_ = "no GGUF configured for " + pendingModel_ +
                         " (pass --model-path " + pendingModel_ + "=/path.gguf)";
            return;
        }
        model_ = pendingModel_;
        process_->setContextSize(contextMax_);
        process_->switchModel(path, nowMs_);
    } else {
        // Stub / connect-only (--llama-url) / down: no child to reload, so
        // committing the id only relabels (the request body's "model" field and
        // the advertised name). Connect-only says so, since it can mislead.
        model_ = pendingModel_;
        if (mode_ == Mode::Llama) {
            loadError_ = "external server: relabelled only (cannot reload)";
        }
    }
    // Push the new active model to every client (server-authoritative, §7/§9).
    if (room_) {
        room_->broadcast(configJson());
    }
}

void ChatEngine::stopAll() {
    // Abort every in-flight generation as if each client had sent chatStop: keep
    // the partial reply, tell the client, cancel the backend request. The
    // conversations are preserved (reset() is the destructive "new game"). Safe to
    // abort here — the admin path, not inside LlamaClient::pump().
    for (Gen& g : gens_) {
        if (!g.acc.empty()) {
            conv(g.conn, g.convId).addAssistant(g.acc);
        }
        if (room_) {
            room_->send(g.conn, doneMsg(g.convId, g.msgId, "stopped", g.emitted,
                                        tokensPerSec(g.emitted, g.startMs, nowMs_)));
        }
        if (g.reqId && llama_) {
            llama_->cancel(g.reqId);
        }
    }
    gens_.clear();
}

void ChatEngine::reset() {
    // Admin "new game": stop all generation and clear every conversation. Outside
    // pump(), so aborting the live requests is safe.
    if (llama_)
        for (const auto& g : gens_)
            if (g.reqId) llama_->cancel(g.reqId);
    gens_.clear();
    convs_.clear();
}

void ChatEngine::drawAdmin() {
#ifdef VILLEN_ADMIN_UI
    // The engine's own panel body only — the shell owns the chrome (admin-shell
    // §8). Default ImGui font is ASCII-only, so keep text ASCII. The model + every
    // sampling param are server-authoritative (§9). Privacy by construction: this
    // console shows metadata, never message content (§9/§11).
    drawAdmin_ModelPanel();
    ImGui::Spacing();
    drawAdmin_ParamsPanel();
    ImGui::Spacing();
    drawAdmin_StatsPanel();
#endif
}

#ifdef VILLEN_ADMIN_UI
namespace {
// Health-line colours, matching the seat-status palette in admin_ui.cpp: green
// good, amber transitional, grey idle/down.
const ImVec4 kGood{0.50f, 0.86f, 0.50f, 1.0f};
const ImVec4 kWarn{0.95f, 0.75f, 0.35f, 1.0f};
const ImVec4 kIdle{0.60f, 0.60f, 0.60f, 1.0f};
const ImVec4 kErr {0.95f, 0.55f, 0.35f, 1.0f};
}  // namespace

void ChatEngine::drawAdmin_ModelPanel() {
    ImGui::SeparatorText("Model");

    // The combo picks a *pending* model; Load/Switch commits it (§9) — so the
    // operator chooses, then deliberately triggers the multi-second reload.
    const chat::ModelInfo* pend = chat::findModel(pendingModel_);
    const char* preview = pend ? pend->displayName.c_str() : pendingModel_.c_str();
    if (ImGui::BeginCombo("Select", preview)) {
        for (const auto& m : chat::knownModels()) {
            // In spawn mode a model needs a configured GGUF to be loadable (§11);
            // show the others but disable them so the choice is honest.
            const bool loadable = !process_ || !pathForModel(m.id).empty();
            const bool sel = (m.id == pendingModel_);
            ImGui::BeginDisabled(!loadable);
            std::string label = m.displayName;
            if (!loadable) {
                label += "  (no GGUF)";
            }
            if (ImGui::Selectable(label.c_str(), sel)) {
                pendingModel_ = m.id;
            }
            ImGui::EndDisabled();
            if (sel) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const bool busy = process_ && process_->switching();
    ImGui::BeginDisabled(mode_ == Mode::Down || busy);
    if (ImGui::Button(pendingModel_ == model_ ? "Reload" : "Load / Switch")) {
        loadPendingModel();
    }
    ImGui::EndDisabled();

    ImGui::Text("active: %s", model_.c_str());
    if (!loadError_.empty()) {
        ImGui::TextColored(kErr, "%s", loadError_.c_str());
    }

    // --- llama-server health (§9): PID, offload, memory, context, last error ---
    if (process_) {
        const chat::LlamaProcess& p = *process_;
        const char* state = p.ready()       ? "ready"
                            : p.paused()     ? "unloaded"
                            : p.switching()  ? "reloading"
                            : p.running()    ? "loading"
                                             : "down";
        const ImVec4& col = p.ready() ? kGood : (p.paused() ? kIdle : kWarn);
        ImGui::TextColored(col, "llama-server: %s", state);
        if (p.running()) {
            // -ngl>0 requests GPU offload (Vulkan on the Deck — confirm the real
            // radeonsi vs llvmpipe in System Info, steamdeck-debugging §4); this
            // reflects the spawn request, not a runtime probe.
            ImGui::Text("pid %d   offload: %s (-ngl %d)   slots %d",
                        static_cast<int>(p.pid()), p.ngl() > 0 ? "GPU" : "CPU",
                        p.ngl(), p.parallel());
        }
        const std::size_t kb = p.residentKb();
        if (kb > 0) {
            ImGui::Text("memory: %.0f MB   context: %d tok",
                        static_cast<double>(kb) / 1024.0, p.ctxSize());
        } else {
            ImGui::Text("memory: n/a       context: %d tok", p.ctxSize());
        }
        if (p.switchLatencyMs() > 0) {
            ImGui::Text("last reload: %.1f s",
                        static_cast<double>(p.switchLatencyMs()) / 1000.0);
        }
        if (!p.ready() && !p.lastError().empty()) {
            ImGui::TextColored(kErr, "note: %s", p.lastError().c_str());
        }
        if (ImGui::Button("Restart")) {
            process_->restart(nowMs_);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(p.paused());
        if (ImGui::Button("Unload")) {
            process_->stop(nowMs_);
        }
        ImGui::EndDisabled();
    } else if (mode_ == Mode::Llama) {
        ImGui::TextDisabled("external llama-server (connect mode; not managed here)");
    } else if (mode_ == Mode::Stub) {
        ImGui::TextDisabled("stub backend: echoes input, no inference");
    } else {
        ImGui::TextDisabled("no backend (run --engine chat with --llama-bin,");
        ImGui::TextDisabled("  --llama-url, or --chat-stub)");
    }
}

void ChatEngine::drawAdmin_ParamsPanel() {
    ImGui::SeparatorText("Generation");
    // Server-authoritative sampling (§7/§9): these feed every request body and
    // apply to the next chatSend; in-flight generations keep their own params.
    ImGui::SliderFloat("Temperature", &temperature_, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Top-p", &topP_, 0.0f, 1.0f, "%.2f");
    ImGui::SliderInt("Top-k", &topK_, 0, 200);
    ImGui::SliderInt("Max tokens", &maxTokens_, 16, 4096);
    ImGui::SliderFloat("Repeat penalty", &repeatPenalty_, 1.0f, 2.0f, "%.2f");

    // Context window: both the host's token cap (capToTokens, §5) and the next
    // llama-server -c. Broadcast on release (not every drag frame) since clients
    // read contextMax from chatConfig (§7); the backend picks it up on next start.
    ImGui::SliderInt("Context (tok)", &contextMax_, 512, 32768);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (process_) {
            process_->setContextSize(contextMax_);
        }
        if (room_) {
            room_->broadcast(configJson());
        }
    }

    ImGui::SeparatorText("System prompt");
    // Operator-set (§9): prepended to every conversation as the system turn and
    // preserved across chatReset; applies to the next chatSend per conversation.
    ImGui::InputTextMultiline("##sysprompt", &systemPrompt_, ImVec2(-FLT_MIN, 72.0f));
    ImGui::TextDisabled("applied on the next message per conversation");
}

void ChatEngine::drawAdmin_StatsPanel() {
    ImGui::SeparatorText("Live");
    std::size_t convCount = 0;
    for (const auto& kv : convs_) {
        convCount += kv.second.size();
    }
    const int slots = process_ ? process_->parallel() : 0;

    std::string head = "conversations: " + std::to_string(convCount) +
                       "    generating: " + std::to_string(gens_.size());
    if (slots > 0) {
        head += " / " + std::to_string(slots) + " slots";
    }
    ImGui::TextUnformatted(head.c_str());
    // No real request queue yet (§8 is a later step): beyond the in-flight count a
    // second send per conversation is rejected with model_busy, not enqueued.

    // Per-generation metadata — id, counts, rate. NEVER the message text (§9/§11).
    if (!gens_.empty() &&
        ImGui::BeginTable("gens", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("conn");
        ImGui::TableSetupColumn("conv");
        ImGui::TableSetupColumn("tokens");
        ImGui::TableSetupColumn("tok/s");
        ImGui::TableSetupColumn("elapsed");
        ImGui::TableHeadersRow();
        for (const Gen& g : gens_) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", static_cast<unsigned long long>(g.conn));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(g.convId.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", g.emitted);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.1f", tokensPerSec(g.emitted, g.startMs, nowMs_));
            ImGui::TableSetColumnIndex(4);
            const double sec = (g.startMs && nowMs_ > g.startMs)
                                   ? static_cast<double>(nowMs_ - g.startMs) / 1000.0
                                   : 0.0;
            ImGui::Text("%.1f s", sec);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(gens_.empty());
    if (ImGui::Button("Stop all")) {
        stopAll();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(gens_.empty() && convCount == 0);
    if (ImGui::Button("Clear all")) {
        reset();
    }
    ImGui::EndDisabled();
}
#endif  // VILLEN_ADMIN_UI

}  // namespace villen
