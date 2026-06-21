// Villen chat engine — per-model prompt templating (the pure, testable core, §4).
//
// The three target models use three different chat formats and stop tokens (§4).
// The *primary* path is to send structured `messages` to llama-server and let it
// apply the GGUF's own `tokenizer.chat_template` (§4) — the host never touches
// these special-token strings. This table is the *fallback / override* (§4): for
// GGUFs whose embedded template is missing or wrong, render the raw prompt
// ourselves. It is pure string assembly, and it is the unit-tested core (§2):
// "given this conversation and this model id, produce exactly this prompt."
//
// Re-implementing three templates invites drift (§16), so this stays small and
// is pinned by exact-string doctest cases; it is deliberately *not* the default.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "villen/chat/conversation.hpp"

namespace villen::chat {

// The chat-format family a model belongs to (§4). Distinct from the model id:
// several GGUFs can share a family.
enum class ModelFamily { Llama3, ChatML, Mistral };

const char* familyName(ModelFamily f);  // "llama3" | "chatml" | "mistral"

// A model the operator can select (§7 chatConfig.models / admin combo, §9).
struct ModelInfo {
    std::string id;           // wire id, e.g. "qwen2.5-7b-instruct"
    std::string displayName;  // human label, e.g. "Qwen2.5 7B Instruct"
    ModelFamily family;
};

// The three target models (§4/§11). Qwen2.5 (Apache-2.0) is first to wire (§14).
const std::vector<ModelInfo>& knownModels();

// Look up a model by wire id; nullptr if unknown.
const ModelInfo* findModel(std::string_view id);

// Match a GGUF filename to a known model: the wire id appears (case-insensitively)
// as a substring of the filename, e.g. "Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf" ->
// llama-3.1-8b-instruct. Lets the host map operator-supplied GGUFs to models for
// switching (§5) without a hardcoded path table. nullptr if it matches none.
const ModelInfo* matchModelByFilename(std::string_view filename);

// Stop tokens for a family (§4). The backend reports the authoritative stops
// from the GGUF; this matches them for the fallback path so generation never
// leaks a trailing special token into the reply (acceptance §4).
const std::vector<std::string>& stopTokens(ModelFamily f);

// Render `conv` to the raw prompt string a model of this family expects,
// terminated with the assistant generation-prompt header so the model continues
// the assistant turn. Includes the system prompt per the family's convention
// (Llama-3/ChatML: own system block; Mistral: folded into the first user turn).
std::string renderPrompt(ModelFamily f, const Conversation& conv);

}  // namespace villen::chat
