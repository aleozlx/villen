#include "chat_engine.hpp"

#include <algorithm>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "room.hpp"

#ifdef VILLEN_ADMIN_UI
#include "imgui.h"
#endif

using json = nlohmann::json;

namespace villen {
namespace {

// --- §7 server -> client frames ---------------------------------------------

std::string deltaMsg(const std::string& convId, int msgId, const std::string& delta) {
    json msg = {{"type", "chatDelta"}, {"convId", convId}, {"msgId", msgId},
                {"delta", delta}};
    return msg.dump();
}

std::string doneMsg(const std::string& convId, int msgId, const char* stopReason,
                    std::size_t tokens, double tps) {
    json msg = {{"type", "chatDone"}, {"convId", convId}, {"msgId", msgId},
                {"stopReason", stopReason}, {"tokens", tokens}, {"tps", tps}};
    return msg.dump();
}

std::string errorMsg(const std::string& convId, const char* reason) {
    json msg = {{"type", "chatError"}, {"convId", convId}, {"reason", reason}};
    return msg.dump();
}

// Split a reply into stream tokens, each carrying its trailing space, so the
// client reconstructs the text by simple concatenation. Stands in for the real
// tokenizer until step 3 (the stub emits these on the onTick clock).
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

double tokensPerSec(std::size_t emitted, std::uint64_t startMs, std::uint64_t nowMs) {
    if (startMs == 0 || nowMs <= startMs) return 0.0;
    return static_cast<double>(emitted) * 1000.0 / static_cast<double>(nowMs - startMs);
}

}  // namespace

chat::Conversation& ChatEngine::conv(ConnId conn, const std::string& convId) {
    return convs_[conn][convId];
}

ChatEngine::Gen* ChatEngine::genFor(ConnId conn, const std::string& convId) {
    for (auto& g : gens_)
        if (g.conn == conn && g.convId == convId) return &g;
    return nullptr;
}

void ChatEngine::cancel(ConnId conn, const std::string& convId) {
    gens_.erase(std::remove_if(gens_.begin(), gens_.end(),
                               [&](const Gen& g) {
                                   return g.conn == conn && g.convId == convId;
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
                {"ready", true}};  // stub is always ready; step 3 reflects the backend
    return msg.dump();
}

void ChatEngine::onJoin(Room& room, ConnId conn, SeatId) {
    // Every connection learns the active model + context window on join (§7).
    room.send(conn, configJson());
}

void ChatEngine::onLeave(Room&, ConnId conn, SeatId) {
    // Privacy (§11): a dropped connection's conversations and any in-flight
    // generation vanish with it — never persisted, never visible to anyone else.
    convs_.erase(conn);
    gens_.erase(std::remove_if(gens_.begin(), gens_.end(),
                               [conn](const Gen& g) { return g.conn == conn; }),
                gens_.end());
}

void ChatEngine::onMessage(Room& room, ConnId conn, SeatId, std::string_view text) {
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("type")) {
        room.send(conn, errorMsg("", "bad_message"));
        return;
    }
    const std::string type = j.value("type", "");
    const std::string convId = j.value("convId", "");

    if (type == "chatSend") {
        if (convId.empty() || !j.contains("text") || !j["text"].is_string()) {
            room.send(conn, errorMsg(convId, "bad_message"));
            return;
        }
        // Turn-of-generation: one in-flight reply per conversation (§7/§8). A
        // second send while generating is rejected (queueing is a step-8 concern).
        if (genFor(conn, convId)) {
            room.send(conn, errorMsg(convId, "model_busy"));
            return;
        }
        const std::string userText = j["text"].get<std::string>();
        chat::Conversation& c = conv(conn, convId);
        c.setSystem(systemPrompt_);
        c.addUser(userText);
        c.capToTokens(static_cast<std::size_t>(contextMax_));

        Gen g;
        g.conn = conn;
        g.convId = convId;
        g.msgId = nextMsgId_++;
        g.tokens = tokenize("you said: " + userText);  // STUB reply (§14 step 2)
        g.next = 0;
        g.nextMs = 0;   // first token on the next tick → low time-to-first-token
        g.startMs = 0;  // stamped on first emit
        gens_.push_back(std::move(g));
        return;
    }

    if (type == "chatReset") {
        cancel(conn, convId);
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
            room.send(conn, doneMsg(convId, g->msgId, "stopped", g->next,
                                    tokensPerSec(g->next, g->startMs, nowMs_)));
            cancel(conn, convId);
        }
        return;
    }

    room.send(conn, errorMsg(convId, "bad_message"));
}

void ChatEngine::onTick(Room& room, std::uint64_t nowMs) {
    nowMs_ = nowMs;
    for (std::size_t i = 0; i < gens_.size();) {
        Gen& g = gens_[i];
        if (g.startMs == 0) g.startMs = nowMs;

        // Emit every token whose time has come this tick (catches up if the loop
        // slept longer than one interval).
        while (g.next < g.tokens.size() && nowMs >= g.nextMs) {
            const std::string& tok = g.tokens[g.next];
            room.send(g.conn, deltaMsg(g.convId, g.msgId, tok));
            g.acc += tok;
            ++g.next;
            g.nextMs = nowMs + kStubIntervalMs;
        }

        if (g.next >= g.tokens.size()) {
            // Record the assistant turn so the next user message has context.
            conv(g.conn, g.convId).addAssistant(g.acc);
            room.send(g.conn, doneMsg(g.convId, g.msgId, "eos", g.next,
                                      tokensPerSec(g.next, g.startMs, nowMs)));
            gens_.erase(gens_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

std::string ChatEngine::statusLine() const {
    std::size_t convCount = 0;
    for (const auto& kv : convs_) convCount += kv.second.size();
    return model_ + " (stub) - " + std::to_string(convCount) + " conversations, " +
           std::to_string(gens_.size()) + " generating";
}

void ChatEngine::reset() {
    // Admin "new game": stop all generation and clear every conversation.
    gens_.clear();
    convs_.clear();
}

void ChatEngine::drawAdmin() {
#ifdef VILLEN_ADMIN_UI
    // The engine's own panel body only — the shell owns the chrome (admin-shell
    // §8). Default ImGui font is ASCII-only, so keep text ASCII. Model + params
    // are server-authoritative (§9). The full model console (load/switch, health,
    // queue depth, the system-prompt editor) is step 6; this is the step-2 stub.
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
    if (ImGui::Button("Stop all")) gens_.clear();
    ImGui::SameLine();
    if (ImGui::Button("Clear conversations")) reset();
#endif
}

}  // namespace villen
