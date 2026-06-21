#include "villen/chat/prompt.hpp"

namespace villen::chat {

const char* familyName(ModelFamily f) {
    switch (f) {
        case ModelFamily::Llama3:
            return "llama3";
        case ModelFamily::ChatML:
            return "chatml";
        case ModelFamily::Mistral:
            return "mistral";
    }
    return "chatml";
}

const std::vector<ModelInfo>& knownModels() {
    // Qwen2.5 first: Apache-2.0 makes it the natural first model to wire (§11/§14).
    static const std::vector<ModelInfo> kModels = {
        {"qwen2.5-7b-instruct", "Qwen2.5 7B Instruct", ModelFamily::ChatML},
        {"llama-3.1-8b-instruct", "Llama 3.1 8B Instruct", ModelFamily::Llama3},
        {"mistral-7b-instruct", "Mistral 7B Instruct", ModelFamily::Mistral},
    };
    return kModels;
}

const ModelInfo* findModel(std::string_view id) {
    for (const auto& m : knownModels()) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

const ModelInfo* matchModelByFilename(std::string_view filename) {
    std::string lc(filename);
    for (char& c : lc) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    for (const auto& m : knownModels()) {
        if (lc.find(m.id) != std::string::npos) {
            return &m;
        }
    }
    return nullptr;
}

const std::vector<std::string>& stopTokens(ModelFamily f) {
    static const std::vector<std::string> kLlama3 = {"<|eot_id|>"};
    static const std::vector<std::string> kChatML = {"<|im_end|>"};
    static const std::vector<std::string> kMistral = {"</s>"};
    switch (f) {
        case ModelFamily::Llama3:
            return kLlama3;
        case ModelFamily::ChatML:
            return kChatML;
        case ModelFamily::Mistral:
            return kMistral;
    }
    return kChatML;
}

namespace {

// Llama 3.1 — Llama-3 header format (§4). BOS once, each turn wrapped in
// <|start_header_id|>{role}<|end_header_id|>\n\n … <|eot_id|>, ending with the
// assistant header to prompt generation.
std::string renderLlama3(const Conversation& conv) {
    auto block = [](const char* role, const std::string& content) {
        return std::string("<|start_header_id|>") + role + "<|end_header_id|>\n\n" + content +
               "<|eot_id|>";
    };
    std::string out = "<|begin_of_text|>";
    if (conv.hasSystem()) {
        out += block("system", conv.system());
    }
    for (const auto& t : conv.turns()) {
        out += block(roleName(t.role), t.content);
    }
    out += "<|start_header_id|>assistant<|end_header_id|>\n\n";
    return out;
}

// Qwen2.5 — ChatML (§4): <|im_start|>{role}\n … <|im_end|>\n, ending on an open
// assistant turn.
std::string renderChatML(const Conversation& conv) {
    auto block = [](const char* role, const std::string& content) {
        return std::string("<|im_start|>") + role + "\n" + content + "<|im_end|>\n";
    };
    std::string out;
    if (conv.hasSystem()) {
        out += block("system", conv.system());
    }
    for (const auto& t : conv.turns()) {
        out += block(roleName(t.role), t.content);
    }
    out += "<|im_start|>assistant\n";
    return out;
}

// Mistral 7B — classic [INST] format (§4): one BOS, the system prompt folded
// into the first user turn, </s> after each assistant turn, ending on an open
// [/INST] so the model emits the next reply.
std::string renderMistral(const Conversation& conv) {
    std::string out = "<s>";
    bool systemFolded = false;
    for (const auto& t : conv.turns()) {
        if (t.role == Role::User) {
            std::string user = t.content;
            if (!systemFolded && conv.hasSystem()) {
                user = conv.system() + "\n\n" + user;
                systemFolded = true;
            }
            out += "[INST] " + user + " [/INST]";
        } else {  // assistant
            out += t.content + "</s>";
        }
    }
    return out;
}

}  // namespace

std::string renderPrompt(ModelFamily f, const Conversation& conv) {
    switch (f) {
        case ModelFamily::Llama3:
            return renderLlama3(conv);
        case ModelFamily::ChatML:
            return renderChatML(conv);
        case ModelFamily::Mistral:
            return renderMistral(conv);
    }
    return renderChatML(conv);
}

}  // namespace villen::chat
