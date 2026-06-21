#include "chat_engine.hpp"

#include <dirent.h>

#include <algorithm>
#include <cctype>
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

// Lowercased copy, for the case-insensitive ".gguf" suffix test in scanModels.
// (Model-name matching itself lives in the pure core: chat::matchModelByFilename.)
std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

ChatEngine::ChatEngine(ChatBackendConfig cfg) {
    if (!cfg.llamaBin.empty()) {
        // Spawn-and-manage: the host owns the llama-server child (§3.A). Only here
        // do we build the switchable-model registry (§5) — switching is meaningful
        // only for a server we control.
        mode_ = Mode::Llama;
        int port = cfg.llamaPort > 0 ? cfg.llamaPort : 8080;

        if (!cfg.modelsDir.empty()) scanModels(cfg.modelsDir);
        // The explicit --model is the initial active model; make sure it's in the
        // registry (its path wins) and adopt its id. Otherwise default to the first
        // discovered model, else keep the hardcoded model_ default.
        std::string activePath = cfg.model;
        if (!activePath.empty()) {
            const std::string base = activePath.substr(activePath.find_last_of('/') + 1);
            if (const chat::ModelInfo* mi = chat::matchModelByFilename(base)) {
                addAvailable(*mi, activePath);
                model_ = mi->id;
            }
        } else if (!available_.empty()) {
            activePath = available_.front().path;
            model_ = available_.front().id;
        }

        chat::LlamaSpawnConfig sp;
        sp.bin = cfg.llamaBin;
        sp.model = activePath;
        sp.host = cfg.llamaHost;
        sp.port = port;
        sp.ngl = cfg.ngl;
        sp.parallel = cfg.parallel;
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

bool ChatEngine::backendReady() const {
    // A spawned server must pass /health first; a connect-only backend is optimistic
    // (errors surface as backend_down); Down (and Stub-less) is never ready.
    return mode_ == Mode::Stub ||
           (mode_ == Mode::Llama && (!process_ || process_->ready()));
}

std::string ChatEngine::configJson() const {
    // Advertise the models the operator can actually switch to (§5/§7). Outside
    // spawn mode there's no registry, so fall back to just the active id.
    json models = json::array();
    for (const auto& m : available_) models.push_back(m.id);
    if (models.empty()) models.push_back(model_);
    json msg = {{"type", "chatConfig"},
                {"model", model_},
                {"models", std::move(models)},
                {"contextMax", contextMax_},
                {"ready", backendReady()}};
    return msg.dump();
}

void ChatEngine::scanModels(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    std::string base = dir;
    if (!base.empty() && base.back() != '/') base += '/';
    while (dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name.size() < 5 || toLower(name).rfind(".gguf") != name.size() - 5) continue;
        if (const chat::ModelInfo* mi = chat::matchModelByFilename(name)) addAvailable(*mi, base + name);
    }
    ::closedir(d);
    // Stable order = knownModels() order (Qwen first), so the default active model
    // and the admin combo are deterministic regardless of readdir() order.
    std::sort(available_.begin(), available_.end(),
              [](const AvailableModel& a, const AvailableModel& b) {
                  auto rank = [](const std::string& id) {
                      const auto& k = chat::knownModels();
                      for (std::size_t i = 0; i < k.size(); ++i)
                          if (k[i].id == id) return i;
                      return k.size();
                  };
                  return rank(a.id) < rank(b.id);
              });
}

void ChatEngine::addAvailable(const chat::ModelInfo& mi, std::string path) {
    for (auto& m : available_)
        if (m.id == mi.id) { m.path = std::move(path); return; }  // dedup; newest path wins
    available_.push_back({mi.id, mi.displayName, std::move(path), mi.family});
}

const ChatEngine::AvailableModel* ChatEngine::findAvailable(const std::string& id) const {
    for (const auto& m : available_)
        if (m.id == id) return &m;
    return nullptr;
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
    // These fire from pump() in onTick. room_ is normally valid then, but it is
    // nulled by detach() at engine teardown — guard so a sink that pump() runs
    // after detach can't dereference a dead Room.
    chat::StreamSink sink;
    sink.onDelta = [this, conn, convId, msgId](std::string_view d) {
        if (Gen* g = genFor(conn, convId)) g->acc.append(d);
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

    // Tell clients when backend readiness flips — the model finished loading (after
    // spawn or a switch) or went away — so they learn it without polling through
    // backend_down. Edge-triggered: broadcast only on the transition.
    bool ready = backendReady();
    if (ready != readyBroadcast_) {
        readyBroadcast_ = ready;
        room.broadcast(configJson());
    }
}

void ChatEngine::collectPollFds(std::vector<int>& out) {
    // Llama-mode generation sockets: let the host's poll() wake on an inbound
    // token so onTick→pump() drains it at once, instead of at the poll cadence.
    // Stub/Down have no sockets; their gens advance on the onTick clock anyway.
    if (llama_) llama_->collectFds(out);
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

void ChatEngine::reset() {
    // Admin "new game": stop all generation and clear every conversation. Outside
    // pump(), so aborting the live requests is safe.
    if (llama_)
        for (const auto& g : gens_)
            if (g.reqId) llama_->cancel(g.reqId);
    gens_.clear();
    convs_.clear();
}

bool ChatEngine::setActiveModel(const std::string& id) {
    const AvailableModel* m = findAvailable(id);
    if (!m || !process_) return false;  // unknown id, or we don't manage the server
    if (id == model_) return true;      // already active
    model_ = m->id;
    process_->setModel(m->path);        // respawn with the new -m; ready() -> false

    // The backend is reloading, so abort every in-flight generation (outside pump(),
    // so cancelling the live requests is safe) and tell each client its turn ended;
    // clients retry through backend_down until the new model is up.
    for (const auto& g : gens_) {
        if (g.reqId && llama_) llama_->cancel(g.reqId);
        if (room_) room_->send(g.conn, errorMsg(g.convId, "backend_down"));
    }
    gens_.clear();

    // Re-handshake every client with the new model (ready:false now); onTick
    // pushes ready:true again once the new model finishes loading.
    if (room_) room_->broadcast(configJson());
    readyBroadcast_ = backendReady();
    return true;
}

void ChatEngine::cycleModel() {
    if (available_.size() < 2) return;
    std::size_t i = 0;
    for (; i < available_.size(); ++i)
        if (available_[i].id == model_) break;
    setActiveModel(available_[(i + 1) % available_.size()].id);
}

void ChatEngine::drawAdmin() {
#ifdef VILLEN_ADMIN_UI
    // The engine's own panel body only — the shell owns the chrome (admin-shell
    // §8). Default ImGui font is ASCII-only, so keep text ASCII. Model + params
    // are server-authoritative (§9). The full model console (load/switch, health,
    // queue depth, the system-prompt editor) is step 6; this is a minimal console.
    // Switch among the models we actually have a GGUF for (§5). setActiveModel does
    // the real respawn + client re-handshake. The full console (params, queue depth,
    // system-prompt editor) is step 6; this stays a minimal combo.
    if (ImGui::BeginCombo("Model", model_.c_str())) {
        for (const auto& m : available_) {
            bool sel = (m.id == model_);
            if (ImGui::Selectable(m.displayName.c_str(), sel)) setActiveModel(m.id);
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
