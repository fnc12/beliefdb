// beliefdb — the store: a graph of justified claims on top of SQLite.
#pragma once

#include "types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace beliefdb {

class Store {
public:
    /// Opens (or creates) the database at `path`. ":memory:" works.
    /// `actor` is recorded in the revision log for every write.
    static Store open(const std::string& path, std::string actor = "unknown");
    ~Store();
    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    void set_actor(std::string actor);

    // ---- write (each call is one transaction) ----
    /// Records primary evidence. Deduplicated by content hash: returns the
    /// existing id when the same content was observed before.
    std::string observe(const SourceInput&);
    /// Defines a named inference rule. Returns its id; name must be unique.
    std::string define_rule(const RuleInput&);
    /// Declares a predicate's value type (and unit / enum set). Predicates of
    /// type bool/number/ref/text are also created on first use; enumerations
    /// must be declared here first because the member set cannot be inferred.
    void define_predicate(const PredicateInput&);
    /// Records a claim justified by `support`. Rejected without support.
    /// valid_from defaults to the earliest observed_at among source supports.
    std::string assert_claim(const ClaimInput&);
    /// Records a claim derived from other claims through a rule. Its scope is
    /// the intersection of the premises' scopes, narrowed by `scope` if given;
    /// widening it or deriving from premises that are never valid together is an error.
    std::string derive(const DeriveInput&);
    /// Adds an edge. supports / derives_from / assumes form one DAG and are
    /// checked for cycles over their union. contradicts is symmetric and is
    /// stored as (min(id), max(id)). replaces: src is the new node, dst the old.
    /// For replaces the old node (dst) becomes superseded and its dependents
    /// stale, as with supersede(); `reason` is recorded with that.
    void link(const std::string& src, const std::string& dst, EdgeType type,
              std::optional<std::string> rule = std::nullopt, const std::string& reason = "");

    // ---- change (Ф3): nothing is ever deleted, consequences are marked, not recomputed ----
    /// Marks a source, claim or rule retracted and flags every dependent credence
    /// stale (for a rule: every claim derived with it). Dependents keep their old
    /// value with the flag; why() shows the knocked-out support.
    void retract(const std::string& id, const std::string& reason);
    /// Records a corrected node, links it `replaces` the old one, marks the old
    /// superseded and its dependents stale. The old node stays readable. Only one
    /// node in a replaces chain is active; replacing a replaced node is an error.
    std::string supersede(const std::string& old_id, const SourceInput& corrected, const std::string& reason);
    std::string supersede(const std::string& old_id, const ClaimInput& corrected, const std::string& reason);
    std::string supersede(const std::string& old_id, const DeriveInput& corrected, const std::string& reason);
    /// Explicit recompute of one stale claim. Follows replaces chains: an edge to
    /// a superseded justifier is rewired to its active replacement. Retracted
    /// justifiers are dropped; with none left the call fails and the node stays
    /// stale. Method `min` recomputes the value itself; any other method needs
    /// the caller's new judgement in `credence`. Justifiers must be recomputed
    /// first (`stale()` lists shallow nodes first).
    void recompute(const std::string& id, std::optional<double> credence = std::nullopt);

    // ---- read ----
    std::optional<Node> get(const std::string& id) const;
    std::optional<Node> find_rule(const std::string& name) const;
    std::optional<Predicate> get_predicate(const std::string& name) const;
    std::vector<Predicate> list_predicates() const;
    std::vector<Edge> edges_from(const std::string& id) const;
    std::vector<Edge> edges_to(const std::string& id) const;
    /// Unrolls the justification of `id` down to sources.
    /// depth < 0 means unlimited.
    WhyNode why(const std::string& id, int depth = -1) const;
    /// Ids that `id` depends on (transitively) through justification edges.
    std::vector<std::string> depends_on(const std::string& id) const;
    /// Ids that depend on `id` (transitively).
    std::vector<std::string> dependents_of(const std::string& id) const;
    /// The sources a node stands on, with counts, weights and dates.
    Nutrition nutrition(const std::string& id) const;
    /// Claims whose credence is flagged stale, shallow first, with the root cause of each flag.
    std::vector<StaleEntry> stale(const std::string& domain = "") const;
    /// What would change below `id` if its credence became `new_value`. Nothing is written.
    /// Nodes reachable only through `min` steps get a projected value; the rest would merely go stale.
    std::vector<WhatIfEntry> what_if(const std::string& id, double new_value) const;
    void set_weights(NutritionWeights);
    NutritionWeights weights() const;

    // ---- questions (Ф4) ----
    /// Records an open question. Returns its id.
    std::string ask(const QuestionInput&);
    /// Appends a dated progress note to an open question.
    void note_progress(const std::string& question_id, const std::string& text);
    /// Closes a question with a resolution: what answered it, or why it is moot.
    /// Closing is a human/explicit act; the store does not judge the acceptance criterion.
    void close_question(const std::string& question_id, const std::string& resolution);
    /// Open questions (all states with include_closed), oldest first.
    std::vector<QuestionEntry> questions(const std::string& domain = "", bool include_closed = false) const;
    QuestionEntry question(const std::string& id) const;

    // ---- conflicts (Ф4) ----
    /// Contradictions, ranked by how much of the graph stands on them. Explicit
    /// `contradicts` edges plus pairs found by comparing typed values of claims
    /// with the same subject and predicate in overlapping scopes. Nothing is
    /// resolved automatically. Domain filters to pairs with at least one side in it.
    /// Every scan also updates the review state of each pair (conflict_triage):
    /// new pairs open, decisions expire when a side changes, pairs that are no
    /// longer found vanish. By default only open and promoted pairs are returned.
    std::vector<ConflictEntry> conflicts(const std::string& domain = "", bool include_decided = false);
    /// Conflicts involving one claim.
    std::vector<ConflictEntry> conflicts_of(const std::string& id, bool include_decided = false);
    /// Review decisions about a detected pair. They stay until either side's
    /// value, scope or status changes; then the pair comes back as open with
    /// the old reason kept.
    void dismiss_conflict(const std::string& a, const std::string& b, const std::string& reason);  // "not a conflict"
    void resolve_conflict(const std::string& a, const std::string& b, const std::string& reason);  // settled by refines / scope / retract
    /// Pairs by review state, and the detector's own precision (Ф6 input).
    TriageStats triage_stats() const;
    /// Pairs that disagree only where one scope strictly contains the other:
    /// refinement candidates, listed until a `refines` edge links them.
    std::vector<RefinementEntry> refinements(const std::string& domain = "") const;

    // ---- batches and write discipline (Ф5) ----
    /// Quotas and lifetimes; see Policy. Per store handle, not persisted.
    void set_policy(Policy);
    Policy policy() const;
    /// Opens a batch for this actor with a stated purpose and makes it current:
    /// every write from now on is counted against it and tagged with it in
    /// revisions. Fails while another batch of this handle is open.
    std::string begin_batch(const std::string& purpose);
    /// Continues an open batch of this actor from another handle (e.g. the next
    /// CLI invocation). Somebody else's or a closed batch is refused.
    void resume_batch(const std::string& id);
    /// Closes the current batch, or `id`. A batch also closes itself when it
    /// reaches a quota or sits idle longer than the policy allows; it then stays
    /// current and refuses writes until the actor begins another one.
    void end_batch(const std::string& id = "");
    /// Forgets the current batch on this handle without closing it (a server
    /// handle serving several actors switches batches per request).
    void leave_batch();
    std::optional<std::string> current_batch() const;
    BatchEntry batch(const std::string& id) const;
    /// Open batches (all with include_closed), newest first.
    std::vector<BatchEntry> batches(bool include_closed = false) const;
    /// Review verdict on a whole batch: every node created in it that is still
    /// in force is retracted, dependents go stale as with retract().
    void reject_batch(const std::string& id, const std::string& reason);

    // ---- hypotheses (Ф5) ----
    /// Records a machine inference. Premises are claims or hypotheses (a
    /// hypothesis on hypotheses is of order 1 + max order of those premises; its
    /// sources stay the union of the leaves, so a tower adds no calories), the
    /// rule must be named, quotas apply (batch, daily, open queue per domain
    /// unless Policy::open_hypotheses is 0). Credence is the MIN over the
    /// premises: a ceiling, not a judgement. Unreviewed hypotheses expire after
    /// Policy::hypothesis_ttl_days unless that is 0.
    std::string hypothesize(const HypothesisInput&);
    /// Turns a hypothesis into a claim on the strength of `support` (sources or
    /// claims). Returns the claim id; the hypothesis is superseded by it.
    /// Rejecting a hypothesis is retract(id, reason).
    std::string confirm(const ConfirmInput&);
    /// Hypotheses awaiting review, oldest first (with include_reviewed: confirmed,
    /// rejected and expired ones too). Overdue ones are expired first.
    std::vector<HypothesisEntry> hypotheses(const std::string& domain = "", bool include_reviewed = false);
    HypothesisEntry hypothesis(const std::string& id) const;

    // ---- search and dedup (Ф5) ----
    /// Embedding provider, or nullopt for FTS only. Per store handle. A provider
    /// that fails is skipped for Config::backoff_seconds and search says so.
    void set_embeddings(std::optional<EmbeddingConfig>);
    EmbeddingStatus embedding_status() const;
    /// Candidates close to a would-be claim, for the agent to look at before
    /// writing: structural matches (same subject and predicate, overlapping
    /// scope) over live claims and pending hypotheses, then trigram full-text
    /// matches and, when embeddings are configured and reachable, nearest
    /// vectors. Text similarity only generates candidates; what counts as the
    /// same claim is decided on the structural fields (addendum 1, item 10).
    SimilarResult similar(const SimilarQuery&);
    /// Full-text (and, when available, semantic) search over claims and
    /// hypotheses of every status.
    SimilarResult search(const std::string& query, const std::string& domain = "", int limit = 20);
    /// The nearest live claims/hypotheses to a stored node by its stored vector
    /// (never calls the provider); without a vector, full text on its words with a notice.
    SimilarResult neighbours(const NeighbourQuery&);
    /// Live claims and hypotheses created at or after `since`, oldest first
    /// (what a periodic reader has not seen yet). domain "" = every domain.
    std::vector<SimilarEntry> recent(const std::string& domain, const std::string& since, int limit = 50);
    /// Computes vectors for live claims/hypotheses that have none for the
    /// configured model (new nodes written while the provider was down, or a
    /// model change). Returns how many were written; stops at the first failure.
    long reindex_embeddings();

    struct Counts { long nodes = 0, edges = 0, sources = 0, claims = 0, hypotheses = 0, rules = 0, open_batches = 0; };
    Counts counts() const;
    /// The shape of the store for a reader: live (active or stale) nodes per domain, with the newest creation time.
    struct DomainCount { std::string domain; long claims = 0, hypotheses = 0, sources = 0, questions = 0; std::string newest; };
    std::vector<DomainCount> domain_counts() const;
    /// Nodes of one type, newest first, paged: a reader's listing. domain "" = every domain;
    /// status "" = live (active or stale), "all" = every status, else exact.
    struct ListQuery { std::string domain, type = "claim", status; int offset = 0, limit = 50; };
    std::vector<Node> list(const ListQuery&) const;
    long list_count(const ListQuery&) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Store(std::unique_ptr<Impl>);
};

}  // namespace beliefdb
