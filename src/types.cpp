#include "beliefdb/types.hpp"

#include <cmath>
#include <iterator>
#include <sstream>

namespace beliefdb {

namespace {
template <class E, size_t N>
std::optional<E> lookup(std::string_view s, const std::pair<E, std::string_view> (&table)[N]) {
    for (const auto& [e, name] : table)
        if (name == s) return e;
    return std::nullopt;
}
template <class E, size_t N>
std::string_view name_of(E e, const std::pair<E, std::string_view> (&table)[N]) {
    for (const auto& [v, name] : table)
        if (v == e) return name;
    return "?";
}

constexpr std::pair<NodeType, std::string_view> kNodeTypes[] = {
    {NodeType::source, "source"}, {NodeType::claim, "claim"}, {NodeType::hypothesis, "hypothesis"},
    {NodeType::question, "question"}, {NodeType::rule, "rule"}};
constexpr std::pair<EdgeType, std::string_view> kEdgeTypes[] = {
    {EdgeType::supports, "supports"},   {EdgeType::contradicts, "contradicts"}, {EdgeType::derives_from, "derives_from"},
    {EdgeType::assumes, "assumes"},     {EdgeType::answers, "answers"},         {EdgeType::refines, "refines"},
    {EdgeType::replaces, "replaces"}};
constexpr std::pair<Status, std::string_view> kStatuses[] = {
    {Status::active, "active"}, {Status::stale, "stale"}, {Status::retracted, "retracted"}, {Status::superseded, "superseded"}, {Status::closed, "closed"}};
constexpr std::pair<Sensitivity, std::string_view> kSensitivities[] = {
    {Sensitivity::public_, "public"}, {Sensitivity::domain, "domain"}, {Sensitivity::private_, "private"}};
constexpr std::pair<CredenceMethod, std::string_view> kMethods[] = {
    {CredenceMethod::stated, "stated"}, {CredenceMethod::measured, "measured"}, {CredenceMethod::llm_judgment, "llm_judgment"},
    {CredenceMethod::rule, "rule"},     {CredenceMethod::min, "min"},           {CredenceMethod::noisy_or, "noisy_or"},
    {CredenceMethod::manual, "manual"}};
constexpr std::pair<SourceKind, std::string_view> kSourceKinds[] = {
    {SourceKind::measurement, "measurement"}, {SourceKind::document, "document"}, {SourceKind::human_statement, "human_statement"},
    {SourceKind::llm_output, "llm_output"}};
constexpr std::pair<Directness, std::string_view> kDirectness[] = {{Directness::primary, "primary"}, {Directness::secondary, "secondary"}};
constexpr std::pair<ErrorCode, std::string_view> kErrors[] = {
    {ErrorCode::not_found, "not_found"},     {ErrorCode::no_support, "no_support"},   {ErrorCode::invalid_support, "invalid_support"},
    {ErrorCode::cycle, "cycle"},             {ErrorCode::bad_credence, "bad_credence"}, {ErrorCode::empty_scope, "empty_scope"},
    {ErrorCode::bad_input, "bad_input"},     {ErrorCode::duplicate, "duplicate"},     {ErrorCode::immutable, "immutable"},     {ErrorCode::scope_widens, "scope_widens"},
    {ErrorCode::empty_intersection, "empty_intersection"}, {ErrorCode::type_mismatch, "type_mismatch"},
    {ErrorCode::bridge_source, "bridge_source"}, {ErrorCode::already_replaced, "already_replaced"}, {ErrorCode::stale_premise, "stale_premise"}};
constexpr std::pair<ValueType, std::string_view> kValueTypes[] = {
    {ValueType::boolean, "bool"}, {ValueType::number, "number"}, {ValueType::enumeration, "enum"},
    {ValueType::ref, "ref"},      {ValueType::text, "text"}};
}  // namespace

std::string_view to_string(NodeType v) { return name_of(v, kNodeTypes); }
std::string_view to_string(EdgeType v) { return name_of(v, kEdgeTypes); }
std::string_view to_string(Status v) { return name_of(v, kStatuses); }
std::string_view to_string(Sensitivity v) { return name_of(v, kSensitivities); }
std::string_view to_string(CredenceMethod v) { return name_of(v, kMethods); }
std::string_view to_string(ErrorCode v) { return name_of(v, kErrors); }
std::string_view to_string(SourceKind v) { return name_of(v, kSourceKinds); }
std::string_view to_string(Directness v) { return name_of(v, kDirectness); }
std::string_view to_string(ValueType v) { return name_of(v, kValueTypes); }

std::optional<NodeType> node_type_from(std::string_view s) { return lookup(s, kNodeTypes); }
std::optional<EdgeType> edge_type_from(std::string_view s) { return lookup(s, kEdgeTypes); }
std::optional<Status> status_from(std::string_view s) { return lookup(s, kStatuses); }
std::optional<Sensitivity> sensitivity_from(std::string_view s) { return lookup(s, kSensitivities); }
std::optional<CredenceMethod> credence_method_from(std::string_view s) { return lookup(s, kMethods); }
std::optional<SourceKind> source_kind_from(std::string_view s) { return lookup(s, kSourceKinds); }
std::optional<Directness> directness_from(std::string_view s) { return lookup(s, kDirectness); }
std::optional<ValueType> value_type_from(std::string_view s) { return lookup(s, kValueTypes); }

bool is_credence_step(double value) {
    for (double s : kCredenceSteps)
        if (std::fabs(s - value) < 1e-9) return true;
    return false;
}

std::string credence_step_error(double value) {
    // the ladder is sorted; find the two steps around `value`
    double lo = kCredenceSteps[0], hi = kCredenceSteps[std::size(kCredenceSteps) - 1];
    for (double st : kCredenceSteps) {
        if (st <= value) lo = st;
        if (st >= value) { hi = st; break; }
    }
    std::string ladder;
    for (double st : kCredenceSteps) { ladder += ladder.empty() ? "" : " "; ladder += nlohmann::json(st).dump(); }
    const std::string v = nlohmann::json(value).dump();
    if (value < kCredenceSteps[0] || value > hi)
        return "credence " + v + " is outside the ladder " + ladder;
    return "credence " + v + " is not on the ladder " + ladder + "; nearest steps are " + nlohmann::json(lo).dump() + " and " +
           nlohmann::json(hi).dump() + " — pick one deliberately";
}

double NutritionWeights::of(SourceKind k, Directness d) const {
    // llm_output is a retelling by construction, so directness does not apply to it
    const double f = (d == Directness::secondary && k != SourceKind::llm_output) ? secondary_factor : 1.0;
    switch (k) {
        case SourceKind::measurement: return measurement * f;
        case SourceKind::document: return document * f;
        case SourceKind::human_statement: return human_statement * f;
        case SourceKind::llm_output: return llm_output;
    }
    return 0;
}

nlohmann::json Nutrition::to_json() const {
    nlohmann::json kinds = nlohmann::json::object();
    for (const auto& [k, n] : by_kind) kinds[std::string(to_string(k))] = n;
    return {{"id", id},
            {"n_sources", n_sources},
            {"n_attributions", n_attributions},
            {"n_unattributed", n_unattributed},
            {"n_secondary", n_secondary},
            {"weighted", weighted},
            {"concentration", concentration},
            {"by_kind", kinds},
            {"oldest_observed_at", oldest_observed_at},
            {"newest_observed_at", newest_observed_at},
            {"depth", depth},
            {"dilution", dilution},
            {"sources", sources},
            {"attributions", attributions},
            {"computed_at", computed_at}};
}

nlohmann::json WhyNode::to_json() const {
    nlohmann::json j{{"id", id}, {"type", to_string(type)}, {"label", label}, {"status", to_string(status)}};
    if (credence) {
        j["credence"] = {{"value", credence->value},
                         {"method", to_string(credence->method)},
                         {"stale", credence->stale},
                         {"computed_at", credence->computed_at}};
    }
    if (via) j["via"] = to_string(*via);
    if (rule) j["rule"] = *rule;
    if (truncated) j["truncated"] = true;
    if (notice) j["notice"] = *notice;
    auto arr = nlohmann::json::array();
    for (const auto& c : children) arr.push_back(c.to_json());
    j["children"] = std::move(arr);
    return j;
}

nlohmann::json StaleEntry::to_json() const {
    nlohmann::json j{{"id", id}, {"label", label}, {"domain", domain}, {"depth", depth}};
    if (credence) j["credence"] = {{"value", credence->value}, {"method", to_string(credence->method)}, {"computed_at", credence->computed_at}};
    auto arr = nlohmann::json::array();
    for (const auto& c : causes) arr.push_back({{"cause", c.cause}, {"reason", c.reason}, {"at", c.at}});
    j["causes"] = std::move(arr);
    return j;
}

nlohmann::json WhatIfEntry::to_json() const {
    nlohmann::json j{{"id", id}, {"label", label}, {"method", to_string(method)}, {"current", current}};
    j["projected"] = projected ? nlohmann::json(*projected) : nlohmann::json();
    return j;
}

std::string Value::describe() const {
    std::ostringstream os;
    switch (type) {
        case ValueType::boolean: return b ? "true" : "false";
        case ValueType::number:
            os << lo;
            if (hi != lo) os << ".." << hi;
            if (!unit.empty()) os << ' ' << unit;
            return os.str();
        case ValueType::enumeration: case ValueType::ref: return s;
        case ValueType::text: return s.empty() ? "" : "\"" + s + "\"";
    }
    return "";
}

static nlohmann::json side_json(const ConflictSide& s) {
    nlohmann::json j{{"id", s.id}, {"text", s.text}, {"domain", s.domain}, {"status", to_string(s.status)},
                     {"value", s.value.to_json()}, {"scope", s.scope.to_json()}, {"n_dependents", s.n_dependents}, {"n_sources", s.n_sources}};
    if (s.credence) j["credence"] = {{"value", s.credence->value}, {"method", to_string(s.credence->method)}, {"stale", s.credence->stale}};
    return j;
}

nlohmann::json ConflictEntry::to_json() const {
    nlohmann::json j{{"a", side_json(a)}, {"b", side_json(b)}, {"subject", subject}, {"predicate", predicate},
                     {"explicit", explicit_edge}, {"detected", detected}, {"n_dependents", n_dependents}, {"questions", questions}};
    if (overlap) j["overlap"] = overlap->to_json();
    return j;
}

nlohmann::json RefinementEntry::to_json() const {
    return {{"narrower", side_json(narrower)}, {"wider", side_json(wider)}, {"subject", subject}, {"predicate", predicate}};
}

nlohmann::json QuestionEntry::to_json() const {
    auto prog = nlohmann::json::array();
    for (const auto& p : progress) prog.push_back({{"at", p.at}, {"actor", p.actor}, {"text", p.text}});
    return {{"id", id}, {"text", text}, {"why_open", why_open}, {"acceptance_criterion", acceptance_criterion},
            {"domain", domain}, {"status", to_string(status)}, {"created_at", created_at}, {"about", about},
            {"answers", answers}, {"progress", prog}, {"resolution", resolution}};
}

}  // namespace beliefdb
