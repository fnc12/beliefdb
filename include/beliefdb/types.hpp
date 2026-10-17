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
enum class Status { active, stale, retracted, superseded, closed };  // closed: questions only
enum class Sensitivity { public_, domain, private_ };
enum class CredenceMethod { stated, measured, llm_judgment, rule, min, noisy_or, manual };
/// Nature of a piece of evidence: who or what produced it. Coarse and structural
/// on purpose: who said it goes into `attribution`, how much to trust it is a
/// track record (Ф6). There is deliberately no "derived" kind: a conclusion is a
/// claim with derives_from edges, never a source, so reasoning can not be
/// wrapped into the shape of evidence (addendum 2).
enum class SourceKind { measurement, document, human_statement, llm_output };
/// How close the source is to the world. primary = a measurement, raw data or a
/// direct statement of fact; secondary = a retelling of somebody else's
/// conclusion whose own evidence we do not see. Orthogonal to kind.
enum class Directness { primary, secondary };

std::string_view to_string(NodeType);
std::string_view to_string(EdgeType);
std::string_view to_string(Status);
std::string_view to_string(Sensitivity);
std::string_view to_string(CredenceMethod);
std::string_view to_string(SourceKind);
std::string_view to_string(Directness);

std::optional<NodeType> node_type_from(std::string_view);
std::optional<EdgeType> edge_type_from(std::string_view);
std::optional<Status> status_from(std::string_view);
std::optional<Sensitivity> sensitivity_from(std::string_view);
std::optional<CredenceMethod> credence_method_from(std::string_view);
std::optional<SourceKind> source_kind_from(std::string_view);
std::optional<Directness> directness_from(std::string_view);

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
    /// Short human form: "400 mg", "0.5..1 kg", "true", enum member, quoted text.
    std::string describe() const;

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
    Directness directness = Directness::primary;  // secondary = somebody else's conclusion retold
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
    /// Set on the root when it is retracted or superseded: the first thing the
    /// reader must know, before any tree. Also set on children that were knocked out.
    std::optional<std::string> notice;

    nlohmann::json to_json() const;
};

/// Why a node's credence is stale: the root change that reached it.
struct StaleCause {
    std::string cause;   // retracted / superseded node, or a retracted rule
    std::string reason;  // as given to retract()/supersede()
    std::string at;
};

struct StaleEntry {
    std::string id;
    std::string label;
    std::string domain;
    std::optional<Credence> credence;  // the old value, still served with the flag
    int depth = 0;                     // recompute shallow nodes first
    std::vector<StaleCause> causes;

    nlohmann::json to_json() const;
};

/// One node that would change if a claim's credence were set to a new value.
struct WhatIfEntry {
    std::string id;
    std::string label;
    CredenceMethod method = CredenceMethod::min;
    double current = 0;
    std::optional<double> projected;  // set only where every step from the change is `min`; otherwise it would just go stale

    nlohmann::json to_json() const;
};

/// Weights by source kind used for the weighted nutrition number. They are
/// configuration, not findings: meaningful only after calibration (Ф6).
/// A secondary source gets the kind weight times `secondary_factor`;
/// llm_output ignores directness (it is a retelling by construction).
struct NutritionWeights {
    double measurement = 1.0;
    double document = 0.8;
    double human_statement = 0.5;
    double llm_output = 0.1;
    double secondary_factor = 0.5;
    double of(SourceKind k, Directness d = Directness::primary) const;
};

/// What a claim actually stands on: the SET of primary sources in its
/// transitive justification (a union, never a count, so three retellings of
/// one source stay one source) and the set of independent attributions.
struct Nutrition {
    std::string id;
    std::vector<std::string> sources;       // sorted source ids
    std::vector<std::string> attributions;  // sorted distinct attributions; an unattributed source counts as its own
    int n_sources = 0;
    int n_attributions = 0;
    int n_unattributed = 0;
    int n_secondary = 0;       // sources that retell somebody else's conclusion
    double weighted = 0;       // sum of kind weights over sources
    double concentration = 0;  // share of `weighted` contributed by the single heaviest source (0..1)
    std::vector<std::pair<SourceKind, int>> by_kind;
    std::string oldest_observed_at;
    std::string newest_observed_at;
    int depth = 0;          // 0 for a source, 1 for a claim read straight off sources, ...
    double dilution = 0;    // depth / n_sources: a deep conclusion on few sources is degenerate
    std::string computed_at;

    nlohmann::json to_json() const;
};

// ---- questions and conflicts (Ф4) ------------------------------------------

/// An open question is a first-class object, not the absence of an answer.
/// acceptance_criterion says what exactly would close it.
struct QuestionInput {
    std::string text;
    std::string why_open;              // why nobody knows yet
    std::string acceptance_criterion;  // what would close it
    std::vector<std::string> about;    // claims the question is about (e.g. the two sides of a conflict)
    std::string domain = "general";
    Sensitivity sensitivity = Sensitivity::domain;
};

struct ProgressNote {
    std::string at;
    std::string actor;
    std::string text;
};

struct QuestionEntry {
    std::string id;
    std::string text;
    std::string why_open;
    std::string acceptance_criterion;
    std::string domain;
    Status status = Status::active;    // active = open, closed = answered or moot
    std::string created_at;
    std::vector<std::string> about;
    std::vector<std::string> answers;  // claims linked with `answers`
    std::vector<ProgressNote> progress;
    std::string resolution;            // set when closed

    nlohmann::json to_json() const;
};

/// One side of a conflict, as much as is needed to decide what to do with it.
struct ConflictSide {
    std::string id;
    std::string text;
    std::string domain;
    Status status = Status::active;
    Value value;
    Scope scope;
    std::optional<Credence> credence;
    int n_dependents = 0;  // what stands on this claim
    int n_sources = 0;     // what this claim stands on
};

/// Two claims that cannot both hold where their scopes overlap. Found either
/// through an explicit `contradicts` edge or automatically: same subject and
/// predicate, overlapping scope, incompatible typed values. Text values are
/// never compared automatically.
struct ConflictEntry {
    ConflictSide a, b;             // a.id < b.id
    std::string subject, predicate;
    bool explicit_edge = false;    // a contradicts edge exists
    bool detected = false;         // found by value comparison
    std::optional<Scope> overlap;  // where both claim to hold (for detected ones)
    int n_dependents = 0;          // union of both sides' dependents: the ranking key
    std::vector<std::string> questions;  // open questions about this pair

    nlohmann::json to_json() const;
};

/// Same as a detected conflict, except one scope strictly contains the other:
/// the narrower claim most likely refines the wider one rather than
/// contradicting it. Not a conflict until somebody says so; the suggested
/// action is link(narrower, wider, refines).
struct RefinementEntry {
    ConflictSide narrower, wider;
    std::string subject, predicate;

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
    bridge_source,    // a source that wraps a node of this store; link to that node with an edge instead
    already_replaced, // the node has an active replacement; replace that one instead
    stale_premise,    // recompute() needs its justifiers recomputed first
};
std::string_view to_string(ErrorCode);

struct Error : std::runtime_error {
    ErrorCode code;
    Error(ErrorCode c, const std::string& what) : std::runtime_error(what), code(c) {}
};

}  // namespace beliefdb
