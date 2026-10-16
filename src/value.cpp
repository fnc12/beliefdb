#include "beliefdb/types.hpp"

#include <algorithm>
#include <cctype>

namespace beliefdb {

namespace {
std::string lower_trim(std::string_view in) {
    size_t b = 0, e = in.size();
    while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
    std::string out(in.substr(b, e - b));
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

struct UnitAlias { const char* alias; const char* base; double factor; };
// Deliberately small. Money codes are units of their own: there is no exchange
// rate here, "USD" and "KZT" simply do not compare.
constexpr UnitAlias kUnits[] = {
    // mass -> kg
    {"kg", "kg", 1}, {"кг", "kg", 1}, {"g", "kg", 1e-3}, {"г", "kg", 1e-3}, {"mg", "kg", 1e-6}, {"мг", "kg", 1e-6},
    {"t", "kg", 1e3}, {"т", "kg", 1e3},
    // length -> m
    {"m", "m", 1}, {"м", "m", 1}, {"cm", "m", 1e-2}, {"см", "m", 1e-2}, {"mm", "m", 1e-3}, {"мм", "m", 1e-3},
    {"km", "m", 1e3}, {"км", "m", 1e3},
    // time -> s
    {"s", "s", 1}, {"sec", "s", 1}, {"с", "s", 1}, {"ms", "s", 1e-3}, {"мс", "s", 1e-3}, {"min", "s", 60}, {"мин", "s", 60},
    {"h", "s", 3600}, {"ч", "s", 3600}, {"d", "s", 86400}, {"day", "s", 86400}, {"дн", "s", 86400}, {"д", "s", 86400},
    // percent
    {"%", "%", 1}, {"pct", "%", 1}, {"percent", "%", 1},
    // money
    {"$", "USD", 1}, {"usd", "USD", 1}, {"долл", "USD", 1}, {"€", "EUR", 1}, {"eur", "EUR", 1},
    {"₸", "KZT", 1}, {"kzt", "KZT", 1}, {"тг", "KZT", 1}, {"тенге", "KZT", 1},
    {"₽", "RUB", 1}, {"rub", "RUB", 1}, {"руб", "RUB", 1}, {"£", "GBP", 1}, {"gbp", "GBP", 1},
};
}  // namespace

UnitNorm normalize_unit(std::string_view unit) {
    const std::string key = lower_trim(unit);
    if (key.empty()) return {"", 1.0};
    for (const auto& u : kUnits)
        if (key == u.alias) return {u.base, u.factor};
    return {key, 1.0};
}

Value Value::boolean(bool v) { Value x; x.type = ValueType::boolean; x.b = v; return x; }
Value Value::enum_member(std::string m) { Value x; x.type = ValueType::enumeration; x.s = std::move(m); return x; }
Value Value::reference(std::string id) { Value x; x.type = ValueType::ref; x.s = std::move(id); return x; }
Value Value::text(std::string t) { Value x; x.type = ValueType::text; x.s = std::move(t); return x; }

Value Value::number(double lo, double hi, std::string unit) {
    if (lo > hi) std::swap(lo, hi);
    Value x;
    x.type = ValueType::number;
    x.original_lo = lo;
    x.original_hi = hi;
    x.original_unit = unit;
    const auto n = normalize_unit(unit);
    x.unit = n.unit;
    x.lo = lo * n.factor;
    x.hi = hi * n.factor;
    return x;
}

nlohmann::json Value::to_json() const {
    nlohmann::json j{{"type", to_string(type)}};
    switch (type) {
        case ValueType::boolean: j["value"] = b; break;
        case ValueType::number:
            j["lo"] = lo; j["hi"] = hi; j["unit"] = unit;
            j["original"] = {{"lo", original_lo}, {"hi", original_hi}, {"unit", original_unit}};
            break;
        case ValueType::enumeration: case ValueType::ref: case ValueType::text: j["value"] = s; break;
    }
    return j;
}

Value Value::from_json(const nlohmann::json& j) {
    Value v;
    if (!j.is_object()) return text(j.is_string() ? j.get<std::string>() : std::string());
    v.type = value_type_from(j.value("type", "text")).value_or(ValueType::text);
    switch (v.type) {
        case ValueType::boolean: v.b = j.value("value", false); break;
        case ValueType::number:
            v.lo = j.value("lo", 0.0); v.hi = j.value("hi", 0.0); v.unit = j.value("unit", "");
            if (j.contains("original") && j["original"].is_object()) {
                const auto& o = j["original"];
                v.original_lo = o.value("lo", v.lo); v.original_hi = o.value("hi", v.hi); v.original_unit = o.value("unit", v.unit);
            } else { v.original_lo = v.lo; v.original_hi = v.hi; v.original_unit = v.unit; }
            break;
        default: v.s = j.value("value", ""); break;
    }
    return v;
}

std::optional<bool> Value::compatible(const Value& a, const Value& b) {
    if (a.type != b.type) return std::nullopt;
    switch (a.type) {
        case ValueType::text: return std::nullopt;
        case ValueType::boolean: return a.b == b.b;
        case ValueType::enumeration: case ValueType::ref: return a.s == b.s;
        case ValueType::number:
            if (a.unit != b.unit) return std::nullopt;
            return a.lo <= b.hi && b.lo <= a.hi;  // intervals overlap
    }
    return std::nullopt;
}

}  // namespace beliefdb
