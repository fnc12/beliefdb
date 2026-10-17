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
    std::vector<ConflictEntry> conflicts(const std::string& domain = "") const;
    /// Conflicts involving one claim.
    std::vector<ConflictEntry> conflicts_of(const std::string& id) const;
    /// Pairs that disagree only where one scope strictly contains the other:
    /// refinement candidates, listed until a `refines` edge links them.
    std::vector<RefinementEntry> refinements(const std::string& domain = "") const;

    struct Counts { long nodes = 0, edges = 0, sources = 0, claims = 0, rules = 0; };
    Counts counts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Store(std::unique_ptr<Impl>);
};

}  // namespace beliefdb
