// beliefdb — public vocabulary: node/edge kinds, credence, scope, errors.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace beliefdb {

enum class NodeType { source, claim, hypothesis, question, rule };
enum class EdgeType { supports, contradicts, derives_from, assumes, answers, refines, replaces };
enum class Status { active, stale, retracted, superseded };
enum class Sensitivity { public_, domain, private_ };
enum class CredenceMethod { stated, measured, llm_judgment, rule, min, noisy_or, manual };
/// Nature of a piece of evidence. Coarse and structural on purpose: who said it
/// goes into `attribution`, how much to trust it is a track record (Ф6), not a kind.
enum class SourceKind { measurement, document, human_statement, llm_output, derived };

std::string_view to_string(NodeType);
std::string_view to_string(EdgeType);
std::string_view to_string(Status);
std::string_view to_string(Sensitivity);
std::string_view to_string(CredenceMethod);
std::string_view to_string(SourceKind);

std::optional<NodeType> node_type_from(std::string_view);
std::optional<EdgeType> edge_type_from(std::string_view);
std::optional<Status> status_from(std::string_view);
std::optional<Sensitivity> sensitivity_from(std::string_view);
std::optional<CredenceMethod> credence_method_from(std::string_view);
std::optional<SourceKind> source_kind_from(std::string_view);

/// Credence is stored in coarse steps only. A value off the ladder is rejected,
/// not snapped: silent rounding would hide that the caller invented precision.
inline constexpr double kCredenceSteps[] = {0.05, 0.2, 0.35, 0.5, 0.65, 0.8, 0.95};
bool is_credence_step(double value);
/// Error text for a value off the ladder, naming the two nearest steps.
std::string credence_step_error(double value);

/// Area of validity of a claim. A claim without scope is rejected: it is
/// almost always false somewhere else. Timestamps are ISO-8601 (UTC).
struct Scope {
    std::optional<std::string> valid_from;  // event time: from when the claim holds
    std::optional<std::string> valid_to;    // event time: until when (absent = open)
    nlohmann::json conditions = nlohmann::json::object();  // {"patient":"me", "dose":"400mg"}
    std::string note;                       // free text when structure does not fit

    bool empty() const;
    nlohmann::json to_json() const;
    static Scope from_json(const nlohmann::json&);
};

struct Credence {
    double value = 0;
    CredenceMethod method = CredenceMethod::manual;
    nlohmann::json inputs = nlohmann::json::object();  // what the number was computed from
    std::string computed_at;
    bool stale = false;
};

struct Node {
    std::string id;
    NodeType type = NodeType::claim;
    std::string domain;
    std::string created_at;
    std::string updated_at;
    Status status = Status::active;
    Sensitivity sensitivity = Sensitivity::domain;
    nlohmann::json payload;
    std::optional<Credence> credence;  // claims and hypotheses only
};

struct Edge {
    std::string src;
    std::string dst;
    EdgeType type = EdgeType::supports;
    std::optional<std::string> rule_id;
    double weight = 1.0;
    std::string created_at;
};

// ---- write inputs ----------------------------------------------------------

struct SourceInput {
    SourceKind kind = SourceKind::document;
    std::string uri;          // where it came from (URL, file, "telegram:chat/msg", ...)
    std::string content;      // the evidence itself; hashed for deduplication
    std::string observed_at;  // when the evidence was produced (ISO-8601)
    std::string attribution;  // who said it: "врач Иванов", "reddit:user_x", "blog.panic.com"
    std::optional<double> trust_hint;  // manual bootstrap only; measured reliability comes from Ф6
    std::string domain = "general";
    Sensitivity sensitivity = Sensitivity::domain;
};

struct RuleInput {
    std::string name;         // unique, e.g. "dose_from_prescription"
    std::string description;  // how the rule derives a claim from its inputs
    std::string domain = "general";
};

struct ClaimInput {
    std::string text;
    std::string subject;
    std::string predicate;
    std::string value;
    std::vector<std::string> support;  // source or claim ids; must be non-empty
    double credence = 0.5;
    CredenceMethod method = CredenceMethod::stated;  // stated | measured | llm_judgment | manual
    Scope scope;
    std::string domain = "general";
    Sensitivity sensitivity = Sensitivity::domain;
};

struct DeriveInput {
    std::string text;
    std::string subject;
    std::string predicate;
    std::string value;
    std::vector<std::string> from;  // claim ids; must be non-empty
    std::string rule;               // rule id or rule name
    std::optional<double> credence; // absent => computed as MIN over `from`
    CredenceMethod method = CredenceMethod::min;  // min | rule | noisy_or | llm_judgment | manual
    Scope scope;
    std::string domain = "general";
    Sensitivity sensitivity = Sensitivity::domain;
};

// ---- read results ----------------------------------------------------------

/// One level of a justification tree returned by why(). `via` and `rule`
/// describe the edge from the parent to this node (unset on the root).
struct WhyNode {
    std::string id;
    NodeType type = NodeType::claim;
    std::string label;  // claim text, "kind: uri" for sources, rule name
    Status status = Status::active;
    std::optional<Credence> credence;
    std::optional<EdgeType> via;      // supports | derives_from | assumes
    std::optional<std::string> rule;  // rule name for derives_from
    std::vector<WhyNode> children;
    bool truncated = false;  // depth limit reached below this node

    nlohmann::json to_json() const;
};

// ---- errors ----------------------------------------------------------------

enum class ErrorCode {
    not_found,
    no_support,       // assert/derive without any support
    invalid_support,  // support id has the wrong type or is not active
    cycle,            // the edge would make the justification graph cyclic
    bad_credence,     // not on the ladder, or method not allowed for the call
    empty_scope,
    bad_input,        // empty text/domain, unknown enum value, ...
    duplicate,        // rule name already defined
    immutable,        // attempt to modify a source
};
std::string_view to_string(ErrorCode);

struct Error : std::runtime_error {
    ErrorCode code;
    Error(ErrorCode c, const std::string& what) : std::runtime_error(what), code(c) {}
};

}  // namespace beliefdb
