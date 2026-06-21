#include "envelope.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace villen::envelope {

std::string joined(const std::string& session, const std::string& seat) {
    json msg = {{"type", "joined"}, {"session", session}, {"seat", seat}};
    return msg.dump();
}

std::string sessionUpdate(const std::vector<std::pair<std::string, std::string>>& seats) {
    json s = json::object();
    for (const auto& [name, status] : seats) s[name] = status;
    json msg = {{"type", "sessionUpdate"}, {"seats", std::move(s)}};
    return msg.dump();
}

std::string engineAnnounce(std::string_view name) {
    json msg = {{"type", "engine"},
                {"name", name.empty() ? json(nullptr) : json(std::string(name))}};
    return msg.dump();
}

std::string reject(std::string_view reason) {
    json msg = {{"type", "reject"}, {"reason", std::string(reason)}};
    return msg.dump();
}

Incoming parse(std::string_view text) {
    Incoming in;
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("type")) return in;
    // Read each field type-checked: a key present with the wrong JSON type (from a
    // malformed/hostile client) must yield the default, never throw a type_error
    // that would crash the server (DoS). session/seat keep their struct defaults.
    auto str = [&](const char* key, std::string& out) {
        auto it = j.find(key);
        if (it != j.end() && it->is_string()) out = it->get<std::string>();
    };
    str("type", in.type);
    str("session", in.session);
    str("seat", in.seat);
    in.ok = true;
    return in;
}

}  // namespace villen::envelope
