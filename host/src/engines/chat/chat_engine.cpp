#include "chat_engine.hpp"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "room.hpp"

#ifdef VILLEN_ADMIN_UI
#include "imgui.h"
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
    if (cfg.llamaPort > 0) {
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
    for (auto& g : gens_)
        if (g.conn == conn && g.convId == convId) return &g;
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
    json msg = {{"type", "chatConfig"},
                {"model", model_},
                {"models", std::move(models)},
                {"contextMax", contextMax_},
                {"ready", mode_ != Mode::Down}};
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
                 {"max_tokens", maxTokens_}};
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
            room.send(conn, doneMsg(convId, g->msgId, "stopped",
                                    static_cast<int>(g->next ? g->next : g->acc.size()),
                                    tokensPerSec(static_cast<int>(g->next), g->startMs, nowMs_)));
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
    chat::StreamSink sink;
    sink.onDelta = [this, conn, convId, msgId](std::string_view d) {
        if (Gen* g = genFor(conn, convId)) g->acc.append(d);
        room_->send(conn, deltaMsg(convId, msgId, std::string(d)));
    };
    sink.onDone = [this, conn, convId, msgId](const std::string& reason, int tokens) {
        double tps = 0.0;
        if (Gen* g = genFor(conn, convId)) {
            tps = tokensPerSec(tokens, g->startMs, nowMs_);
            conv(conn, convId).addAssistant(g->acc);
        }
        room_->send(conn, doneMsg(convId, msgId, reason.c_str(), tokens, tps));
        removeGen(conn, convId, /*abort=*/false);
    };
    sink.onError = [this, conn, convId, msgId](const std::string& reason) {
        (void)msgId;
        room_->send(conn, errorMsg(convId, reason.c_str()));
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
    if (llama_) llama_->pump();  // drives Llama-mode sinks (may remove gens)

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
    std::size_t convCount = 0;
    for (const auto& kv : convs_) convCount += kv.second.size();
    return model_ + " (" + backend + ") - " + std::to_string(convCount) +
           " conversations, " + std::to_string(gens_.size()) + " generating";
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
    // §8). Default ImGui font is ASCII-only, so keep text ASCII. Model + params
    // are server-authoritative (§9). The full model console (load/switch, health,
    // queue depth, the system-prompt editor) is step 6; this is a minimal console.
    const auto& models = chat::knownModels();
    if (ImGui::BeginCombo("Model", model_.c_str())) {
        for (const auto& m : models) {
            bool sel = (m.id == model_);
            if (ImGui::Selectable(m.displayName.c_str(), sel)) {
                model_ = m.id;
                if (room_) room_->broadcast(configJson());  // push to all clients
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SliderFloat("Temperature", &temperature_, 0.0f, 2.0f, "%.2f");
    ImGui::Spacing();
    ImGui::TextUnformatted(statusLine().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Stop all")) reset();
#endif
}

}  // namespace villen
