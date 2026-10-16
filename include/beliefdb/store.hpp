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
    void link(const std::string& src, const std::string& dst, EdgeType type,
              std::optional<std::string> rule = std::nullopt);

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

    struct Counts { long nodes = 0, edges = 0, sources = 0, claims = 0, rules = 0; };
    Counts counts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Store(std::unique_ptr<Impl>);
};

}  // namespace beliefdb
