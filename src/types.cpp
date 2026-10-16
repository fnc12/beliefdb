#include "beliefdb/types.hpp"

#include <cmath>
#include <iterator>

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
    {Status::active, "active"}, {Status::stale, "stale"}, {Status::retracted, "retracted"}, {Status::superseded, "superseded"}};
constexpr std::pair<Sensitivity, std::string_view> kSensitivities[] = {
    {Sensitivity::public_, "public"}, {Sensitivity::domain, "domain"}, {Sensitivity::private_, "private"}};
constexpr std::pair<CredenceMethod, std::string_view> kMethods[] = {
    {CredenceMethod::stated, "stated"}, {CredenceMethod::measured, "measured"}, {CredenceMethod::llm_judgment, "llm_judgment"},
    {CredenceMethod::rule, "rule"},     {CredenceMethod::min, "min"},           {CredenceMethod::noisy_or, "noisy_or"},
    {CredenceMethod::manual, "manual"}};
constexpr std::pair<SourceKind, std::string_view> kSourceKinds[] = {
    {SourceKind::measurement, "measurement"}, {SourceKind::document, "document"}, {SourceKind::human_statement, "human_statement"},
    {SourceKind::llm_output, "llm_output"},   {SourceKind::derived, "derived"}};
constexpr std::pair<ErrorCode, std::string_view> kErrors[] = {
    {ErrorCode::not_found, "not_found"},     {ErrorCode::no_support, "no_support"},   {ErrorCode::invalid_support, "invalid_support"},
    {ErrorCode::cycle, "cycle"},             {ErrorCode::bad_credence, "bad_credence"}, {ErrorCode::empty_scope, "empty_scope"},
    {ErrorCode::bad_input, "bad_input"},     {ErrorCode::duplicate, "duplicate"},     {ErrorCode::immutable, "immutable"}};
}  // namespace

std::string_view to_string(NodeType v) { return name_of(v, kNodeTypes); }
std::string_view to_string(EdgeType v) { return name_of(v, kEdgeTypes); }
std::string_view to_string(Status v) { return name_of(v, kStatuses); }
std::string_view to_string(Sensitivity v) { return name_of(v, kSensitivities); }
std::string_view to_string(CredenceMethod v) { return name_of(v, kMethods); }
std::string_view to_string(ErrorCode v) { return name_of(v, kErrors); }
std::string_view to_string(SourceKind v) { return name_of(v, kSourceKinds); }

std::optional<NodeType> node_type_from(std::string_view s) { return lookup(s, kNodeTypes); }
std::optional<EdgeType> edge_type_from(std::string_view s) { return lookup(s, kEdgeTypes); }
std::optional<Status> status_from(std::string_view s) { return lookup(s, kStatuses); }
std::optional<Sensitivity> sensitivity_from(std::string_view s) { return lookup(s, kSensitivities); }
std::optional<CredenceMethod> credence_method_from(std::string_view s) { return lookup(s, kMethods); }
std::optional<SourceKind> source_kind_from(std::string_view s) { return lookup(s, kSourceKinds); }

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
    auto arr = nlohmann::json::array();
    for (const auto& c : children) arr.push_back(c.to_json());
    j["children"] = std::move(arr);
    return j;
}

}  // namespace beliefdb
