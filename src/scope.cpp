#include "beliefdb/types.hpp"

namespace beliefdb {

bool Scope::empty() const {
    return !valid_from && !valid_to && (conditions.is_null() || conditions.empty()) && note.empty();
}

nlohmann::json Scope::to_json() const {
    nlohmann::json j = nlohmann::json::object();
    if (valid_from) j["valid_from"] = *valid_from;
    if (valid_to) j["valid_to"] = *valid_to;
    if (!conditions.is_null() && !conditions.empty()) j["conditions"] = conditions;
    if (!note.empty()) j["note"] = note;
    return j;
}

Scope Scope::from_json(const nlohmann::json& j) {
    Scope s;
    if (!j.is_object()) return s;
    if (j.contains("valid_from") && j["valid_from"].is_string()) s.valid_from = j["valid_from"].get<std::string>();
    if (j.contains("valid_to") && j["valid_to"].is_string()) s.valid_to = j["valid_to"].get<std::string>();
    if (j.contains("conditions") && j["conditions"].is_object()) s.conditions = j["conditions"];
    if (j.contains("note") && j["note"].is_string()) s.note = j["note"].get<std::string>();
    return s;
}

namespace {
// unset start = -inf, unset end = +inf
bool starts_before_or_at(const std::optional<std::string>& a, const std::optional<std::string>& b) {
    if (!a) return true;
    if (!b) return false;
    return *a <= *b;
}
bool ends_after_or_at(const std::optional<std::string>& a, const std::optional<std::string>& b) {
    if (!a) return true;
    if (!b) return false;
    return *a >= *b;
}
const nlohmann::json& conds(const Scope& s) {
    static const nlohmann::json empty = nlohmann::json::object();
    return s.conditions.is_object() ? s.conditions : empty;
}
}  // namespace

std::optional<Scope> Scope::intersect(const Scope& a, const Scope& b) {
    Scope r;
    // later start
    if (a.valid_from && b.valid_from) r.valid_from = std::max(*a.valid_from, *b.valid_from);
    else r.valid_from = a.valid_from ? a.valid_from : b.valid_from;
    // earlier end
    if (a.valid_to && b.valid_to) r.valid_to = std::min(*a.valid_to, *b.valid_to);
    else r.valid_to = a.valid_to ? a.valid_to : b.valid_to;
    if (r.valid_from && r.valid_to && *r.valid_from > *r.valid_to) return std::nullopt;
    // union of conditions; a shared key with different values empties the intersection
    r.conditions = conds(a);
    for (const auto& [k, v] : conds(b).items()) {
        if (r.conditions.contains(k)) {
            if (r.conditions[k] != v) return std::nullopt;
        } else {
            r.conditions[k] = v;
        }
    }
    return r;
}

bool Scope::contains(const Scope& outer, const Scope& inner) {
    if (!starts_before_or_at(outer.valid_from, inner.valid_from)) return false;
    if (!ends_after_or_at(outer.valid_to, inner.valid_to)) return false;
    for (const auto& [k, v] : conds(outer).items()) {
        const auto& ic = conds(inner);
        if (!ic.contains(k) || ic[k] != v) return false;
    }
    return true;
}

bool Scope::same(const Scope& a, const Scope& b) {
    return a.valid_from == b.valid_from && a.valid_to == b.valid_to && conds(a) == conds(b);
}

bool Scope::strictly_contains(const Scope& outer, const Scope& inner) {
    return contains(outer, inner) && !same(outer, inner);
}

}  // namespace beliefdb
