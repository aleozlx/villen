// Villen chat engine — conversation state (the pure, testable core, DESIGN-chat §2/§4).
//
// This is the upstream-of-the-model half of `chat`: a deterministic
// conversation state machine — append turns, reset, cap context. It speaks no
// network and no inference; it holds the per-conversation {role, content} turns
// the host hands to llama-server (§3.A) and the templating layer (prompt.hpp)
// renders. There is no deterministic oracle for *generation* (§2), so this — and
// the templates — is what gets the same CI treatment as chess: pure data in,
// exact strings out, no GPU and no model.
//
// Keep this dependency-free (no llama, no json, no I/O) so `villen_chat` stays a
// drop-in core like `villen_engine`.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace villen::chat {

enum class Role { System, User, Assistant };

const char* roleName(Role r);  // "system" | "user" | "assistant"

struct Turn {
    Role role{Role::User};
    std::string content;
};

inline bool operator==(const Turn& a, const Turn& b) {
    return a.role == b.role && a.content == b.content;
}
inline bool operator!=(const Turn& a, const Turn& b) {
    return !(a == b);
}

// One conversation's state. The system prompt is held separately from the
// dialogue (it is operator-set, §9, and survives reset); the dialogue is the
// alternating user/assistant turns. `messages()` flattens the two into the
// ordered list the host serialises to /v1/chat/completions, and prompt.hpp
// renders for the fallback raw-prompt path.
class Conversation {
 public:
    // Operator-set system prompt (§9). Empty string clears it.
    void setSystem(std::string content);
    const std::string& system() const { return system_; }
    bool hasSystem() const { return !system_.empty(); }

    void addUser(std::string content);
    void addAssistant(std::string content);
    void append(Role role, std::string content);  // System routes to setSystem

    // chatReset (§7): drop the dialogue. The system prompt is operator state, so
    // it is preserved; pass clearSystem=true for a full wipe.
    void reset(bool clearSystem = false);

    const std::vector<Turn>& turns() const { return turns_; }  // dialogue only
    std::size_t size() const { return turns_.size(); }
    bool empty() const { return turns_.empty(); }

    // [system?] + dialogue, in order — the messages array the host sends to
    // llama-server, and prompt.hpp iterates for the fallback template.
    std::vector<Turn> messages() const;

    // --- Context cap (§5) -----------------------------------------------------
    // The pure core has no tokenizer (that lives in llama-server), so the cap is
    // an *approximate* char-based estimate — enough to bound memory growth; the
    // backend enforces the hard limit. Open question §17: sliding-window vs.
    // summarization. MVP is the sliding window below.

    // ~tokens for a string: ceil(chars / kCharsPerToken). Deterministic.
    static std::size_t estimateTokens(std::string_view text);

    // Estimate for the whole conversation: every message's content plus a fixed
    // per-message overhead for the template's role delimiters.
    std::size_t estimatedTokens() const;

    // Drop oldest dialogue turns (preserving the system prompt and at least the
    // most recent turn) until estimatedTokens() <= budget, then drop a leading
    // assistant turn so the dialogue still opens on a user turn (Mistral, §4,
    // requires user-first). Returns the number of turns dropped.
    std::size_t capToTokens(std::size_t budget);

    static constexpr std::size_t kCharsPerToken = 4;       // rough GPT-ish ratio
    static constexpr std::size_t kPerMessageOverhead = 4;  // role delimiters

 private:
    std::string system_;
    std::vector<Turn> turns_;
};

}  // namespace villen::chat
