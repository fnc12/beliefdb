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
/// almost always false somewhere else.
///
/// Two time axes are kept apart on purpose: valid_from/valid_to say when the
/// claim holds in the world (event time); created_at / status changes on the
/// node say when we learned it (transaction time). valid_to unset means "still
/// in force", not "unknown". Timestamps are ISO-8601 strings compared
/// lexicographically, so keep one precision per axis within a domain.
///
/// conditions is a flat object; a missing key is a wildcard. Hence
/// {patient: me} is narrower than {} and both intersect.
struct Scope {
    std::optional<std::string> valid_from;
    std::optional<std::string> valid_to;
    nlohmann::json conditions = nlohmann::json::object();
    std::string note;  // free text; never used in comparisons

    bool empty() const;
    nlohmann::json to_json() const;
    static Scope from_json(const nlohmann::json&);

    /// Intersection of validity: later start, earlier end, union of conditions.
    /// nullopt when the interval is empty or a shared key has different values.
    static std::optional<Scope> intersect(const Scope& a, const Scope& b);
    /// inner ⊆ outer: outer's interval covers inner's and every condition of
    /// outer is present in inner with the same value.
    static bool contains(const Scope& outer, const Scope& inner);
    /// outer strictly wider than inner (contains, and not equal on the compared fields).
    static bool strictly_contains(const Scope& outer, const Scope& inner);
    /// equal on interval and conditions (note ignored)
    static bool same(const Scope& a, const Scope& b);
};

/// Typed value of a claim. Comparability is a property of the type: text is
/// explicitly incomparable and the automatic contradiction detector never
/// looks at it. number is always an interval [lo, hi] (a point is lo == hi)
/// in a normalized unit, with the original spelling kept.
enum class ValueType { boolean, number, enumeration, ref, text };
std::string_view to_string(ValueType);
std::optional<ValueType> value_type_from(std::string_view);

struct Value {
    ValueType type = ValueType::text;
    bool b = false;                 // boolean
    double lo = 0, hi = 0;          // number, in `unit`
    std::string unit;               // normalized unit, e.g. "kg", "USD", "%"
    std::string original_unit;      // as written, e.g. "мг" or "$"
    double original_lo = 0, original_hi = 0;
    std::string s;                  // enum member | ref node id | text

    static Value boolean(bool v);
    static Value number(double lo, double hi, std::string unit = "");
    static Value point(double v, std::string unit = "") { return number(v, v, std::move(unit)); }
    static Value enum_member(std::string member);
    static Value reference(std::string node_id);
    static Value text(std::string t);

    nlohmann::json to_json() const;
    static Value from_json(const nlohmann::json&);

    /// true: the two values can both hold; false: they exclude each other;
    /// nullopt: not comparable (text, different types, different units).
    static std::optional<bool> compatible(const Value& a, const Value& b);
};

/// Unit normalization: a small alias table (mass, length, time, money, percent),
/// not a unit system. Returns {base_unit, factor} so that value_in_base = value * factor.
/// Unknown units are returned trimmed, factor 1, so they still compare by string.
struct UnitNorm { std::string unit; double factor = 1.0; };
UnitNorm normalize_unit(std::string_view unit);

/// Predicate registry entry. Created explicitly or on first use; later uses
/// must match the declared value type and unit family.
struct Predicate {
    std::string name;
    ValueType value_type = ValueType::text;
    std::string unit;                       // normalized base unit for number
    std::vector<std::string> enum_values;   // for enumeration
    std::string note;
    std::string created_at;
};

struct PredicateInput {
    std::string name;
    ValueType value_type = ValueType::text;
    std::string unit;
    std::vector<std::string> enum_values;
    std::string note;
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
    std::string predicate;   // registry key; empty = untyped claim
    Value value;             // defaults to text("")
    std::vector<std::string> support;  // source or claim ids; must be non-empty
    double credence = 0.5;
    CredenceMethod method = CredenceMethod::stated;  // stated | measured | llm_judgment | manual
    Scope scope;             // valid_from defaults to the earliest observed_at among source supports
    std::string domain = "general";
    Sensitivity sensitivity = Sensitivity::domain;
};

struct DeriveInput {
    std::string text;
    std::string subject;
    std::string predicate;
    Value value;
    std::vector<std::string> from;  // claim ids; must be non-empty
    std::string rule;               // rule id or rule name
    std::optional<double> credence; // absent => computed as MIN over `from`
    CredenceMethod method = CredenceMethod::min;  // min | rule | noisy_or | llm_judgment | manual
    Scope scope;  // may only narrow the intersection of the premises' scopes; unset axes inherit it
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
    scope_widens,     // derived scope wider than the intersection of its premises
    empty_intersection,  // premises are never valid together
    type_mismatch,    // value type/unit disagrees with the predicate registry
};
std::string_view to_string(ErrorCode);

struct Error : std::runtime_error {
    ErrorCode code;
    Error(ErrorCode c, const std::string& what) : std::runtime_error(what), code(c) {}
};

}  // namespace beliefdb
