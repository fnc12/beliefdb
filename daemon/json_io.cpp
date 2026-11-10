#include "json_io.hpp"

namespace beliefd {

using namespace beliefdb;
using nlohmann::json;

Value parse_value(const json& j) {
    if (j.is_null()) return Value::text("");
    if (j.is_string()) return Value::text(j.get<std::string>());
    if (j.is_boolean()) return Value::boolean(j.get<bool>());
    if (j.is_number()) return Value::point(j.get<double>());
    if (!j.is_object()) throw HttpError{400, "bad_input", "value must be an object, string, number or bool"};
    const auto type = value_type_from(j.value("type", "text"));
    if (!type) throw HttpError{400, "bad_input", "unknown value type: " + j.value("type", "")};
    switch (*type) {
        case ValueType::boolean: return Value::boolean(j.value("value", false));
        case ValueType::number: {
            const double lo = j.contains("lo") ? j["lo"].get<double>() : j.value("value", 0.0);
            const double hi = j.contains("hi") ? j["hi"].get<double>() : lo;
            return Value::number(lo, hi, j.value("unit", ""));
        }
        case ValueType::enumeration: return Value::enum_member(j.value("value", ""));
        case ValueType::ref: return Value::reference(j.value("value", ""));
        case ValueType::text: return Value::text(j.value("value", ""));
    }
    return Value::text("");
}

std::vector<std::string> ids_of(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key)) return out;
    if (j[key].is_string()) { out.push_back(j[key].get<std::string>()); return out; }
    if (j[key].is_array()) for (const auto& x : j[key]) if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

std::string need_str(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string() || j[key].get<std::string>().empty())
        throw HttpError{400, "bad_input", std::string(key) + " is required"};
    return j[key].get<std::string>();
}

SourceInput parse_source(const json& j) {
    SourceInput in;
    in.kind = enum_or(source_kind_from(j.value("kind", "document")), "kind", j.value("kind", ""));
    in.directness = enum_or(directness_from(j.value("directness", "primary")), "directness", j.value("directness", ""));
    in.uri = j.value("uri", "");
    in.content = need_str(j, "content");
    in.observed_at = j.value("observed_at", "");
    in.attribution = j.value("attribution", "");
    if (j.contains("trust_hint") && j["trust_hint"].is_number()) in.trust_hint = j["trust_hint"].get<double>();
    in.domain = j.value("domain", "general");
    in.sensitivity = enum_or(sensitivity_from(j.value("sensitivity", "domain")), "sensitivity", j.value("sensitivity", ""));
    return in;
}

ClaimInput parse_claim(const json& j) {
    ClaimInput in;
    in.text = need_str(j, "text");
    in.subject = j.value("subject", "");
    in.predicate = j.value("predicate", "");
    in.value = parse_value(j.value("value", json()));
    in.support = ids_of(j, "support");
    in.credence = j.value("credence", 0.5);
    in.method = enum_or(credence_method_from(j.value("method", "stated")), "method", j.value("method", ""));
    in.scope = Scope::from_json(j.value("scope", json::object()));
    in.domain = j.value("domain", "general");
    in.sensitivity = enum_or(sensitivity_from(j.value("sensitivity", "domain")), "sensitivity", j.value("sensitivity", ""));
    in.despite = ids_of(j, "despite");
    return in;
}

DeriveInput parse_derive(const json& j) {
    DeriveInput in;
    in.text = need_str(j, "text");
    in.subject = j.value("subject", "");
    in.predicate = j.value("predicate", "");
    in.value = parse_value(j.value("value", json()));
    in.from = ids_of(j, "from");
    in.rule = need_str(j, "rule");
    if (j.contains("credence") && j["credence"].is_number()) in.credence = j["credence"].get<double>();
    in.method = enum_or(credence_method_from(j.value("method", "min")), "method", j.value("method", ""));
    in.scope = Scope::from_json(j.value("scope", json::object()));
    in.domain = j.value("domain", "general");
    in.sensitivity = enum_or(sensitivity_from(j.value("sensitivity", "domain")), "sensitivity", j.value("sensitivity", ""));
    in.despite = ids_of(j, "despite");
    return in;
}

HypothesisInput parse_hypothesis(const json& j) {
    HypothesisInput in;
    in.text = need_str(j, "text");
    in.subject = j.value("subject", "");
    in.predicate = j.value("predicate", "");
    in.value = parse_value(j.value("value", json()));
    in.from = ids_of(j, "from");
    in.rule = need_str(j, "rule");
    in.scope = Scope::from_json(j.value("scope", json::object()));
    in.domain = j.value("domain", "general");
    in.sensitivity = enum_or(sensitivity_from(j.value("sensitivity", "domain")), "sensitivity", j.value("sensitivity", ""));
    in.despite = ids_of(j, "despite");
    return in;
}

json node_json(const Node& n) {
    json j{{"id", n.id},         {"type", to_string(n.type)},   {"domain", n.domain},
           {"status", to_string(n.status)}, {"sensitivity", to_string(n.sensitivity)},
           {"created_at", n.created_at}, {"updated_at", n.updated_at}, {"payload", n.payload}};
    if (n.credence)
        j["credence"] = {{"value", n.credence->value}, {"method", to_string(n.credence->method)},
                         {"inputs", n.credence->inputs}, {"computed_at", n.credence->computed_at}, {"stale", n.credence->stale}};
    return j;
}

}  // namespace beliefd
