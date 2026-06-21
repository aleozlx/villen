#include "villen/chat/conversation.hpp"

#include <utility>

namespace villen::chat {

const char* roleName(Role r) {
    switch (r) {
        case Role::System:    return "system";
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
    }
    return "user";
}

void Conversation::setSystem(std::string content) { system_ = std::move(content); }

void Conversation::addUser(std::string content) {
    turns_.push_back({Role::User, std::move(content)});
}

void Conversation::addAssistant(std::string content) {
    turns_.push_back({Role::Assistant, std::move(content)});
}

void Conversation::append(Role role, std::string content) {
    if (role == Role::System) {
        setSystem(std::move(content));
        return;
    }
    turns_.push_back({role, std::move(content)});
}

void Conversation::reset(bool clearSystem) {
    turns_.clear();
    if (clearSystem) system_.clear();
}

std::vector<Turn> Conversation::messages() const {
    std::vector<Turn> out;
    out.reserve(turns_.size() + 1);
    if (hasSystem()) out.push_back({Role::System, system_});
    for (const auto& t : turns_) out.push_back(t);
    return out;
}

std::size_t Conversation::estimateTokens(std::string_view text) {
    return (text.size() + kCharsPerToken - 1) / kCharsPerToken;  // ceil division
}

std::size_t Conversation::estimatedTokens() const {
    std::size_t total = 0;
    if (hasSystem()) total += estimateTokens(system_) + kPerMessageOverhead;
    for (const auto& t : turns_)
        total += estimateTokens(t.content) + kPerMessageOverhead;
    return total;
}

std::size_t Conversation::capToTokens(std::size_t budget) {
    std::size_t dropped = 0;
    // Trim oldest dialogue turns, but never the most recent (the live query).
    while (turns_.size() > 1 && estimatedTokens() > budget) {
        turns_.erase(turns_.begin());
        ++dropped;
    }
    // If trimming left the dialogue opening on an assistant turn, drop that too
    // so it still opens on a user turn (Mistral folds the system prompt into the
    // first user turn, §4 — a leading assistant turn would be malformed). Only a
    // cleanup of our own trimming: a within-budget conversation is left untouched.
    if (dropped > 0) {
        while (turns_.size() > 1 && turns_.front().role == Role::Assistant) {
            turns_.erase(turns_.begin());
            ++dropped;
        }
    }
    return dropped;
}

}  // namespace villen::chat
