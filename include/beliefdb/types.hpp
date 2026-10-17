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
    /// Ids of close claims the caller has seen and still wants a separate claim
    /// for (then link it refines/contradicts). Without it a structural duplicate
    /// is refused: same subject and predicate, overlapping scope, compatible value.
    std::vector<std::string> despite;
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
    std::vector<std::string> despite;  // as in ClaimInput
};

/// A machine inference that nobody has confirmed yet. Same shape as a derived
/// claim, different namespace (h_): it can not justify anything, it adds no
/// sources (its evidence set is the union of its premises'), it is counted
/// under its own quota and it expires unreviewed. confirm() turns it into a claim.
struct HypothesisInput {
    std::string text;
    std::string subject;
    std::string predicate;
    Value value;
    std::vector<std::string> from;  // claims only; a hypothesis can not be built on a hypothesis (depth 1)
    std::string rule;               // rule id or name
    Scope scope;                    // may only narrow the intersection of the premises' scopes
    std::string domain = "general";
    Sensitivity sensitivity = Sensitivity::domain;
    std::vector<std::string> despite;  // as in ClaimInput
};

/// What turns a hypothesis into a claim: evidence from the world or a human,
/// never another inference. The new claim keeps the derives_from edges of the
/// hypothesis and adds `supports` from `support`; the hypothesis is superseded by it.
struct ConfirmInput {
    std::string hypothesis;
    std::vector<std::string> support;  // source or claim ids; must be non-empty
    double credence = 0.5;
    CredenceMethod method = CredenceMethod::stated;  // as for assert
};

// ---- search and dedup (Ф5) ---------------------------------------------------

/// Where to get embeddings. No default URL on purpose: nothing points outside
/// unless configured (addendum 1, item 10). OpenAI-style POST {base_url}/v1/embeddings.
struct EmbeddingConfig {
    std::string base_url;         // e.g. "http://casablanca:12000"
    std::string model = "bge-m3"; // sent as `model`; stored next to every vector as model_id
    int timeout_ms = 3000;
    int backoff_seconds = 60;     // after a failure, do not try again for this long (writes must not stall)
};

struct EmbeddingStatus {
    bool configured = false;
    std::string model;
    bool available = false;    // last call succeeded, or none failed yet
    std::string last_error;
    int dim = 0;               // of the stored vectors for this model
    long n_vectors = 0;        // stored for this model
    long n_missing = 0;        // live claims/hypotheses without a vector for this model (reindex_embeddings fills them)
    nlohmann::json to_json() const;
};

struct SimilarQuery {
    std::string text;
    std::string subject;
    std::string predicate;
    std::optional<Value> value;  // compared with structural matches when given
    std::optional<Scope> scope;  // for the structural check; absent = any
    std::string domain;          // empty = all
    int limit = 10;
};

/// One candidate. Candidate generation only: whether it is "the same" is
/// decided on the structural fields (subject, predicate, scope, value), never
/// on text similarity alone.
struct SimilarEntry {
    std::string id;
    NodeType type = NodeType::claim;
    std::string text;
    std::string subject, predicate;
    std::string domain;
    Status status = Status::active;
    Value value;
    Scope scope;
    bool structural = false;          // same subject and predicate, overlapping scope
    std::optional<bool> compatible;   // value comparison with the query's value, when both are typed
    std::optional<double> fts_rank;   // FTS5 bm25 rank: lower is better
    std::optional<double> cosine;     // embedding similarity, when available
    nlohmann::json to_json() const;
};

struct SimilarResult {
    std::vector<SimilarEntry> entries;  // structural first, then by cosine, then by FTS rank
    std::string mode;                   // "fts" | "fts+embeddings"
    std::string notice;                 // why embeddings were not used, if they were not
    nlohmann::json to_json() const;
};

// ---- write discipline (Ф5) -------------------------------------------------

/// Quotas and lifetimes. Configuration, not findings: the defaults are
/// deliberately tight, an agent that writes more than this in one go is
/// almost certainly retelling itself (spec, "дисциплина записи").
struct Policy {
    int batch_claims = 20;            // claims (assert, derive, confirm) per batch; the batch closes when reached
    int batch_hypotheses = 5;         // hypotheses per batch
    int daily_claims = 50;            // per (domain, actor) and UTC day, across batches
    int daily_hypotheses = 10;
    int open_hypotheses = 20;         // unreviewed hypotheses per domain: the queue backs up instead of growing
    double batch_idle_seconds = 1800; // a batch with no write for this long closes itself
    double hypothesis_ttl_days = 30;  // unreviewed for this long => retracted as expired
    bool require_batch = false;       // claims and hypotheses only inside an open batch (for agents)
};

/// A batch is the unit of writing and of review: the actor states a purpose
/// before writing, quotas are counted against it, and a human can look at one
/// batch and accept or reject it whole.
struct BatchEntry {
    std::string id;
    std::string actor;
    std::string purpose;
    std::string started_at;
    std::string last_write_at;
    std::string closed_at;      // empty while open
    std::string closed_reason;  // "ended" | "idle" | "quota: ..." | "rejected: ..."
    int n_claims = 0;
    int n_hypotheses = 0;
    int n_sources = 0;
    std::vector<std::string> nodes;  // created in this batch, in order

    nlohmann::json to_json() const;
};

struct HypothesisEntry {
    std::string id;
    std::string text;
    std::string subject, predicate;
    Value value;
    Scope scope;
    std::string domain;
    std::vector<std::string> from;
    std::string rule;                  // rule id
    std::optional<Credence> credence;  // a ceiling: min over the premises, never more
    Status status = Status::active;    // active = awaiting review; superseded = confirmed; retracted = rejected or expired
    std::string created_at;
    std::string expires_at;
    std::string batch_id;
    std::string outcome;               // "confirmed as c_..." | "rejected: ..." | "expired" | "" while open
    double days_left = 0;              // until expiry, while open

    nlohmann::json to_json() const;
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

/// State of the human review of one detected pair (addendum 3). The pair is
/// found again on every scan; what is kept is the decision about it, and a
/// decision expires when either side changes (fingerprint).
enum class TriageState { open, dismissed, resolved, vanished, promoted };
std::string_view to_string(TriageState);
std::optional<TriageState> triage_state_from(std::string_view);

struct Triage {
    TriageState state = TriageState::open;
    std::string first_seen_at;
    std::string last_seen_at;
    int times_seen = 0;
    std::string decided_by;
    std::string decided_at;
    std::string reason;       // why dismissed / how resolved
    std::string expired_at;   // set when a decision was invalidated by a change of either side
    int open_days = 0;        // days since first_seen while open: long-open pairs point at a poor scope model
};

struct TriageStats {
    int open = 0, dismissed = 0, resolved = 0, vanished = 0, promoted = 0;
    /// (promoted + resolved) / (promoted + resolved + dismissed): precision of the detector itself
    std::optional<double> precision;
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
    Triage triage;                 // review state of this pair (addendum 3)

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
    not_a_conflict,   // triage decision about a pair the detector does not find
    no_batch,         // the policy requires an open batch, or the batch given is closed / somebody else's
    quota_exceeded,   // batch, daily or open-hypotheses quota reached
    duplicate_claim,  // a claim with the same subject, predicate, overlapping scope and compatible value exists
};
std::string_view to_string(ErrorCode);

struct Error : std::runtime_error {
    ErrorCode code;
    Error(ErrorCode c, const std::string& what) : std::runtime_error(what), code(c) {}
};

}  // namespace beliefdb
