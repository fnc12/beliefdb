#include "beliefdb/store.hpp"

#include "schema.hpp"
#include "util.hpp"

#include <algorithm>
#include <set>
#include <unordered_set>

namespace beliefdb {

using nlohmann::json;
using namespace schema;

struct Store::Impl {
    Storage storage;
    std::string actor;

    explicit Impl(const std::string& path, std::string actor_) : storage(schema::make_storage(path)), actor(std::move(actor_)) {}

    // ---- helpers ----------------------------------------------------------

    static std::string type_prefix(NodeType t) {
        switch (t) {
            case NodeType::source: return "s_";
            case NodeType::claim: return "c_";
            case NodeType::hypothesis: return "h_";
            case NodeType::question: return "q_";
            case NodeType::rule: return "r_";
        }
        return "n_";
    }

    static Node to_node(const NodeRow& r, const CredenceRow* c) {
        Node n;
        n.id = r.id;
        n.type = node_type_from(r.type).value_or(NodeType::claim);
        n.domain = r.domain;
        n.created_at = r.created_at;
        n.updated_at = r.updated_at;
        n.status = status_from(r.status).value_or(Status::active);
        n.sensitivity = sensitivity_from(r.sensitivity).value_or(Sensitivity::domain);
        n.payload = json::parse(r.payload, nullptr, false);
        if (c) {
            Credence cr;
            cr.value = c->value;
            cr.method = credence_method_from(c->method).value_or(CredenceMethod::manual);
            cr.inputs = json::parse(c->inputs, nullptr, false);
            cr.computed_at = c->computed_at;
            cr.stale = c->stale != 0;
            n.credence = cr;
        }
        return n;
    }

    static Edge to_edge(const EdgeRow& r) {
        Edge e;
        e.src = r.src;
        e.dst = r.dst;
        e.type = edge_type_from(r.type).value_or(EdgeType::supports);
        e.rule_id = r.rule_id;
        e.weight = r.weight;
        e.created_at = r.created_at;
        return e;
    }

    std::optional<Node> load(const std::string& id) {
        auto row = storage.get_pointer<NodeRow>(id);
        if (!row) return std::nullopt;
        auto cr = storage.get_pointer<CredenceRow>(id);
        return to_node(*row, cr.get());
    }

    Node must_load(const std::string& id) {
        auto n = load(id);
        if (!n) throw Error(ErrorCode::not_found, "node not found: " + id);
        return *n;
    }

    void record_revision(const std::string& node_id, const std::string& reason, const json* before, const json* after) {
        RevisionRow r;
        r.node_id = node_id;
        r.at = util::now_iso();
        r.actor = actor;
        if (before) r.before = before->dump();
        if (after) r.after = after->dump();
        r.reason = reason;
        storage.insert(r);
    }

    std::string insert_node(NodeType type, const std::string& domain, Sensitivity sens, const json& payload,
                            const std::string& reason) {
        const auto now = util::now_iso();
        NodeRow r;
        r.id = type_prefix(type) + util::ulid();
        r.type = std::string(to_string(type));
        r.domain = domain;
        r.created_at = now;
        r.updated_at = now;
        r.status = std::string(to_string(Status::active));
        r.sensitivity = std::string(to_string(sens));
        r.payload = payload.dump();
        storage.replace(r);
        record_revision(r.id, reason, nullptr, &payload);
        return r.id;
    }

    void insert_credence(const std::string& node_id, double value, CredenceMethod method, const json& inputs) {
        CredenceRow c;
        c.node_id = node_id;
        c.value = value;
        c.method = std::string(to_string(method));
        c.inputs = inputs.dump();
        c.computed_at = util::now_iso();
        c.stale = 0;
        storage.replace(c);
    }

    void insert_edge(const std::string& src, const std::string& dst, EdgeType type, std::optional<std::string> rule_id) {
        using namespace sqlite_orm;
        const auto type_s = std::string(to_string(type));
        const auto exists = storage.count<EdgeRow>(
            where(c(&EdgeRow::src) == src && c(&EdgeRow::dst) == dst && c(&EdgeRow::type) == type_s));
        if (exists > 0) throw Error(ErrorCode::duplicate, "edge already exists: " + src + " -" + type_s + "-> " + dst);
        EdgeRow e;
        e.src = src;
        e.dst = dst;
        e.type = type_s;
        e.rule_id = std::move(rule_id);
        e.weight = 1.0;
        e.created_at = util::now_iso();
        storage.replace(e);
        const json after{{"src", src}, {"dst", dst}, {"type", type_s}};
        record_revision(src, "link " + type_s, nullptr, &after);
    }

    /// Dependency relation used for acyclicity and for why():
    ///   X derives_from Y, X assumes Y  => X depends on Y   (edge src -> dst)
    ///   Y supports X                   => X depends on Y   (edge dst <- src)
    /// Returns the transitive closure of nodes `start` depends on.
    std::vector<std::string> closure(const std::string& start) {
        using namespace sqlite_orm;
        std::unordered_set<std::string> seen;
        std::vector<std::string> frontier{start}, out;
        const std::vector<std::string> fwd_types{"derives_from", "assumes"};
        while (!frontier.empty()) {
            std::vector<std::string> next;
            auto fwd = storage.select(&EdgeRow::dst, where(in(&EdgeRow::src, frontier) && in(&EdgeRow::type, fwd_types)));
            auto back = storage.select(&EdgeRow::src, where(in(&EdgeRow::dst, frontier) && c(&EdgeRow::type) == "supports"));
            for (auto* v : {&fwd, &back})
                for (auto& id : *v)
                    if (seen.insert(id).second) { out.push_back(id); next.push_back(id); }
            frontier = std::move(next);
        }
        return out;
    }

    void check_acyclic(const std::string& dependent, const std::string& dependency) {
        if (dependent == dependency) throw Error(ErrorCode::cycle, "a node cannot justify itself: " + dependent);
        auto reach = closure(dependency);
        if (std::find(reach.begin(), reach.end(), dependent) != reach.end())
            throw Error(ErrorCode::cycle, dependency + " already depends on " + dependent + "; the edge would close a cycle");
    }

    /// A node that may serve as justification: exists, is a source or claim,
    /// and has not been retracted or superseded.
    Node check_support(const std::string& id, bool claims_only) {
        auto n = load(id);
        if (!n) throw Error(ErrorCode::invalid_support, "support not found: " + id);
        const bool ok_type = claims_only ? n->type == NodeType::claim : (n->type == NodeType::source || n->type == NodeType::claim);
        if (!ok_type)
            throw Error(ErrorCode::invalid_support,
                        id + " is a " + std::string(to_string(n->type)) + (claims_only ? ", only claims can be derived from" : ", only sources and claims can support"));
        if (n->status == Status::retracted || n->status == Status::superseded)
            throw Error(ErrorCode::invalid_support, id + " is " + std::string(to_string(n->status)) + " and cannot justify anything");
        return *n;
    }

    std::optional<Node> rule_by_name(const std::string& name) {
        using namespace sqlite_orm;
        auto ids = storage.select(&NodeRow::id, where(c(&NodeRow::type) == "rule" &&
                                                      json_extract<std::string>(&NodeRow::payload, "$.name") == name));
        if (ids.empty()) return std::nullopt;
        return load(ids.front());
    }

    Node resolve_rule(const std::string& id_or_name) {
        if (auto n = load(id_or_name); n && n->type == NodeType::rule) return *n;
        if (auto n = rule_by_name(id_or_name)) return *n;
        throw Error(ErrorCode::not_found, "rule not found: " + id_or_name);
    }

    static std::string label_of(const Node& n) {
        const auto& p = n.payload;
        auto str = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
        switch (n.type) {
            case NodeType::source: return str("kind") + ": " + str("uri");
            case NodeType::rule: return str("name");
            default: return str("text");
        }
    }

    WhyNode why_rec(const std::string& id, int depth, std::vector<std::string>& path) {
        using namespace sqlite_orm;
        const Node n = must_load(id);
        WhyNode w;
        w.id = n.id;
        w.type = n.type;
        w.label = label_of(n);
        w.status = n.status;
        w.credence = n.credence;
        if (n.type == NodeType::source) return w;

        auto supports = storage.get_all<EdgeRow>(where(c(&EdgeRow::dst) == id && c(&EdgeRow::type) == "supports"),
                                                 order_by(&EdgeRow::created_at));
        const std::vector<std::string> fwd_types{"derives_from", "assumes"};
        auto bases = storage.get_all<EdgeRow>(where(c(&EdgeRow::src) == id && in(&EdgeRow::type, fwd_types)),
                                              multi_order_by(order_by(&EdgeRow::type).desc(), order_by(&EdgeRow::created_at)));
        if (supports.empty() && bases.empty()) return w;
        if (depth == 0) { w.truncated = true; return w; }

        path.push_back(id);
        auto visit = [&](const std::string& child_id, EdgeType via, const std::optional<std::string>& rule_id) {
            WhyNode child;
            if (std::find(path.begin(), path.end(), child_id) != path.end()) {
                child.id = child_id;
                child.label = "(cycle)";
                child.truncated = true;
            } else {
                child = why_rec(child_id, depth < 0 ? -1 : depth - 1, path);
            }
            child.via = via;
            if (rule_id) {
                if (auto r = load(*rule_id)) child.rule = label_of(*r);
            }
            w.children.push_back(std::move(child));
        };
        for (const auto& e : supports) visit(e.src, EdgeType::supports, std::nullopt);
        for (const auto& e : bases) visit(e.dst, edge_type_from(e.type).value_or(EdgeType::derives_from), e.rule_id);
        path.pop_back();
        return w;
    }
};

// ---- lifecycle -----------------------------------------------------------------

Store::Store(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Store::~Store() = default;
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;

Store Store::open(const std::string& path, std::string actor) {
    auto impl = std::make_unique<Impl>(path, std::move(actor));
    auto& st = impl->storage;
    st.open_forever();
    st.busy_timeout(5000);
    if (path != ":memory:" && !path.empty()) {
        st.pragma.journal_mode(sqlite_orm::journal_mode::WAL);
        st.pragma.synchronous(1);  // NORMAL: durable on power loss with WAL, far fewer fsyncs
    }
    st.sync_schema();
    return Store(std::move(impl));
}

void Store::set_actor(std::string actor) { impl_->actor = std::move(actor); }

// ---- write ---------------------------------------------------------------------

std::string Store::observe(const SourceInput& in) {
    using namespace sqlite_orm;
    if (in.content.empty()) throw Error(ErrorCode::bad_input, "source.content is required");
    if (in.trust_hint && (*in.trust_hint < 0.0 || *in.trust_hint > 1.0))
        throw Error(ErrorCode::bad_input, "trust_hint must be within [0, 1]");
    if (in.domain.empty()) throw Error(ErrorCode::bad_input, "domain is required");
    if (!in.observed_at.empty() && !util::looks_like_iso8601(in.observed_at))
        throw Error(ErrorCode::bad_input, "observed_at must be ISO-8601 (YYYY-MM-DD[THH:MM:SS...]), got: " + in.observed_at);

    const auto hash = util::sha256_hex(in.content);
    auto guard = impl_->storage.transaction_guard();
    auto existing = impl_->storage.select(
        &NodeRow::id, where(c(&NodeRow::type) == "source" && json_extract<std::string>(&NodeRow::payload, "$.content_hash") == hash));
    if (!existing.empty()) { guard.commit(); return existing.front(); }

    json payload{{"kind", to_string(in.kind)},
                 {"uri", in.uri},
                 {"content", in.content},
                 {"content_hash", hash},
                 {"attribution", in.attribution},
                 {"observed_at", in.observed_at.empty() ? util::now_iso() : in.observed_at},
                 {"retrieved_at", util::now_iso()}};
    // A manual hint is bootstrap only and stays visibly manual, so it is never
    // confused with reliability measured from predictions (Ф6).
    if (in.trust_hint) payload["trust_hint"] = json{{"value", *in.trust_hint}, {"method", "manual"}};
    auto id = impl_->insert_node(NodeType::source, in.domain, in.sensitivity, payload, "observe");
    guard.commit();
    return id;
}

std::string Store::define_rule(const RuleInput& in) {
    if (in.name.empty()) throw Error(ErrorCode::bad_input, "rule.name is required");
    auto guard = impl_->storage.transaction_guard();
    if (impl_->rule_by_name(in.name)) throw Error(ErrorCode::duplicate, "rule already defined: " + in.name);
    const json payload{{"name", in.name}, {"description", in.description}, {"n_applied", 0}, {"n_correct", 0}, {"brier", nullptr}};
    auto id = impl_->insert_node(NodeType::rule, in.domain, Sensitivity::public_, payload, "define_rule");
    guard.commit();
    return id;
}

std::string Store::assert_claim(const ClaimInput& in) {
    if (in.text.empty()) throw Error(ErrorCode::bad_input, "claim.text is required");
    if (in.domain.empty()) throw Error(ErrorCode::bad_input, "domain is required");
    if (in.support.empty()) throw Error(ErrorCode::no_support, "a claim needs at least one supporting source or claim");
    if (in.scope.empty()) throw Error(ErrorCode::empty_scope, "a claim needs a scope: validity interval, conditions or a note");
    if (!is_credence_step(in.credence)) throw Error(ErrorCode::bad_credence, credence_step_error(in.credence));
    switch (in.method) {
        case CredenceMethod::stated: case CredenceMethod::measured: case CredenceMethod::llm_judgment: case CredenceMethod::manual: break;
        default:
            throw Error(ErrorCode::bad_credence, std::string("method ") + std::string(to_string(in.method)) +
                                                     " is a derivation method; assert accepts stated|measured|llm_judgment|manual");
    }

    auto guard = impl_->storage.transaction_guard();
    std::set<std::string> uniq(in.support.begin(), in.support.end());
    for (const auto& sid : uniq) impl_->check_support(sid, false);

    json payload{{"text", in.text}, {"subject", in.subject}, {"predicate", in.predicate}, {"value", in.value}, {"scope", in.scope.to_json()}};
    auto id = impl_->insert_node(NodeType::claim, in.domain, in.sensitivity, payload, "assert");
    impl_->insert_credence(id, in.credence, in.method, json{{"support", json(uniq)}});
    for (const auto& sid : uniq) impl_->insert_edge(sid, id, EdgeType::supports, std::nullopt);
    guard.commit();
    return id;
}

std::string Store::derive(const DeriveInput& in) {
    if (in.text.empty()) throw Error(ErrorCode::bad_input, "claim.text is required");
    if (in.domain.empty()) throw Error(ErrorCode::bad_input, "domain is required");
    if (in.from.empty()) throw Error(ErrorCode::no_support, "derive needs at least one claim in `from`");
    if (in.scope.empty()) throw Error(ErrorCode::empty_scope, "a derived claim needs a scope too");
    switch (in.method) {
        case CredenceMethod::min: case CredenceMethod::rule: case CredenceMethod::noisy_or:
        case CredenceMethod::llm_judgment: case CredenceMethod::manual: break;
        default:
            throw Error(ErrorCode::bad_credence, std::string("method ") + std::string(to_string(in.method)) +
                                                     " is an observation method; derive accepts min|rule|noisy_or|llm_judgment|manual");
    }

    auto guard = impl_->storage.transaction_guard();
    const Node rule = impl_->resolve_rule(in.rule);
    std::set<std::string> uniq(in.from.begin(), in.from.end());
    json inputs_from = json::array();
    double min_value = 1.0;
    for (const auto& fid : uniq) {
        const Node base = impl_->check_support(fid, true);
        if (!base.credence) throw Error(ErrorCode::invalid_support, fid + " has no credence");
        min_value = std::min(min_value, base.credence->value);
        inputs_from.push_back({{"id", fid}, {"credence", base.credence->value}, {"stale", base.credence->stale}});
    }
    double value;
    if (in.credence) {
        if (!is_credence_step(*in.credence)) throw Error(ErrorCode::bad_credence, credence_step_error(*in.credence));
        value = *in.credence;
    } else {
        if (in.method != CredenceMethod::min)
            throw Error(ErrorCode::bad_credence, "credence is required unless method is min");
        value = min_value;
    }

    json payload{{"text", in.text}, {"subject", in.subject}, {"predicate", in.predicate}, {"value", in.value}, {"scope", in.scope.to_json()}};
    auto id = impl_->insert_node(NodeType::claim, in.domain, in.sensitivity, payload, "derive");
    impl_->insert_credence(id, value, in.method, json{{"from", inputs_from}, {"rule", rule.id}, {"min", min_value}});
    for (const auto& fid : uniq) impl_->insert_edge(id, fid, EdgeType::derives_from, rule.id);

    // rule track record
    {
        NodeRow row = impl_->storage.get<NodeRow>(rule.id);
        json before = json::parse(row.payload);
        json after = before;
        after["n_applied"] = before.value("n_applied", 0) + 1;
        row.payload = after.dump();
        row.updated_at = util::now_iso();
        impl_->storage.update(row);
        impl_->record_revision(rule.id, "applied to " + id, &before, &after);
    }
    guard.commit();
    return id;
}

void Store::link(const std::string& src_in, const std::string& dst_in, EdgeType type, std::optional<std::string> rule) {
    // contradicts is symmetric: stored once as (min(id), max(id)) so the primary
    // key rejects the mirrored duplicate. src/dst carry no direction for it.
    const bool canonical = type == EdgeType::contradicts && dst_in < src_in;
    const std::string& src = canonical ? dst_in : src_in;
    const std::string& dst = canonical ? src_in : dst_in;
    auto guard = impl_->storage.transaction_guard();
    const Node a = impl_->must_load(src);
    const Node b = impl_->must_load(dst);
    auto require = [&](bool ok, const char* what) {
        if (!ok) throw Error(ErrorCode::bad_input, std::string("edge ") + std::string(to_string(type)) + ": " + what);
    };
    const bool a_claim = a.type == NodeType::claim, b_claim = b.type == NodeType::claim;
    std::optional<std::string> rule_id;
    switch (type) {
        case EdgeType::supports:
            require(a.type == NodeType::source || a_claim, "src must be a source or claim");
            require(b_claim, "dst must be a claim");
            require(a.status != Status::retracted && a.status != Status::superseded, "src is retracted/superseded");
            impl_->check_acyclic(dst, src);
            break;
        case EdgeType::derives_from:
            require(a_claim && b_claim, "both ends must be claims");
            if (!rule) throw Error(ErrorCode::bad_input, "derives_from requires a rule");
            rule_id = impl_->resolve_rule(*rule).id;
            impl_->check_acyclic(src, dst);
            break;
        case EdgeType::assumes:
            require(a_claim && b_claim, "both ends must be claims");
            impl_->check_acyclic(src, dst);
            break;
        case EdgeType::contradicts:
        case EdgeType::refines:
            require(a_claim && b_claim, "both ends must be claims");
            require(src != dst, "a claim cannot relate to itself");
            break;
        case EdgeType::answers:
            require(a_claim, "src must be a claim");
            require(b.type == NodeType::question, "dst must be a question");
            break;
        case EdgeType::replaces:
            require(src != dst, "a node cannot replace itself");
            require(a.type == b.type, "replacement must have the same type as the replaced node");
            break;
    }
    impl_->insert_edge(src, dst, type, rule_id);
    guard.commit();
}

// ---- read ----------------------------------------------------------------------

std::optional<Node> Store::get(const std::string& id) const { return impl_->load(id); }
std::optional<Node> Store::find_rule(const std::string& name) const { return impl_->rule_by_name(name); }

std::vector<Edge> Store::edges_from(const std::string& id) const {
    using namespace sqlite_orm;
    std::vector<Edge> out;
    for (const auto& r : impl_->storage.get_all<EdgeRow>(where(c(&EdgeRow::src) == id), order_by(&EdgeRow::created_at)))
        out.push_back(Impl::to_edge(r));
    return out;
}

std::vector<Edge> Store::edges_to(const std::string& id) const {
    using namespace sqlite_orm;
    std::vector<Edge> out;
    for (const auto& r : impl_->storage.get_all<EdgeRow>(where(c(&EdgeRow::dst) == id), order_by(&EdgeRow::created_at)))
        out.push_back(Impl::to_edge(r));
    return out;
}

WhyNode Store::why(const std::string& id, int depth) const {
    std::vector<std::string> path;
    return impl_->why_rec(id, depth, path);
}

std::vector<std::string> Store::depends_on(const std::string& id) const {
    impl_->must_load(id);
    return impl_->closure(id);
}

Store::Counts Store::counts() const {
    using namespace sqlite_orm;
    Counts k;
    k.nodes = static_cast<long>(impl_->storage.count<NodeRow>());
    k.edges = static_cast<long>(impl_->storage.count<EdgeRow>());
    k.sources = static_cast<long>(impl_->storage.count<NodeRow>(where(c(&NodeRow::type) == "source")));
    k.claims = static_cast<long>(impl_->storage.count<NodeRow>(where(c(&NodeRow::type) == "claim")));
    k.rules = static_cast<long>(impl_->storage.count<NodeRow>(where(c(&NodeRow::type) == "rule")));
    return k;
}

}  // namespace beliefdb
