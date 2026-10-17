#include "beliefdb/store.hpp"

#include "schema.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>
#include <set>
#include <unordered_set>

namespace beliefdb {

using nlohmann::json;
using namespace schema;

struct Store::Impl {
    Storage storage;
    std::string actor;
    NutritionWeights weights;
    bool in_txn = false;

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

    Node resolve_rule(const std::string& id_or_name, bool must_be_active = true) {
        std::optional<Node> n = load(id_or_name);
        if (!n || n->type != NodeType::rule) n = rule_by_name(id_or_name);
        if (!n) throw Error(ErrorCode::not_found, "rule not found: " + id_or_name);
        if (must_be_active && n->status != Status::active)
            throw Error(ErrorCode::invalid_support, "rule " + n->id + " is " + std::string(to_string(n->status)) + " and cannot be applied");
        return *n;
    }

    static Predicate to_predicate(const PredicateRow& r) {
        Predicate p;
        p.name = r.name;
        p.value_type = value_type_from(r.value_type).value_or(ValueType::text);
        p.unit = r.unit;
        auto ev = json::parse(r.enum_values, nullptr, false);
        if (ev.is_array())
            for (const auto& v : ev) if (v.is_string()) p.enum_values.push_back(v.get<std::string>());
        p.note = r.note;
        p.created_at = r.created_at;
        return p;
    }

    std::optional<Predicate> load_predicate(const std::string& name) {
        auto row = storage.get_pointer<PredicateRow>(name);
        if (!row) return std::nullopt;
        return to_predicate(*row);
    }

    /// Registry gate: a predicate keeps one value type (and unit family / member
    /// set) for its whole life. First use registers it, except enumerations.
    void check_predicate(const std::string& name, const Value& v) {
        if (name.empty()) return;
        auto p = load_predicate(name);
        if (!p) {
            if (v.type == ValueType::enumeration)
                throw Error(ErrorCode::type_mismatch, "predicate '" + name + "' is not declared; enumerations need define_predicate with their member set first");
            PredicateRow r;
            r.name = name;
            r.value_type = std::string(to_string(v.type));
            r.unit = v.type == ValueType::number ? v.unit : "";
            r.enum_values = "[]";
            r.created_at = util::now_iso();
            storage.replace(r);
            const json after{{"name", name}, {"value_type", r.value_type}, {"unit", r.unit}};
            record_revision("predicate:" + name, "define_predicate (first use)", nullptr, &after);
            return;
        }
        if (p->value_type != v.type)
            throw Error(ErrorCode::type_mismatch, "predicate '" + name + "' is " + std::string(to_string(p->value_type)) + ", got " +
                                                       std::string(to_string(v.type)));
        if (v.type == ValueType::number && p->unit != v.unit)
            throw Error(ErrorCode::type_mismatch, "predicate '" + name + "' is measured in " + (p->unit.empty() ? "(no unit)" : p->unit) +
                                                       ", got " + (v.unit.empty() ? "(no unit)" : v.unit));
        if (v.type == ValueType::enumeration &&
            std::find(p->enum_values.begin(), p->enum_values.end(), v.s) == p->enum_values.end())
            throw Error(ErrorCode::type_mismatch, "'" + v.s + "' is not a member of enum predicate '" + name + "'");
    }

    // ---- evidence (Ф2) -----------------------------------------------------

    static std::vector<std::string> decode_set(const std::vector<char>& blob) {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : blob) {
            if (ch == '\n') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur.push_back(ch);
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
    static std::vector<char> encode_set(const std::set<std::string>& ids) {
        std::vector<char> blob;
        for (const auto& id : ids) { blob.insert(blob.end(), id.begin(), id.end()); blob.push_back('\n'); }
        return blob;
    }

    // ---- status changes (Ф3) ----------------------------------------------
    // Nothing is deleted. A change of status is a revision {status: old} -> {status: new}.

    void set_status(NodeRow& row, Status to, const std::string& reason, const json& extra_payload = json::object()) {
        const json before{{"status", row.status}};
        row.status = std::string(to_string(to));
        row.updated_at = util::now_iso();
        if (!extra_payload.empty()) {
            json p = json::parse(row.payload, nullptr, false);
            if (!p.is_object()) p = json::object();
            for (const auto& [k, v] : extra_payload.items()) p[k] = v;
            row.payload = p.dump();
        }
        storage.update(row);
        json after{{"status", row.status}};
        if (!extra_payload.empty()) after["payload"] = extra_payload;
        record_revision(row.id, reason, &before, &after);
    }

    /// Flags the credence of `ids` stale because of `cause`. A node keeps its
    /// old value; only the flag and the recorded cause change. Nodes without a
    /// credence row (sources, rules) are skipped.
    void mark_stale(const std::vector<std::string>& ids, const std::string& cause, const std::string& reason, const std::string& at) {
        using namespace sqlite_orm;
        for (const auto& id : ids) {
            auto cr = storage.get_pointer<CredenceRow>(id);
            if (!cr) continue;
            if (!cr->stale) {
                cr->stale = 1;
                storage.update(*cr);
            }
            StaleRow m;
            m.node_id = id;
            m.cause = cause;
            m.reason = reason;
            m.at = at;
            storage.replace(m);
            NodeRow row = storage.get<NodeRow>(id);
            if (row.status == "active") set_status(row, Status::stale, "stale because of " + cause);
        }
    }

    /// A node going out of use: every dependent is flagged, and evidence sets
    /// below it are refreshed (a dead justifier no longer contributes sources).
    void knock_out(const std::string& id, const std::string& reason, const std::string& at) {
        const auto down = dependents(id);
        mark_stale(down, id, reason, at);
        for (size_t pass = 0; pass < down.size() + 1; ++pass) {
            bool changed = false;
            for (const auto& d : down) changed = compute_evidence(d) || changed;
            if (!changed) break;
        }
    }

    /// The active end of a replaces chain starting at `id` (id itself if active).
    /// nullopt when the chain ends in a node that is not active (retracted).
    std::optional<std::string> active_replacement(const std::string& id) {
        using namespace sqlite_orm;
        std::string cur = id;
        std::unordered_set<std::string> seen;
        while (seen.insert(cur).second) {
            NodeRow row = storage.get<NodeRow>(cur);
            if (row.status != "superseded") return row.status == "retracted" ? std::nullopt : std::optional(cur);
            auto next = storage.select(&EdgeRow::src, where(c(&EdgeRow::dst) == cur && c(&EdgeRow::type) == "replaces"));
            if (next.empty()) return std::nullopt;
            cur = next.front();
        }
        return std::nullopt;
    }

    /// Preconditions for `new_id replaces old_id`.
    void check_replace(const Node& neu, const Node& old) {
        if (neu.id == old.id) throw Error(ErrorCode::bad_input, "a node cannot replace itself");
        if (neu.type != old.type) throw Error(ErrorCode::bad_input, "replacement must have the same type as the replaced node");
        if (old.status == Status::superseded) {
            auto head = active_replacement(old.id);
            throw Error(ErrorCode::already_replaced, old.id + " is already replaced" + (head ? " by " + *head + "; replace that one instead" : ""));
        }
        if (old.status == Status::retracted)
            throw Error(ErrorCode::bad_input, old.id + " is retracted; a retracted node is not corrected, record the new one on its own");
        if (neu.status != Status::active)
            throw Error(ErrorCode::bad_input, "replacement " + neu.id + " is " + std::string(to_string(neu.status)));
        const auto below = closure(neu.id);
        if (std::find(below.begin(), below.end(), old.id) != below.end())
            throw Error(ErrorCode::bad_input, "replacement " + neu.id + " depends on " + old.id + " which it is meant to replace");
    }

    /// After the replaces edge exists: the old node is superseded, dependents go stale.
    void after_replace(const std::string& new_id, const std::string& old_id, const std::string& reason) {
        const auto at = util::now_iso();
        NodeRow row = storage.get<NodeRow>(old_id);
        set_status(row, Status::superseded, "superseded by " + new_id + (reason.empty() ? "" : ": " + reason),
                   json{{"superseded_by", json{{"id", new_id}, {"reason", reason}, {"at", at}}}});
        knock_out(old_id, "superseded by " + new_id + (reason.empty() ? "" : ": " + reason), at);
    }

    /// A new claim standing on stale premises is stale from birth, for the same causes.
    void inherit_staleness(const std::string& id, const std::set<std::string>& premises) {
        using namespace sqlite_orm;
        for (const auto& p : premises)
            for (const auto& m : storage.get_all<StaleRow>(where(c(&StaleRow::node_id) == p)))
                mark_stale({id}, m.cause, m.reason, m.at);
    }

    /// Justifiers that are still in force: retracted and superseded ones are
    /// kept as edges (why() shows them knocked out) but contribute nothing.
    std::vector<std::string> live_justifiers(const std::string& id) {
        using namespace sqlite_orm;
        auto all = justifiers(id);
        if (all.empty()) return all;
        const std::vector<std::string> dead{"retracted", "superseded"};
        return storage.select(&NodeRow::id, where(in(&NodeRow::id, all) && not in(&NodeRow::status, dead)));
    }

    static std::string notice_of(const Node& n) {
        if (n.status == Status::retracted) {
            const auto r = n.payload.value("retracted", json::object());
            return "retracted" + (r.value("reason", "").empty() ? "" : ": " + r.value("reason", "")) +
                   (r.value("at", "").empty() ? "" : " (" + r.value("at", "").substr(0, 10) + ")");
        }
        if (n.status == Status::superseded) {
            const auto r = n.payload.value("superseded_by", json::object());
            return "superseded by " + r.value("id", "?") + (r.value("reason", "").empty() ? "" : ": " + r.value("reason", "")) +
                   (r.value("at", "").empty() ? "" : " (" + r.value("at", "").substr(0, 10) + ")");
        }
        return "";
    }

    /// Direct justifiers of a node: supports sources/claims and derives_from/assumes bases.
    std::vector<std::string> justifiers(const std::string& id) {
        using namespace sqlite_orm;
        const std::vector<std::string> fwd_types{"derives_from", "assumes"};
        auto a = storage.select(&EdgeRow::src, where(c(&EdgeRow::dst) == id && c(&EdgeRow::type) == "supports"));
        auto b = storage.select(&EdgeRow::dst, where(c(&EdgeRow::src) == id && in(&EdgeRow::type, fwd_types)));
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }

    /// Nodes that depend on `start`, transitively (reverse of closure()).
    std::vector<std::string> dependents(const std::string& start) {
        using namespace sqlite_orm;
        std::unordered_set<std::string> seen;
        std::vector<std::string> frontier{start}, out;
        const std::vector<std::string> fwd_types{"derives_from", "assumes"};
        while (!frontier.empty()) {
            std::vector<std::string> next;
            auto up = storage.select(&EdgeRow::src, where(in(&EdgeRow::dst, frontier) && in(&EdgeRow::type, fwd_types)));
            auto sup = storage.select(&EdgeRow::dst, where(in(&EdgeRow::src, frontier) && c(&EdgeRow::type) == "supports"));
            for (auto* v : {&up, &sup})
                for (auto& id : *v)
                    if (seen.insert(id).second) { out.push_back(id); next.push_back(id); }
            frontier = std::move(next);
        }
        return out;
    }

    /// Recomputes the evidence row of one node from its justifiers. Returns true if it changed.
    bool compute_evidence(const std::string& id) {
        std::set<std::string> sources;
        int depth = 0;
        for (const auto& j : live_justifiers(id)) {
            if (j.rfind("s_", 0) == 0) { sources.insert(j); depth = std::max(depth, 1); continue; }
            auto row = storage.get_pointer<EvidenceRow>(j);
            if (!row) { compute_evidence(j); row = storage.get_pointer<EvidenceRow>(j); }
            if (!row) continue;
            for (auto& sid : decode_set(row->source_set)) sources.insert(sid);
            depth = std::max(depth, row->depth + 1);
        }
        EvidenceRow r;
        r.node_id = id;
        r.source_set = encode_set(sources);
        r.n_sources = static_cast<int>(sources.size());
        r.depth = depth;
        r.computed_at = util::now_iso();
        if (auto old = storage.get_pointer<EvidenceRow>(id); old && old->source_set == r.source_set && old->depth == r.depth)
            return false;
        storage.replace(r);
        return true;
    }

    /// After a new justification edge into `id`: recompute it and everything downstream.
    /// Dependents form a DAG, so a few passes over them reach a fixed point.
    void refresh_evidence_from(const std::string& id) {
        compute_evidence(id);
        const auto down = dependents(id);
        for (size_t pass = 0; pass < down.size() + 1; ++pass) {
            bool changed = false;
            for (const auto& d : down) changed = compute_evidence(d) || changed;
            if (!changed) break;
        }
    }

    Nutrition nutrition_of(const std::string& id) {
        using namespace sqlite_orm;
        const Node n = must_load(id);
        Nutrition out;
        out.id = id;
        std::vector<std::string> ids;
        if (n.type == NodeType::source) {
            ids = {id};
            out.depth = 0;
        } else {
            auto row = storage.get_pointer<EvidenceRow>(id);
            if (!row) { compute_evidence(id); row = storage.get_pointer<EvidenceRow>(id); }
            ids = decode_set(row->source_set);
            out.depth = row->depth;
            out.computed_at = row->computed_at;
        }
        out.sources = ids;
        out.n_sources = static_cast<int>(ids.size());
        if (ids.empty()) return out;

        std::set<std::string> attributions;
        std::map<SourceKind, int> kinds;
        double max_w = 0;
        for (const auto& r : storage.get_all<NodeRow>(where(in(&NodeRow::id, ids)))) {
            const json p = json::parse(r.payload, nullptr, false);
            const auto kind = source_kind_from(p.value("kind", "document")).value_or(SourceKind::document);
            const auto directness = directness_from(p.value("directness", "primary")).value_or(Directness::primary);
            if (directness == Directness::secondary && kind != SourceKind::llm_output) out.n_secondary++;
            const double w = weights.of(kind, directness);
            out.weighted += w;
            max_w = std::max(max_w, w);
            kinds[kind]++;
            const auto attr = p.value("attribution", "");
            if (attr.empty()) { out.n_unattributed++; attributions.insert("src:" + r.id); }
            else attributions.insert(attr);
            const auto at = p.value("observed_at", "");
            if (!at.empty()) {
                if (out.oldest_observed_at.empty() || at < out.oldest_observed_at) out.oldest_observed_at = at;
                if (out.newest_observed_at.empty() || at > out.newest_observed_at) out.newest_observed_at = at;
            }
        }
        out.attributions.assign(attributions.begin(), attributions.end());
        out.n_attributions = static_cast<int>(attributions.size());
        out.concentration = out.weighted > 0 ? max_w / out.weighted : 0;
        for (const auto& [k, c] : kinds) out.by_kind.emplace_back(k, c);
        out.dilution = out.n_sources > 0 ? static_cast<double>(out.depth) / out.n_sources : 0;
        if (out.computed_at.empty()) out.computed_at = util::now_iso();
        return out;
    }

    // ---- conflicts (Ф4) ----------------------------------------------------

    struct Compared {
        enum Kind { none, compatible, contradicts, refines } kind = none;
        std::optional<Scope> overlap;
        bool a_is_narrower = false;
    };

    static bool genuinely_narrower(const Scope& inner, const Scope& outer) {
        if (inner.valid_to && (!outer.valid_to || *inner.valid_to < *outer.valid_to)) return true;
        const auto& ic = inner.conditions, &oc = outer.conditions;
        if (ic.is_object())
            for (const auto& [k, v] : ic.items())
                if (!oc.is_object() || !oc.contains(k)) return true;
        return false;
    }

    /// Deterministic comparison of two claims. Text is never compared; a
    /// nested scope with incompatible values is a refinement candidate, not a
    /// contradiction (addendum 1, п. 1Б).
    static Compared compare_claims(const Node& a, const Node& b) {
        Compared r;
        const auto sa = a.payload.value("subject", ""), sb = b.payload.value("subject", "");
        const auto pa = a.payload.value("predicate", ""), pb = b.payload.value("predicate", "");
        if (sa.empty() || pa.empty() || sa != sb || pa != pb) return r;
        const Value va = Value::from_json(a.payload.value("value", json::object()));
        const Value vb = Value::from_json(b.payload.value("value", json::object()));
        const auto compat = Value::compatible(va, vb);
        if (!compat) return r;  // text, different types or units: not comparable
        const Scope xa = Scope::from_json(a.payload.value("scope", json::object()));
        const Scope xb = Scope::from_json(b.payload.value("scope", json::object()));
        auto overlap = Scope::intersect(xa, xb);
        if (!overlap) return r;  // never valid together: both may be right
        r.overlap = overlap;
        if (*compat) { r.kind = Compared::compatible; return r; }
        // A nested scope is a refinement only when the inner one is genuinely
        // narrower: bounded by valid_to where the outer is not (or earlier), or
        // by extra conditions. Two open-ended claims that differ only in when
        // they start are both in force now: that is a contradiction (or a
        // change over time that wants supersede), never a refinement.
        if (Scope::strictly_contains(xa, xb) && genuinely_narrower(xb, xa)) { r.kind = Compared::refines; r.a_is_narrower = false; return r; }
        if (Scope::strictly_contains(xb, xa) && genuinely_narrower(xa, xb)) { r.kind = Compared::refines; r.a_is_narrower = true; return r; }
        r.kind = Compared::contradicts;
        return r;
    }

    ConflictSide side_of(const Node& n) {
        ConflictSide s;
        s.id = n.id;
        s.text = n.payload.value("text", "");
        s.domain = n.domain;
        s.status = n.status;
        s.value = Value::from_json(n.payload.value("value", json::object()));
        s.scope = Scope::from_json(n.payload.value("scope", json::object()));
        s.credence = n.credence;
        s.n_dependents = static_cast<int>(dependents(n.id).size());
        if (auto ev = storage.get_pointer<EvidenceRow>(n.id)) s.n_sources = ev->n_sources;
        return s;
    }

    /// Claims still in force (active or stale) that take part in comparisons.
    std::vector<Node> live_claims(const std::string& domain) {
        using namespace sqlite_orm;
        const std::vector<std::string> alive{"active", "stale"};
        std::vector<Node> out;
        auto rows = domain.empty()
            ? storage.get_all<NodeRow>(where(c(&NodeRow::type) == "claim" && in(&NodeRow::status, alive)))
            : storage.get_all<NodeRow>(where(c(&NodeRow::type) == "claim" && in(&NodeRow::status, alive) && c(&NodeRow::domain) == domain));
        for (const auto& r : rows) {
            auto cr = storage.get_pointer<CredenceRow>(r.id);
            out.push_back(to_node(r, cr.get()));
        }
        return out;
    }

    bool edge_exists(const std::string& src, const std::string& dst, const char* type) {
        using namespace sqlite_orm;
        return storage.count<EdgeRow>(where(c(&EdgeRow::src) == src && c(&EdgeRow::dst) == dst && c(&EdgeRow::type) == type)) > 0;
    }

    std::vector<std::string> open_questions_about(const std::string& a, const std::string& b) {
        using namespace sqlite_orm;
        std::vector<std::string> out;
        for (const auto& r : storage.get_all<NodeRow>(where(c(&NodeRow::type) == "question" && c(&NodeRow::status) == "active"))) {
            const json p = json::parse(r.payload, nullptr, false);
            const auto about = p.value("about", json::array());
            bool has_a = false, has_b = false;
            for (const auto& x : about) { if (x == a) has_a = true; if (x == b) has_b = true; }
            if (has_a && has_b) out.push_back(r.id);
        }
        return out;
    }

    /// All conflicts among `pool` (and explicit edges touching it), unsorted.
    std::vector<ConflictEntry> conflicts_among(const std::vector<Node>& pool, std::vector<RefinementEntry>* refinements) {
        using namespace sqlite_orm;
        std::map<std::string, Node> by_id;
        for (const auto& n : pool) by_id.emplace(n.id, n);
        std::map<std::pair<std::string, std::string>, ConflictEntry> found;  // (a<b)
        auto entry = [&](const Node& x, const Node& y) -> ConflictEntry& {
            const Node& a = x.id < y.id ? x : y;
            const Node& b = x.id < y.id ? y : x;
            auto key = std::make_pair(a.id, b.id);
            auto it = found.find(key);
            if (it == found.end()) {
                ConflictEntry e;
                e.a = side_of(a);
                e.b = side_of(b);
                e.subject = a.payload.value("subject", "");
                e.predicate = a.payload.value("predicate", "");
                it = found.emplace(key, std::move(e)).first;
            }
            return it->second;
        };
        // group by (subject, predicate) and compare pairwise within a group
        std::map<std::pair<std::string, std::string>, std::vector<const Node*>> groups;
        for (const auto& n : pool) {
            const auto s = n.payload.value("subject", ""), p = n.payload.value("predicate", "");
            if (!s.empty() && !p.empty()) groups[{s, p}].push_back(&n);
        }
        for (auto& [key, members] : groups) {
            for (size_t i = 0; i < members.size(); ++i)
                for (size_t j = i + 1; j < members.size(); ++j) {
                    const auto cmp = compare_claims(*members[i], *members[j]);
                    if (cmp.kind == Compared::contradicts) {
                        auto& e = entry(*members[i], *members[j]);
                        e.detected = true;
                        e.overlap = cmp.overlap;
                    } else if (cmp.kind == Compared::refines && refinements) {
                        const Node& narrow = cmp.a_is_narrower ? *members[i] : *members[j];
                        const Node& wide = cmp.a_is_narrower ? *members[j] : *members[i];
                        if (edge_exists(narrow.id, wide.id, "refines") || edge_exists(wide.id, narrow.id, "refines")) continue;
                        if (edge_exists(std::min(narrow.id, wide.id), std::max(narrow.id, wide.id), "contradicts")) continue;  // somebody decided already
                        RefinementEntry r;
                        r.narrower = side_of(narrow);
                        r.wider = side_of(wide);
                        r.subject = key.first;
                        r.predicate = key.second;
                        refinements->push_back(std::move(r));
                    }
                }
        }
        // explicit edges touching the pool
        for (const auto& e : storage.get_all<EdgeRow>(where(c(&EdgeRow::type) == "contradicts"))) {
            auto ia = by_id.find(e.src), ib = by_id.find(e.dst);
            if (ia == by_id.end() && ib == by_id.end()) continue;
            const Node a = ia != by_id.end() ? ia->second : must_load(e.src);
            const Node b = ib != by_id.end() ? ib->second : must_load(e.dst);
            const auto dead = [](const Node& n) { return n.status == Status::retracted || n.status == Status::superseded; };
            if (dead(a) || dead(b)) continue;
            auto& en = entry(a, b);
            en.explicit_edge = true;
            if (en.subject.empty()) en.subject = a.payload.value("subject", "");
            if (en.predicate.empty()) en.predicate = a.payload.value("predicate", "");
        }
        std::vector<ConflictEntry> out;
        for (auto& [key, e] : found) {
            // ranking key: everything that stands on either side, counted once
            std::set<std::string> dep;
            for (const auto& d : dependents(e.a.id)) dep.insert(d);
            for (const auto& d : dependents(e.b.id)) dep.insert(d);
            e.n_dependents = static_cast<int>(dep.size());
            e.questions = open_questions_about(e.a.id, e.b.id);
            out.push_back(std::move(e));
        }
        return out;
    }

    static void rank(std::vector<ConflictEntry>& v) {
        std::sort(v.begin(), v.end(), [](const ConflictEntry& x, const ConflictEntry& y) {
            if (x.n_dependents != y.n_dependents) return x.n_dependents > y.n_dependents;
            if (x.a.id != y.a.id) return x.a.id < y.a.id;
            return x.b.id < y.b.id;
        });
    }

    QuestionEntry question_of(const Node& n) {
        using namespace sqlite_orm;
        QuestionEntry q;
        q.id = n.id;
        q.text = n.payload.value("text", "");
        q.why_open = n.payload.value("why_open", "");
        q.acceptance_criterion = n.payload.value("acceptance_criterion", "");
        q.domain = n.domain;
        q.status = n.status;
        q.created_at = n.created_at;
        for (const auto& x : n.payload.value("about", json::array())) if (x.is_string()) q.about.push_back(x.get<std::string>());
        for (const auto& p : n.payload.value("progress", json::array()))
            q.progress.push_back({p.value("at", ""), p.value("actor", ""), p.value("text", "")});
        q.resolution = n.payload.value("closed", json::object()).value("resolution", "");
        q.answers = storage.select(&EdgeRow::src, where(c(&EdgeRow::dst) == n.id && c(&EdgeRow::type) == "answers"), order_by(&EdgeRow::created_at));
        return q;
    }

    // ---- conflict triage (addendum 3) --------------------------------------

    static std::string pair_fingerprint(const ConflictSide& a, const ConflictSide& b) {
        // what the decision was based on: value, scope and status of both sides
        const json j = json::array({a.value.to_json(), a.scope.to_json(), to_string(a.status),
                                    b.value.to_json(), b.scope.to_json(), to_string(b.status)});
        return util::sha256_hex(j.dump());
    }

    static int days_between(const std::string& from_iso, const std::string& to_iso) {
        auto day = [](const std::string& s) -> long {
            int y = 0, m = 0, d = 0;
            if (std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) return 0;
            // days since epoch, civil-from-days (Howard Hinnant), good enough for differences
            y -= m <= 2;
            const long era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<long>(doe) - 719468;
        };
        return static_cast<int>(day(to_iso) - day(from_iso));
    }

    /// Reconciles the scan result with conflict_triage and fills entry.triage.
    /// Called inside a transaction by conflicts().
    void sync_triage(std::vector<ConflictEntry>& found) {
        using namespace sqlite_orm;
        const auto now = util::now_iso();
        std::set<std::pair<std::string, std::string>> seen;
        for (auto& e : found) {
            seen.insert({e.a.id, e.b.id});
            const auto fp = pair_fingerprint(e.a, e.b);
            auto row = storage.get_pointer<TriageRow>(e.a.id, e.b.id);
            TriageRow r;
            if (row) {
                r = *row;
                r.last_seen_at = now;
                r.times_seen++;
                const auto state = triage_state_from(r.state).value_or(TriageState::open);
                if (e.explicit_edge) {
                    r.state = "promoted";
                } else if (state == TriageState::vanished) {
                    r.state = "open";  // came back
                    r.fingerprint = fp;
                } else if (state == TriageState::dismissed || state == TriageState::resolved) {
                    if (r.fingerprint != fp) {
                        // the decision was about different content: it expires, the reason stays visible
                        r.state = "open";
                        r.expired_at = now;
                        r.fingerprint = fp;
                    }
                } else if (state == TriageState::open) {
                    r.fingerprint = fp;
                }
                storage.update(r);
            } else {
                r.lo_id = e.a.id;
                r.hi_id = e.b.id;
                r.first_seen_at = now;
                r.last_seen_at = now;
                r.times_seen = 1;
                r.state = e.explicit_edge ? "promoted" : "open";
                r.fingerprint = fp;
                storage.insert(r);
            }
            e.triage = to_triage(r, now);
        }
        // open pairs the scan no longer finds have settled on their own (a side superseded/retracted, scope narrowed...)
        for (auto r : storage.get_all<TriageRow>(where(c(&TriageRow::state) == "open"))) {
            if (seen.count({r.lo_id, r.hi_id})) continue;
            r.state = "vanished";
            r.last_seen_at = now;
            storage.update(r);
        }
    }

    static Triage to_triage(const TriageRow& r, const std::string& now) {
        Triage t;
        t.state = triage_state_from(r.state).value_or(TriageState::open);
        t.first_seen_at = r.first_seen_at;
        t.last_seen_at = r.last_seen_at;
        t.times_seen = r.times_seen;
        t.decided_by = r.decided_by;
        t.decided_at = r.decided_at;
        t.reason = r.reason;
        t.expired_at = r.expired_at;
        if (t.state == TriageState::open) t.open_days = days_between(r.first_seen_at, now);
        return t;
    }

    /// A human decision about a pair: it must currently be found by the detector.
    void decide_pair(const std::string& x, const std::string& y, TriageState state, const std::string& reason) {
        if (reason.empty()) throw Error(ErrorCode::bad_input, "a triage decision needs a reason");
        const std::string lo = std::min(x, y), hi = std::max(x, y);
        if (lo == hi) throw Error(ErrorCode::bad_input, "a pair needs two different claims");
        const Node a = must_load(lo), b = must_load(hi);
        if (a.type != NodeType::claim || b.type != NodeType::claim) throw Error(ErrorCode::bad_input, "both sides must be claims");
        const auto cmp = compare_claims(a, b);
        const bool explicit_edge = edge_exists(lo, hi, "contradicts");
        if (explicit_edge)
            throw Error(ErrorCode::bad_input, lo + " and " + hi + " are linked contradicts explicitly; that is a decision already, retract or supersede a side to settle it");
        if (cmp.kind != Compared::contradicts)
            throw Error(ErrorCode::not_a_conflict, "the detector does not find " + lo + " and " + hi + " in conflict; nothing to decide");
        const auto now = util::now_iso();
        auto row = storage.get_pointer<TriageRow>(lo, hi);
        TriageRow r;
        if (row) r = *row;
        else { r.lo_id = lo; r.hi_id = hi; r.first_seen_at = now; r.times_seen = 0; }
        r.last_seen_at = now;
        r.state = std::string(to_string(state));
        r.fingerprint = pair_fingerprint(side_of(a), side_of(b));
        r.decided_by = actor;
        r.decided_at = now;
        r.reason = reason;
        r.expired_at.clear();
        if (row) storage.update(r); else storage.insert(r);
    }

    /// A contradicts edge is the explicit decision: the pair is promoted.
    void promote_pair(const std::string& lo, const std::string& hi) {
        const auto now = util::now_iso();
        auto row = storage.get_pointer<TriageRow>(lo, hi);
        TriageRow r;
        if (row) r = *row;
        else { r.lo_id = lo; r.hi_id = hi; r.first_seen_at = now; r.times_seen = 0; }
        r.last_seen_at = now;
        r.state = "promoted";
        r.decided_by = actor;
        r.decided_at = now;
        if (row) storage.update(r); else storage.insert(r);
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
        if (const auto note = notice_of(n); !note.empty()) w.notice = note;
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
                if (auto r = load(*rule_id)) child.rule = label_of(*r) + (r->status == Status::active ? "" : " (" + std::string(to_string(r->status)) + ")");
            }
            w.children.push_back(std::move(child));
        };
        for (const auto& e : supports) visit(e.src, EdgeType::supports, std::nullopt);
        for (const auto& e : bases) visit(e.dst, edge_type_from(e.type).value_or(EdgeType::derives_from), e.rule_id);
        path.pop_back();
        return w;
    }
};

/// One transaction per public call; a public call made from inside another
/// (supersede -> assert_claim -> link) joins the outer one instead of nesting.
struct Txn {
    Storage& storage;
    bool& in_txn;
    bool owner;
    template <class ImplT>
    explicit Txn(ImplT& impl) : storage(impl.storage), in_txn(impl.in_txn), owner(!impl.in_txn) {
        if (owner) { storage.begin_transaction(); in_txn = true; }
    }
    void commit() {
        if (owner) { storage.commit(); in_txn = false; owner = false; }
    }
    ~Txn() {
        if (owner) { try { storage.rollback(); } catch (...) {} in_txn = false; }
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

    // A source is evidence from the world, never a wrapper around a node of this
    // store: why() would stop at a fake source instead of continuing into the
    // other domain (addendum 2, case Б). Cross-domain justification is an edge.
    Txn guard(*impl_);
    for (const auto& ref : util::node_ids_in(in.uri + "\n" + in.content))
        if (impl_->storage.get_pointer<NodeRow>(ref))
            throw Error(ErrorCode::bridge_source, "source refers to node " + ref + " of this store; a conclusion is not evidence, link to it with derives_from/supports instead");
    const auto hash = util::sha256_hex(in.content);
    auto existing = impl_->storage.select(
        &NodeRow::id, where(c(&NodeRow::type) == "source" && json_extract<std::string>(&NodeRow::payload, "$.content_hash") == hash));
    if (!existing.empty()) { guard.commit(); return existing.front(); }

    json payload{{"kind", to_string(in.kind)},
                 {"directness", to_string(in.directness)},
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
    Txn guard(*impl_);
    if (impl_->rule_by_name(in.name)) throw Error(ErrorCode::duplicate, "rule already defined: " + in.name);
    const json payload{{"name", in.name}, {"description", in.description}, {"n_applied", 0}, {"n_correct", 0}, {"brier", nullptr}};
    auto id = impl_->insert_node(NodeType::rule, in.domain, Sensitivity::public_, payload, "define_rule");
    guard.commit();
    return id;
}

void Store::define_predicate(const PredicateInput& in) {
    if (in.name.empty()) throw Error(ErrorCode::bad_input, "predicate.name is required");
    if (in.value_type == ValueType::enumeration && in.enum_values.empty())
        throw Error(ErrorCode::bad_input, "an enum predicate needs its member set");
    Txn guard(*impl_);
    if (impl_->load_predicate(in.name)) throw Error(ErrorCode::duplicate, "predicate already defined: " + in.name);
    PredicateRow r;
    r.name = in.name;
    r.value_type = std::string(to_string(in.value_type));
    r.unit = in.value_type == ValueType::number ? normalize_unit(in.unit).unit : "";
    r.enum_values = json(in.enum_values).dump();
    r.note = in.note;
    r.created_at = util::now_iso();
    impl_->storage.replace(r);
    const json after{{"name", r.name}, {"value_type", r.value_type}, {"unit", r.unit}, {"enum_values", in.enum_values}};
    impl_->record_revision("predicate:" + r.name, "define_predicate", nullptr, &after);
    guard.commit();
}

std::string Store::assert_claim(const ClaimInput& in) {
    if (in.text.empty()) throw Error(ErrorCode::bad_input, "claim.text is required");
    if (in.domain.empty()) throw Error(ErrorCode::bad_input, "domain is required");
    if (in.support.empty()) throw Error(ErrorCode::no_support, "a claim needs at least one supporting source or claim");
    if (!is_credence_step(in.credence)) throw Error(ErrorCode::bad_credence, credence_step_error(in.credence));
    switch (in.method) {
        case CredenceMethod::stated: case CredenceMethod::measured: case CredenceMethod::llm_judgment: case CredenceMethod::manual: break;
        default:
            throw Error(ErrorCode::bad_credence, std::string("method ") + std::string(to_string(in.method)) +
                                                     " is a derivation method; assert accepts stated|measured|llm_judgment|manual");
    }

    Txn guard(*impl_);
    std::set<std::string> uniq(in.support.begin(), in.support.end());
    Scope scope = in.scope;
    const bool infer_from = !in.scope.valid_from;
    for (const auto& sid : uniq) {
        const Node sup = impl_->check_support(sid, false);
        // a claim read off evidence holds from when the earliest evidence was produced
        if (infer_from && sup.type == NodeType::source && sup.payload.contains("observed_at")) {
            const auto at = sup.payload["observed_at"].get<std::string>();
            if (!at.empty() && (!scope.valid_from || at < *scope.valid_from)) scope.valid_from = at;
        }
    }
    if (scope.empty()) throw Error(ErrorCode::empty_scope, "a claim needs a scope: validity interval, conditions or a note");
    impl_->check_predicate(in.predicate, in.value);

    json payload{{"text", in.text}, {"subject", in.subject}, {"predicate", in.predicate}, {"value", in.value.to_json()}, {"scope", scope.to_json()}};
    if (infer_from && scope.valid_from) payload["scope_valid_from_inferred"] = true;  // taken from the source date, not stated
    auto id = impl_->insert_node(NodeType::claim, in.domain, in.sensitivity, payload, "assert");
    impl_->insert_credence(id, in.credence, in.method, json{{"support", json(uniq)}});
    for (const auto& sid : uniq) impl_->insert_edge(sid, id, EdgeType::supports, std::nullopt);
    impl_->compute_evidence(id);
    impl_->inherit_staleness(id, uniq);
    guard.commit();
    return id;
}

std::string Store::derive(const DeriveInput& in) {
    if (in.text.empty()) throw Error(ErrorCode::bad_input, "claim.text is required");
    if (in.domain.empty()) throw Error(ErrorCode::bad_input, "domain is required");
    if (in.from.empty()) throw Error(ErrorCode::no_support, "derive needs at least one claim in `from`");
    switch (in.method) {
        case CredenceMethod::min: case CredenceMethod::rule: case CredenceMethod::noisy_or:
        case CredenceMethod::llm_judgment: case CredenceMethod::manual: break;
        default:
            throw Error(ErrorCode::bad_credence, std::string("method ") + std::string(to_string(in.method)) +
                                                     " is an observation method; derive accepts min|rule|noisy_or|llm_judgment|manual");
    }

    Txn guard(*impl_);
    const Node rule = impl_->resolve_rule(in.rule);
    std::set<std::string> uniq(in.from.begin(), in.from.end());
    json inputs_from = json::array();
    double min_value = 1.0;
    std::optional<Scope> premises;  // intersection of the premises' scopes
    for (const auto& fid : uniq) {
        const Node base = impl_->check_support(fid, true);
        if (!base.credence) throw Error(ErrorCode::invalid_support, fid + " has no credence");
        min_value = std::min(min_value, base.credence->value);
        inputs_from.push_back({{"id", fid}, {"credence", base.credence->value}, {"stale", base.credence->stale}});
        const Scope sc = Scope::from_json(base.payload.value("scope", json::object()));
        if (!premises) {
            premises = sc;
        } else {
            auto inter = Scope::intersect(*premises, sc);
            if (!inter)
                throw Error(ErrorCode::empty_intersection, "premises are never valid together: " + fid + " does not overlap the others");
            premises = *inter;
        }
    }
    // A conclusion cannot hold in more situations than its premises: the caller
    // may narrow the intersection, never widen it. Unset axes inherit it.
    Scope scope = *premises;
    scope.note = in.scope.note;
    if (in.scope.valid_from) {
        if (premises->valid_from && *in.scope.valid_from < *premises->valid_from)
            throw Error(ErrorCode::scope_widens, "valid_from " + *in.scope.valid_from + " is earlier than the premises allow (" + *premises->valid_from + ")");
        scope.valid_from = in.scope.valid_from;
    }
    if (in.scope.valid_to) {
        if (premises->valid_to && *in.scope.valid_to > *premises->valid_to)
            throw Error(ErrorCode::scope_widens, "valid_to " + *in.scope.valid_to + " is later than the premises allow (" + *premises->valid_to + ")");
        scope.valid_to = in.scope.valid_to;
    }
    if (scope.valid_from && scope.valid_to && *scope.valid_from > *scope.valid_to)
        throw Error(ErrorCode::empty_intersection, "requested interval is empty");
    if (in.scope.conditions.is_object()) {
        for (const auto& [k, v] : in.scope.conditions.items()) {
            if (scope.conditions.contains(k) && scope.conditions[k] != v)
                throw Error(ErrorCode::scope_widens, "condition " + k + " contradicts the premises' scope");
            scope.conditions[k] = v;
        }
    }
    impl_->check_predicate(in.predicate, in.value);
    double value;
    if (in.credence) {
        if (!is_credence_step(*in.credence)) throw Error(ErrorCode::bad_credence, credence_step_error(*in.credence));
        value = *in.credence;
    } else {
        if (in.method != CredenceMethod::min)
            throw Error(ErrorCode::bad_credence, "credence is required unless method is min");
        value = min_value;
    }

    json payload{{"text", in.text}, {"subject", in.subject}, {"predicate", in.predicate}, {"value", in.value.to_json()}, {"scope", scope.to_json()}};
    auto id = impl_->insert_node(NodeType::claim, in.domain, in.sensitivity, payload, "derive");
    impl_->insert_credence(id, value, in.method, json{{"from", inputs_from}, {"rule", rule.id}, {"min", min_value}});
    for (const auto& fid : uniq) impl_->insert_edge(id, fid, EdgeType::derives_from, rule.id);
    impl_->compute_evidence(id);
    impl_->inherit_staleness(id, uniq);

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

void Store::link(const std::string& src_in, const std::string& dst_in, EdgeType type, std::optional<std::string> rule, const std::string& reason) {
    // contradicts is symmetric: stored once as (min(id), max(id)) so the primary
    // key rejects the mirrored duplicate. src/dst carry no direction for it.
    const bool canonical = type == EdgeType::contradicts && dst_in < src_in;
    const std::string& src = canonical ? dst_in : src_in;
    const std::string& dst = canonical ? src_in : dst_in;
    Txn guard(*impl_);
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
            impl_->check_replace(a, b);
            break;
    }
    impl_->insert_edge(src, dst, type, rule_id);
    switch (type) {
        case EdgeType::supports: impl_->refresh_evidence_from(dst); break;
        case EdgeType::derives_from: case EdgeType::assumes: impl_->refresh_evidence_from(src); break;
        case EdgeType::replaces: impl_->after_replace(src, dst, reason); break;
        case EdgeType::contradicts: impl_->promote_pair(src, dst); break;
        default: break;
    }
    guard.commit();
}

// ---- change (Ф3) -----------------------------------------------------------------

void Store::retract(const std::string& id, const std::string& reason) {
    using namespace sqlite_orm;
    if (reason.empty()) throw Error(ErrorCode::bad_input, "retract needs a reason");
    Txn guard(*impl_);
    const Node n = impl_->must_load(id);
    if (n.status == Status::retracted) throw Error(ErrorCode::bad_input, id + " is already retracted");
    if (n.status == Status::superseded) throw Error(ErrorCode::bad_input, id + " is superseded; retract its replacement instead");
    const auto at = util::now_iso();
    NodeRow row = impl_->storage.get<NodeRow>(id);
    impl_->set_status(row, Status::retracted, "retracted: " + reason, json{{"retracted", json{{"reason", reason}, {"at", at}}}});
    if (n.type == NodeType::rule) {
        // every application of the rule, and everything built on those
        auto applied = impl_->storage.select(&EdgeRow::src, where(c(&EdgeRow::rule_id) == id && c(&EdgeRow::type) == "derives_from"));
        std::set<std::string> uniq(applied.begin(), applied.end());
        for (const auto& a : uniq) {
            impl_->mark_stale({a}, id, "rule retracted: " + reason, at);
            impl_->mark_stale(impl_->dependents(a), id, "rule retracted: " + reason, at);
        }
    } else {
        impl_->knock_out(id, "retracted: " + reason, at);
    }
    guard.commit();
}

std::string Store::supersede(const std::string& old_id, const SourceInput& corrected, const std::string& reason) {
    Txn guard(*impl_);
    const Node old = impl_->must_load(old_id);
    if (old.type != NodeType::source) throw Error(ErrorCode::bad_input, old_id + " is not a source");
    const auto new_id = observe(corrected);
    if (new_id == old_id) throw Error(ErrorCode::duplicate, "the corrected content is the same as " + old_id);
    link(new_id, old_id, EdgeType::replaces, std::nullopt, reason);
    guard.commit();
    return new_id;
}

std::string Store::supersede(const std::string& old_id, const ClaimInput& corrected, const std::string& reason) {
    Txn guard(*impl_);
    const Node old = impl_->must_load(old_id);
    if (old.type != NodeType::claim) throw Error(ErrorCode::bad_input, old_id + " is not a claim");
    const auto new_id = assert_claim(corrected);
    link(new_id, old_id, EdgeType::replaces, std::nullopt, reason);
    guard.commit();
    return new_id;
}

std::string Store::supersede(const std::string& old_id, const DeriveInput& corrected, const std::string& reason) {
    Txn guard(*impl_);
    const Node old = impl_->must_load(old_id);
    if (old.type != NodeType::claim) throw Error(ErrorCode::bad_input, old_id + " is not a claim");
    const auto new_id = derive(corrected);
    link(new_id, old_id, EdgeType::replaces, std::nullopt, reason);
    guard.commit();
    return new_id;
}

void Store::recompute(const std::string& id, std::optional<double> credence) {
    using namespace sqlite_orm;
    Txn guard(*impl_);
    const Node n = impl_->must_load(id);
    if (n.type != NodeType::claim || !n.credence) throw Error(ErrorCode::bad_input, id + " has no credence to recompute");
    if (n.status == Status::retracted || n.status == Status::superseded)
        throw Error(ErrorCode::bad_input, id + " is " + std::string(to_string(n.status)) + "; nothing to recompute");

    // 1. follow replaces chains: an edge to a superseded justifier moves to its active replacement
    std::vector<std::string> live;
    json followed = json::array();
    const std::vector<std::string> fwd_types{"derives_from", "assumes"};
    auto in_edges = impl_->storage.get_all<EdgeRow>(where(c(&EdgeRow::dst) == id && c(&EdgeRow::type) == "supports"));
    auto out_edges = impl_->storage.get_all<EdgeRow>(where(c(&EdgeRow::src) == id && in(&EdgeRow::type, fwd_types)));
    auto consider = [&](EdgeRow e, bool incoming) {
        const std::string jid = incoming ? e.src : e.dst;
        NodeRow jr = impl_->storage.get<NodeRow>(jid);
        if (jr.status == "retracted") return;  // the edge stays as history, the justifier is gone
        if (jr.status == "superseded") {
            auto head = impl_->active_replacement(jid);
            if (!head) return;  // the chain ends in a retracted node
            impl_->storage.remove_all<EdgeRow>(where(c(&EdgeRow::src) == e.src && c(&EdgeRow::dst) == e.dst && c(&EdgeRow::type) == e.type));
            if (incoming) e.src = *head; else e.dst = *head;
            const auto dup = impl_->storage.count<EdgeRow>(where(c(&EdgeRow::src) == e.src && c(&EdgeRow::dst) == e.dst && c(&EdgeRow::type) == e.type));
            if (dup == 0) {
                if (incoming) impl_->check_acyclic(id, *head); else impl_->check_acyclic(id, *head);
                e.created_at = util::now_iso();
                impl_->storage.insert(e);
            }
            followed.push_back({{"from", jid}, {"to", *head}, {"via", e.type}});
            live.push_back(*head);
            return;
        }
        live.push_back(jid);
    };
    for (const auto& e : in_edges) consider(e, true);
    for (const auto& e : out_edges) consider(e, false);
    if (live.empty())
        throw Error(ErrorCode::no_support, id + " has no live justification left; retract it or add support before recomputing");

    // 2. premises must be current themselves: recompute bottom-up (stale() lists shallow nodes first)
    double min_value = 1.0;
    int n_claims = 0;
    json inputs_from = json::array();
    for (const auto& jid : live) {
        auto j = impl_->must_load(jid);
        if (!j.credence) continue;
        if (j.credence->stale) throw Error(ErrorCode::stale_premise, jid + " is itself stale; recompute it first");
        min_value = std::min(min_value, j.credence->value);
        ++n_claims;
        inputs_from.push_back({{"id", jid}, {"credence", j.credence->value}});
    }

    // 3. the value: min computes it, every other method needs a fresh judgement
    CredenceRow cr = impl_->storage.get<CredenceRow>(id);
    const auto method = credence_method_from(cr.method).value_or(CredenceMethod::manual);
    double value;
    if (method == CredenceMethod::min) {
        if (credence) throw Error(ErrorCode::bad_input, "method min computes its own value; do not pass credence");
        if (n_claims == 0) throw Error(ErrorCode::no_support, id + " has method min but no live claim premise left");
        value = min_value;
    } else {
        if (!credence)
            throw Error(ErrorCode::bad_credence, id + " has method " + std::string(to_string(method)) +
                                                     "; recompute needs your new credence, the store cannot judge it");
        if (!is_credence_step(*credence)) throw Error(ErrorCode::bad_credence, credence_step_error(*credence));
        value = *credence;
    }

    const json before{{"credence", cr.value}, {"stale", cr.stale != 0},
                      {"causes", [&] { json a = json::array(); for (const auto& m : impl_->storage.get_all<StaleRow>(where(c(&StaleRow::node_id) == id))) a.push_back(m.cause); return a; }()}};
    json inputs = json::parse(cr.inputs, nullptr, false);
    if (!inputs.is_object()) inputs = json::object();
    inputs["recomputed_from"] = live;
    if (method == CredenceMethod::min) { inputs["from"] = inputs_from; inputs["min"] = min_value; }
    if (!followed.empty()) inputs["followed"] = followed;
    const double old_value = cr.value;
    cr.value = value;
    cr.inputs = inputs.dump();
    cr.computed_at = util::now_iso();
    cr.stale = 0;
    impl_->storage.update(cr);
    impl_->storage.remove_all<StaleRow>(where(c(&StaleRow::node_id) == id));
    NodeRow row = impl_->storage.get<NodeRow>(id);
    const json after{{"credence", value}, {"stale", false}, {"followed", followed}};
    if (row.status == "stale") impl_->set_status(row, Status::active, "recomputed");
    impl_->record_revision(id, "recompute", &before, &after);
    impl_->refresh_evidence_from(id);
    // a changed value is itself a change for everything above
    if (value != old_value) {
        std::ostringstream os;
        os << "recomputed " << old_value << " -> " << value;
        impl_->mark_stale(impl_->dependents(id), id, os.str(), util::now_iso());
    }
    guard.commit();
}

// ---- questions (Ф4) --------------------------------------------------------------

std::string Store::ask(const QuestionInput& in) {
    if (in.text.empty()) throw Error(ErrorCode::bad_input, "question.text is required");
    if (in.acceptance_criterion.empty())
        throw Error(ErrorCode::bad_input, "question.acceptance_criterion is required: say what would close it");
    if (in.domain.empty()) throw Error(ErrorCode::bad_input, "domain is required");
    Txn guard(*impl_);
    for (const auto& id : in.about) {
        auto n = impl_->load(id);
        if (!n || n->type != NodeType::claim) throw Error(ErrorCode::not_found, "about: claim not found: " + id);
    }
    const json payload{{"text", in.text}, {"why_open", in.why_open}, {"acceptance_criterion", in.acceptance_criterion},
                       {"about", json(in.about)}, {"progress", json::array()}};
    auto id = impl_->insert_node(NodeType::question, in.domain, in.sensitivity, payload, "ask");
    guard.commit();
    return id;
}

void Store::note_progress(const std::string& question_id, const std::string& text) {
    if (text.empty()) throw Error(ErrorCode::bad_input, "progress note is empty");
    Txn guard(*impl_);
    NodeRow row = impl_->storage.get<NodeRow>(question_id);
    if (row.type != "question") throw Error(ErrorCode::bad_input, question_id + " is not a question");
    if (row.status != "active") throw Error(ErrorCode::bad_input, question_id + " is " + row.status + "; progress goes to open questions only");
    json before = json::parse(row.payload, nullptr, false);
    json after = before;
    after["progress"].push_back({{"at", util::now_iso()}, {"actor", impl_->actor}, {"text", text}});
    row.payload = after.dump();
    row.updated_at = util::now_iso();
    impl_->storage.update(row);
    impl_->record_revision(question_id, "progress", &before, &after);
    guard.commit();
}

void Store::close_question(const std::string& question_id, const std::string& resolution) {
    if (resolution.empty()) throw Error(ErrorCode::bad_input, "closing a question needs a resolution: what answered it, or why it is moot");
    Txn guard(*impl_);
    NodeRow row = impl_->storage.get<NodeRow>(question_id);
    if (row.type != "question") throw Error(ErrorCode::bad_input, question_id + " is not a question");
    if (row.status != "active") throw Error(ErrorCode::bad_input, question_id + " is already " + row.status);
    impl_->set_status(row, Status::closed, "closed: " + resolution,
                      json{{"closed", json{{"at", util::now_iso()}, {"actor", impl_->actor}, {"resolution", resolution}}}});
    guard.commit();
}

std::vector<QuestionEntry> Store::questions(const std::string& domain, bool include_closed) const {
    using namespace sqlite_orm;
    std::vector<QuestionEntry> out;
    for (const auto& r : impl_->storage.get_all<NodeRow>(where(c(&NodeRow::type) == "question"), order_by(&NodeRow::created_at))) {
        if (!domain.empty() && r.domain != domain) continue;
        if (!include_closed && r.status != "active") continue;
        out.push_back(impl_->question_of(Impl::to_node(r, nullptr)));
    }
    return out;
}

QuestionEntry Store::question(const std::string& id) const {
    const Node n = impl_->must_load(id);
    if (n.type != NodeType::question) throw Error(ErrorCode::bad_input, id + " is not a question");
    return impl_->question_of(n);
}

// ---- conflicts (Ф4) --------------------------------------------------------------

std::vector<ConflictEntry> Store::conflicts(const std::string& domain, bool include_decided) {
    // compare across the whole store: the same fact about the world may be
    // claimed in two domains; then keep the pairs with a side in `domain`
    Txn guard(*impl_);
    auto out = impl_->conflicts_among(impl_->live_claims(""), nullptr);
    impl_->sync_triage(out);
    guard.commit();
    out.erase(std::remove_if(out.begin(), out.end(), [&](const ConflictEntry& e) {
                  if (!domain.empty() && e.a.domain != domain && e.b.domain != domain) return true;
                  if (include_decided) return false;
                  return e.triage.state != TriageState::open && e.triage.state != TriageState::promoted;
              }), out.end());
    Impl::rank(out);
    return out;
}

std::vector<ConflictEntry> Store::conflicts_of(const std::string& id, bool include_decided) {
    const Node n = impl_->must_load(id);
    if (n.type != NodeType::claim) return {};
    auto out = conflicts("", include_decided);
    out.erase(std::remove_if(out.begin(), out.end(), [&](const ConflictEntry& e) { return e.a.id != id && e.b.id != id; }), out.end());
    return out;
}

void Store::dismiss_conflict(const std::string& a, const std::string& b, const std::string& reason) {
    Txn guard(*impl_);
    impl_->decide_pair(a, b, TriageState::dismissed, reason);
    guard.commit();
}

void Store::resolve_conflict(const std::string& a, const std::string& b, const std::string& reason) {
    Txn guard(*impl_);
    impl_->decide_pair(a, b, TriageState::resolved, reason);
    guard.commit();
}

TriageStats Store::triage_stats() const {
    TriageStats s;
    for (const auto& r : impl_->storage.get_all<TriageRow>()) {
        switch (triage_state_from(r.state).value_or(TriageState::open)) {
            case TriageState::open: s.open++; break;
            case TriageState::dismissed: s.dismissed++; break;
            case TriageState::resolved: s.resolved++; break;
            case TriageState::vanished: s.vanished++; break;
            case TriageState::promoted: s.promoted++; break;
        }
    }
    const int decided = s.promoted + s.resolved + s.dismissed;
    if (decided > 0) s.precision = static_cast<double>(s.promoted + s.resolved) / decided;
    return s;
}

std::vector<RefinementEntry> Store::refinements(const std::string& domain) const {
    std::vector<RefinementEntry> refs;
    impl_->conflicts_among(impl_->live_claims(""), &refs);
    if (!domain.empty()) {
        refs.erase(std::remove_if(refs.begin(), refs.end(), [&](const RefinementEntry& e) { return e.narrower.domain != domain && e.wider.domain != domain; }), refs.end());
    }
    std::sort(refs.begin(), refs.end(), [](const RefinementEntry& x, const RefinementEntry& y) {
        return x.narrower.id != y.narrower.id ? x.narrower.id < y.narrower.id : x.wider.id < y.wider.id;
    });
    return refs;
}

// ---- read ----------------------------------------------------------------------

std::optional<Node> Store::get(const std::string& id) const { return impl_->load(id); }
std::optional<Node> Store::find_rule(const std::string& name) const { return impl_->rule_by_name(name); }
std::optional<Predicate> Store::get_predicate(const std::string& name) const { return impl_->load_predicate(name); }

std::vector<Predicate> Store::list_predicates() const {
    using namespace sqlite_orm;
    std::vector<Predicate> out;
    for (const auto& r : impl_->storage.get_all<PredicateRow>(order_by(&PredicateRow::name))) out.push_back(Impl::to_predicate(r));
    return out;
}

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

std::vector<std::string> Store::dependents_of(const std::string& id) const {
    impl_->must_load(id);
    return impl_->dependents(id);
}

Nutrition Store::nutrition(const std::string& id) const { return impl_->nutrition_of(id); }
std::vector<StaleEntry> Store::stale(const std::string& domain) const {
    using namespace sqlite_orm;
    std::vector<StaleEntry> out;
    for (const auto& nid : impl_->storage.select(&CredenceRow::node_id, where(c(&CredenceRow::stale) == 1))) {
        auto n = impl_->load(nid);
        if (!n || (!domain.empty() && n->domain != domain)) continue;
        StaleEntry e;
        e.id = nid;
        e.label = Impl::label_of(*n);
        e.domain = n->domain;
        e.credence = n->credence;
        if (auto ev = impl_->storage.get_pointer<EvidenceRow>(nid)) e.depth = ev->depth;
        for (const auto& m : impl_->storage.get_all<StaleRow>(where(c(&StaleRow::node_id) == nid), order_by(&StaleRow::at)))
            e.causes.push_back({m.cause, m.reason, m.at});
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const StaleEntry& a, const StaleEntry& b) {
        return a.depth != b.depth ? a.depth < b.depth : a.id < b.id;
    });
    return out;
}

std::vector<WhatIfEntry> Store::what_if(const std::string& id, double new_value) const {
    const Node n = impl_->must_load(id);
    if (n.type != NodeType::claim || !n.credence) throw Error(ErrorCode::bad_input, id + " has no credence");
    if (!is_credence_step(new_value)) throw Error(ErrorCode::bad_credence, credence_step_error(new_value));

    // projected[x] = the value x would take; absent = x would merely go stale.
    // Dependents form a DAG, so passes to a fixed point settle it.
    std::map<std::string, std::optional<double>> projected{{id, new_value}};
    const auto down = impl_->dependents(id);
    std::map<std::string, Node> nodes;
    for (const auto& d : down) nodes.emplace(d, impl_->must_load(d));
    for (size_t pass = 0; pass < down.size() + 1; ++pass) {
        bool changed = false;
        for (const auto& d : down) {
            const Node& dn = nodes.at(d);
            std::optional<double> val;
            if (dn.credence && dn.credence->method == CredenceMethod::min) {
                double m = 1.0;
                bool ok = true, any = false;
                for (const auto& j : impl_->live_justifiers(d)) {
                    auto it = projected.find(j);
                    if (it != projected.end()) {
                        if (!it->second) { ok = false; break; }
                        m = std::min(m, *it->second);
                        any = true;
                        continue;
                    }
                    auto jn = nodes.count(j) ? nodes.at(j) : impl_->must_load(j);
                    if (jn.credence) { m = std::min(m, jn.credence->value); any = true; }
                }
                if (ok && any) val = m;
            }
            auto it = projected.find(d);
            if (it == projected.end() || it->second != val) { projected[d] = val; changed = true; }
        }
        if (!changed) break;
    }
    std::vector<WhatIfEntry> out;
    for (const auto& d : down) {
        const Node& dn = nodes.at(d);
        WhatIfEntry e;
        e.id = d;
        e.label = Impl::label_of(dn);
        if (dn.credence) { e.method = dn.credence->method; e.current = dn.credence->value; }
        e.projected = projected[d];
        out.push_back(std::move(e));
    }
    return out;
}

void Store::set_weights(NutritionWeights w) { impl_->weights = w; }
NutritionWeights Store::weights() const { return impl_->weights; }

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
