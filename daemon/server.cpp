#include "server.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <httplib.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace beliefd {

using namespace beliefdb;
using nlohmann::json;

// ---- config --------------------------------------------------------------------

static std::string env_name_for(const std::string& actor) {
    std::string s = "BELIEFD_TOKEN_";
    for (unsigned char ch : actor) s += ch == '-' ? '_' : static_cast<char>(std::toupper(ch));
    return s;
}

Config Config::from_json(const json& j, const Getenv& getenv_fn) {
    Config c;
    c.db = j.value("db", c.db);
    c.listen = j.value("listen", c.listen);
    c.port = j.value("port", c.port);
    if (j.contains("policy") && j["policy"].is_object()) {
        const auto& p = j["policy"];
        c.policy.batch_claims = p.value("batch_claims", c.policy.batch_claims);
        c.policy.batch_hypotheses = p.value("batch_hypotheses", c.policy.batch_hypotheses);
        c.policy.daily_claims = p.value("daily_claims", c.policy.daily_claims);
        c.policy.daily_hypotheses = p.value("daily_hypotheses", c.policy.daily_hypotheses);
        c.policy.open_hypotheses = p.value("open_hypotheses", c.policy.open_hypotheses);
        c.policy.batch_idle_seconds = p.value("batch_idle_seconds", c.policy.batch_idle_seconds);
        c.policy.hypothesis_ttl_days = p.value("hypothesis_ttl_days", c.policy.hypothesis_ttl_days);
        c.policy.require_batch = p.value("require_batch", c.policy.require_batch);
    }
    if (j.contains("embeddings") && j["embeddings"].is_object()) {
        const auto& e = j["embeddings"];
        EmbeddingConfig ec;
        ec.base_url = e.value("base_url", "");
        ec.model = e.value("model", ec.model);
        ec.timeout_ms = e.value("timeout_ms", ec.timeout_ms);
        ec.backoff_seconds = e.value("backoff_seconds", ec.backoff_seconds);
        c.quiet_hours = e.value("quiet_hours", "");
        if (ec.base_url.empty()) c.warnings.push_back("embeddings.base_url is empty: full-text only");
        else c.embeddings = ec;
    }
    if (j.contains("actors") && j["actors"].is_array()) {
        for (const auto& a : j["actors"]) {
            ActorConfig ac;
            ac.name = a.value("name", "");
            if (ac.name.empty()) { c.warnings.push_back("actor without a name skipped"); continue; }
            if (a.contains("domains") && a["domains"].is_array())
                for (const auto& d : a["domains"]) if (d.is_string()) ac.domains.push_back(d.get<std::string>());
            ac.human = a.value("human", false);
            if (a.contains("token")) { c.warnings.push_back("actor " + ac.name + ": tokens do not belong in the file, use " + env_name_for(ac.name)); }
            ac.token = getenv_fn(env_name_for(ac.name));
            if (ac.token.empty()) { c.warnings.push_back("actor " + ac.name + " has no token (" + env_name_for(ac.name) + " unset): disabled"); continue; }
            c.actors.push_back(std::move(ac));
        }
    }
    return c;
}

Config Config::load(const std::string& path, const Getenv& getenv_fn) {
    std::ifstream f(path);
    if (!f) throw Error(ErrorCode::bad_input, "cannot read config " + path);
    json j = json::parse(f);
    return from_json(j, getenv_fn);
}

bool in_quiet_hours(const std::string& spec, int minutes_of_day) {
    int h1 = 0, m1 = 0, h2 = 0, m2 = 0;
    if (std::sscanf(spec.c_str(), "%d:%d-%d:%d", &h1, &m1, &h2, &m2) != 4) return false;
    const int from = h1 * 60 + m1, to = h2 * 60 + m2;
    if (from == to) return false;
    if (from < to) return minutes_of_day >= from && minutes_of_day < to;
    return minutes_of_day >= from || minutes_of_day < to;  // wraps midnight
}

static int local_minutes_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm.tm_hour * 60 + tm.tm_min;
}

// ---- request helpers -------------------------------------------------------------

struct HttpError {
    int status;
    std::string code;
    std::string message;
};

static int status_of(ErrorCode c) {
    switch (c) {
        case ErrorCode::not_found: return 404;
        case ErrorCode::quota_exceeded: return 429;
        case ErrorCode::no_batch: case ErrorCode::duplicate: case ErrorCode::duplicate_claim: case ErrorCode::already_replaced: return 409;
        default: return 400;
    }
}

static Value parse_value(const json& j) {
    if (j.is_null()) return Value::text("");
    if (j.is_string()) return Value::text(j.get<std::string>());
    if (j.is_boolean()) return Value::boolean(j.get<bool>());
    if (j.is_number()) return Value::point(j.get<double>());
    if (!j.is_object()) throw HttpError{400, "bad_input", "value must be an object, string, number or bool"};
    const auto type = value_type_from(j.value("type", "text"));
    if (!type) throw HttpError{400, "bad_input", "unknown value type: " + j.value("type", "")};
    switch (*type) {
        case ValueType::boolean: return Value::boolean(j.value("value", false));
        case ValueType::number: {
            const double lo = j.contains("lo") ? j["lo"].get<double>() : j.value("value", 0.0);
            const double hi = j.contains("hi") ? j["hi"].get<double>() : lo;
            return Value::number(lo, hi, j.value("unit", ""));
        }
        case ValueType::enumeration: return Value::enum_member(j.value("value", ""));
        case ValueType::ref: return Value::reference(j.value("value", ""));
        case ValueType::text: return Value::text(j.value("value", ""));
    }
    return Value::text("");
}

static std::vector<std::string> ids_of(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key)) return out;
    if (j[key].is_string()) { out.push_back(j[key].get<std::string>()); return out; }
    if (j[key].is_array()) for (const auto& x : j[key]) if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

template <class E>
static E enum_or(std::optional<E> v, const char* what, const std::string& text) {
    if (!v) throw HttpError{400, "bad_input", std::string("unknown ") + what + ": " + text};
    return *v;
}

static std::string need_str(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string() || j[key].get<std::string>().empty())
        throw HttpError{400, "bad_input", std::string(key) + " is required"};
    return j[key].get<std::string>();
}

static SourceInput parse_source(const json& j) {
    SourceInput in;
    in.kind = enum_or(source_kind_from(j.value("kind", "document")), "kind", j.value("kind", ""));
    in.directness = enum_or(directness_from(j.value("directness", "primary")), "directness", j.value("directness", ""));
    in.uri = j.value("uri", "");
    in.content = need_str(j, "content");
    in.observed_at = j.value("observed_at", "");
    in.attribution = j.value("attribution", "");
    if (j.contains("trust_hint") && j["trust_hint"].is_number()) in.trust_hint = j["trust_hint"].get<double>();
    in.domain = j.value("domain", "general");
    in.sensitivity = enum_or(sensitivity_from(j.value("sensitivity", "domain")), "sensitivity", j.value("sensitivity", ""));
    return in;
}

static ClaimInput parse_claim(const json& j) {
    ClaimInput in;
    in.text = need_str(j, "text");
    in.subject = j.value("subject", "");
    in.predicate = j.value("predicate", "");
    in.value = parse_value(j.value("value", json()));
    in.support = ids_of(j, "support");
    in.credence = j.value("credence", 0.5);
    in.method = enum_or(credence_method_from(j.value("method", "stated")), "method", j.value("method", ""));
    in.scope = Scope::from_json(j.value("scope", json::object()));
    in.domain = j.value("domain", "general");
    in.sensitivity = enum_or(sensitivity_from(j.value("sensitivity", "domain")), "sensitivity", j.value("sensitivity", ""));
    in.despite = ids_of(j, "despite");
    return in;
}

static DeriveInput parse_derive(const json& j) {
    DeriveInput in;
    in.text = need_str(j, "text");
    in.subject = j.value("subject", "");
    in.predicate = j.value("predicate", "");
    in.value = parse_value(j.value("value", json()));
    in.from = ids_of(j, "from");
    in.rule = need_str(j, "rule");
    if (j.contains("credence") && j["credence"].is_number()) in.credence = j["credence"].get<double>();
    in.method = enum_or(credence_method_from(j.value("method", "min")), "method", j.value("method", ""));
    in.scope = Scope::from_json(j.value("scope", json::object()));
    in.domain = j.value("domain", "general");
    in.sensitivity = enum_or(sensitivity_from(j.value("sensitivity", "domain")), "sensitivity", j.value("sensitivity", ""));
    in.despite = ids_of(j, "despite");
    return in;
}

static HypothesisInput parse_hypothesis(const json& j) {
    HypothesisInput in;
    in.text = need_str(j, "text");
    in.subject = j.value("subject", "");
    in.predicate = j.value("predicate", "");
    in.value = parse_value(j.value("value", json()));
    in.from = ids_of(j, "from");
    in.rule = need_str(j, "rule");
    in.scope = Scope::from_json(j.value("scope", json::object()));
    in.domain = j.value("domain", "general");
    in.sensitivity = enum_or(sensitivity_from(j.value("sensitivity", "domain")), "sensitivity", j.value("sensitivity", ""));
    in.despite = ids_of(j, "despite");
    return in;
}

static json node_json(const Node& n) {
    json j{{"id", n.id},         {"type", to_string(n.type)},   {"domain", n.domain},
           {"status", to_string(n.status)}, {"sensitivity", to_string(n.sensitivity)},
           {"created_at", n.created_at}, {"updated_at", n.updated_at}, {"payload", n.payload}};
    if (n.credence)
        j["credence"] = {{"value", n.credence->value}, {"method", to_string(n.credence->method)},
                         {"inputs", n.credence->inputs}, {"computed_at", n.credence->computed_at}, {"stale", n.credence->stale}};
    return j;
}

// ---- the server ------------------------------------------------------------------

struct Server::Impl {
    Config cfg;
    Store db;
    httplib::Server srv;
    std::thread thread;
    std::mutex mu;
    int port = 0;
    bool embeddings_on = false;

    explicit Impl(Config c) : cfg(std::move(c)), db(Store::open(cfg.db, "beliefd")) {
        db.set_policy(cfg.policy);
        routes();
    }

    // ---- actors and visibility ----

    const ActorConfig* authenticate(const httplib::Request& req) const {
        auto auth = req.get_header_value("Authorization");
        const std::string prefix = "Bearer ";
        if (auth.rfind(prefix, 0) != 0) return nullptr;
        const auto token = auth.substr(prefix.size());
        if (token.empty()) return nullptr;
        for (const auto& a : cfg.actors) if (a.token == token) return &a;
        return nullptr;
    }

    static bool owns(const ActorConfig& a, const std::string& domain) {
        if (a.human) return true;
        for (const auto& d : a.domains) if (d == domain) return true;
        return false;
    }

    static bool can_see(const ActorConfig& a, const Node& n) {
        if (a.human || n.sensitivity != Sensitivity::private_) return true;
        return owns(a, n.domain);
    }

    void require_own_domain(const ActorConfig& a, const std::string& domain) const {
        if (!owns(a, domain)) throw HttpError{403, "forbidden", "actor " + a.name + " may not write to domain " + domain};
    }

    Node visible_node(const ActorConfig& a, const std::string& id) {
        auto n = db.get(id);
        if (!n) throw Error(ErrorCode::not_found, "node not found: " + id);
        if (!can_see(a, *n)) throw HttpError{403, "forbidden", id + " is private to domain " + n->domain};
        return *n;
    }

    Node own_node(const ActorConfig& a, const std::string& id) {
        const Node n = visible_node(a, id);
        require_own_domain(a, n.domain);
        return n;
    }

    /// Prunes a why() tree: subtrees the actor may not see are replaced by a stub.
    void prune(const ActorConfig& a, WhyNode& w) {
        for (auto& ch : w.children) {
            auto n = db.get(ch.id);
            if (n && !can_see(a, *n)) {
                ch.label = "(private to " + n->domain + ")";
                ch.children.clear();
                ch.credence.reset();
                ch.truncated = true;
                continue;
            }
            prune(a, ch);
        }
    }

    void redact_side(const ActorConfig& a, ConflictSide& s) {
        auto n = db.get(s.id);
        if (n && !can_see(a, *n)) { s.text = "(private to " + n->domain + ")"; s.value = Value::text(""); s.scope = Scope{}; s.credence.reset(); }
    }

    template <class T>
    void keep_visible(const ActorConfig& a, std::vector<T>& v) {
        v.erase(std::remove_if(v.begin(), v.end(), [&](const T& e) { auto n = db.get(e.id); return n && !can_see(a, *n); }), v.end());
    }

    void apply_quiet_hours() {
        const bool quiet = !cfg.quiet_hours.empty() && in_quiet_hours(cfg.quiet_hours, local_minutes_now());
        const bool want = cfg.embeddings.has_value() && !quiet;
        if (want != embeddings_on) {
            db.set_embeddings(want ? cfg.embeddings : std::nullopt);
            embeddings_on = want;
        }
    }

    bool quiet_now() const { return !cfg.quiet_hours.empty() && in_quiet_hours(cfg.quiet_hours, local_minutes_now()); }

    // ---- plumbing ----

    static void reply(httplib::Response& res, int status, const json& body) {
        res.status = status;
        res.set_content(body.dump(), "application/json");
    }

    using Handler = std::function<json(const ActorConfig&, const httplib::Request&, const json& body)>;

    httplib::Server::Handler wrap(Handler fn) {
        return [this, fn = std::move(fn)](const httplib::Request& req, httplib::Response& res) {
            const ActorConfig* actor = authenticate(req);
            if (!actor) { reply(res, 401, {{"error", "unauthorized"}, {"message", "Authorization: Bearer <token> of a configured actor"}}); return; }
            std::lock_guard<std::mutex> lock(mu);
            try {
                apply_quiet_hours();
                db.set_actor(actor->name);
                Policy p = cfg.policy;
                p.require_batch = cfg.policy.require_batch && !actor->human;
                db.set_policy(p);
                // the batch header matters for writes only; reads and batch management ignore it
                db.leave_batch();
                if (req.method == "POST" && req.path.rfind("/batch/", 0) != 0)
                    if (const auto b = req.get_header_value("X-Belief-Batch"); !b.empty()) db.resume_batch(b);
                json body = json::object();
                if (!req.body.empty()) {
                    body = json::parse(req.body, nullptr, false);
                    if (!body.is_object()) throw HttpError{400, "bad_json", "the body must be a JSON object"};
                }
                reply(res, 200, fn(*actor, req, body));
            } catch (const HttpError& e) {
                reply(res, e.status, {{"error", e.code}, {"message", e.message}});
            } catch (const Error& e) {
                reply(res, status_of(e.code), {{"error", to_string(e.code)}, {"message", e.what()}});
            } catch (const json::exception& e) {
                reply(res, 400, {{"error", "bad_json"}, {"message", e.what()}});
            } catch (const std::exception& e) {
                reply(res, 500, {{"error", "internal"}, {"message", e.what()}});
            }
        };
    }

    static int int_param(const httplib::Request& req, const char* key, int def) {
        return req.has_param(key) ? std::atoi(req.get_param_value(key).c_str()) : def;
    }

    /// What the CLI prints after a write, as data: contradictions and neighbours of the new node.
    json after_write(const ActorConfig& a, const std::string& id, const std::string& text, const std::string& subject, const std::string& predicate, const Value& value) {
        json out{{"id", id}};
        auto cs = db.conflicts_of(id);
        auto conflicts = json::array();
        for (auto& c : cs) {
            auto& other = c.a.id == id ? c.b : c.a;
            redact_side(a, other);
            conflicts.push_back({{"id", other.id}, {"text", other.text}, {"value", other.value.describe()}});
        }
        out["conflicts"] = conflicts;
        SimilarQuery q;
        q.text = text; q.subject = subject; q.predicate = predicate; q.value = value; q.limit = 5;
        auto sim = db.similar(q);
        keep_visible(a, sim.entries);
        auto similar = json::array();
        for (const auto& e : sim.entries) if (e.id != id) similar.push_back(e.to_json());
        out["similar"] = similar;
        out["search_mode"] = sim.mode;
        return out;
    }

    void routes() {
        using Req = const httplib::Request&;
        // ---- health ----
        srv.Get("/health", wrap([this](const ActorConfig&, Req, const json&) {
            auto k = db.counts();
            return json{{"ok", true},
                        {"counts", {{"nodes", k.nodes}, {"edges", k.edges}, {"sources", k.sources}, {"claims", k.claims}, {"hypotheses", k.hypotheses}, {"rules", k.rules}, {"open_batches", k.open_batches}}},
                        {"embeddings", db.embedding_status().to_json()},
                        {"quiet_hours", cfg.quiet_hours},
                        {"quiet_now", quiet_now()},
                        {"triage", db.triage_stats().to_json()}};
        }));
        srv.Get("/whoami", wrap([](const ActorConfig& a, Req, const json&) {
            return json{{"actor", a.name}, {"domains", a.domains}, {"human", a.human}};
        }));

        // ---- reads ----
        srv.Get(R"(/node/([^/]+))", wrap([this](const ActorConfig& a, Req req, const json&) {
            const Node n = visible_node(a, req.matches[1]);
            json j = node_json(n);
            auto edges = json::array();
            for (const auto& e : db.edges_from(n.id)) edges.push_back({{"dir", "out"}, {"type", to_string(e.type)}, {"to", e.dst}, {"rule", e.rule_id ? json(*e.rule_id) : json()}});
            for (const auto& e : db.edges_to(n.id)) edges.push_back({{"dir", "in"}, {"type", to_string(e.type)}, {"from", e.src}, {"rule", e.rule_id ? json(*e.rule_id) : json()}});
            j["edges"] = edges;
            return j;
        }));
        srv.Get(R"(/why/([^/]+))", wrap([this](const ActorConfig& a, Req req, const json&) {
            const std::string id = req.matches[1];
            visible_node(a, id);
            auto w = db.why(id, int_param(req, "depth", -1));
            prune(a, w);
            json j = w.to_json();
            j["nutrition"] = db.nutrition(id).to_json();
            return j;
        }));
        srv.Get(R"(/nutrition/([^/]+))", wrap([this](const ActorConfig& a, Req req, const json&) {
            visible_node(a, req.matches[1]);
            return db.nutrition(req.matches[1]).to_json();
        }));
        srv.Get(R"(/what-if/([^/]+))", wrap([this](const ActorConfig& a, Req req, const json&) {
            visible_node(a, req.matches[1]);
            if (!req.has_param("credence")) throw HttpError{400, "bad_input", "credence is required"};
            auto r = db.what_if(req.matches[1], std::strtod(req.get_param_value("credence").c_str(), nullptr));
            keep_visible(a, r);
            auto arr = json::array();
            for (const auto& e : r) arr.push_back(e.to_json());
            return json{{"entries", arr}};
        }));
        srv.Get("/stale", wrap([this](const ActorConfig& a, Req req, const json&) {
            auto r = db.stale(req.get_param_value("domain"));
            keep_visible(a, r);
            auto arr = json::array();
            for (const auto& e : r) arr.push_back(e.to_json());
            return json{{"entries", arr}};
        }));
        srv.Get("/conflicts", wrap([this](const ActorConfig& a, Req req, const json&) {
            auto cs = db.conflicts(req.get_param_value("domain"), req.has_param("all"));
            const int older = int_param(req, "older_than", 0);
            auto arr = json::array();
            for (auto& c : cs) {
                if (c.triage.open_days < older) continue;
                redact_side(a, c.a);
                redact_side(a, c.b);
                arr.push_back(c.to_json());
            }
            return json{{"entries", arr}};
        }));
        srv.Get("/refinements", wrap([this](const ActorConfig& a, Req req, const json&) {
            auto rs = db.refinements(req.get_param_value("domain"));
            auto arr = json::array();
            for (auto& r : rs) { redact_side(a, r.narrower); redact_side(a, r.wider); arr.push_back(r.to_json()); }
            return json{{"entries", arr}};
        }));
        srv.Get("/questions", wrap([this](const ActorConfig& a, Req req, const json&) {
            auto qs = db.questions(req.get_param_value("domain"), req.has_param("all"));
            keep_visible(a, qs);
            auto arr = json::array();
            for (const auto& q : qs) arr.push_back(q.to_json());
            return json{{"entries", arr}};
        }));
        srv.Get(R"(/question/([^/]+))", wrap([this](const ActorConfig& a, Req req, const json&) {
            visible_node(a, req.matches[1]);
            return db.question(req.matches[1]).to_json();
        }));
        srv.Get("/hypotheses", wrap([this](const ActorConfig& a, Req req, const json&) {
            auto hs = db.hypotheses(req.get_param_value("domain"), req.has_param("all"));
            keep_visible(a, hs);
            auto arr = json::array();
            for (const auto& h : hs) arr.push_back(h.to_json());
            return json{{"entries", arr}};
        }));
        srv.Get(R"(/hypothesis/([^/]+))", wrap([this](const ActorConfig& a, Req req, const json&) {
            visible_node(a, req.matches[1]);
            return db.hypothesis(req.matches[1]).to_json();
        }));
        srv.Get("/search", wrap([this](const ActorConfig& a, Req req, const json&) {
            if (!req.has_param("q")) throw HttpError{400, "bad_input", "q is required"};
            auto r = db.search(req.get_param_value("q"), req.get_param_value("domain"), int_param(req, "limit", 20));
            keep_visible(a, r.entries);
            return r.to_json();
        }));
        srv.Post("/similar", wrap([this](const ActorConfig& a, Req, const json& body) {
            SimilarQuery q;
            q.text = body.value("text", "");
            q.subject = body.value("subject", "");
            q.predicate = body.value("predicate", "");
            if (body.contains("value") && !body["value"].is_null()) q.value = parse_value(body["value"]);
            if (body.contains("scope") && body["scope"].is_object()) q.scope = Scope::from_json(body["scope"]);
            q.domain = body.value("domain", "");
            q.limit = body.value("limit", 10);
            auto r = db.similar(q);
            keep_visible(a, r.entries);
            return r.to_json();
        }));
        srv.Get("/predicates", wrap([this](const ActorConfig&, Req, const json&) {
            auto arr = json::array();
            for (const auto& p : db.list_predicates())
                arr.push_back({{"name", p.name}, {"value_type", to_string(p.value_type)}, {"unit", p.unit}, {"enum_values", p.enum_values}, {"note", p.note}});
            return json{{"entries", arr}};
        }));
        srv.Get(R"(/rule/([^/]+))", wrap([this](const ActorConfig&, Req req, const json&) {
            auto r = db.find_rule(req.matches[1]);
            if (!r) throw Error(ErrorCode::not_found, "rule not found: " + std::string(req.matches[1]));
            return node_json(*r);
        }));
        srv.Get("/batches", wrap([this](const ActorConfig& a, Req req, const json&) {
            auto arr = json::array();
            for (const auto& b : db.batches(req.has_param("all")))
                if (a.human || b.actor == a.name) arr.push_back(b.to_json());
            return json{{"entries", arr}};
        }));
        srv.Get(R"(/batch/([^/]+))", wrap([this](const ActorConfig& a, Req req, const json&) {
            auto b = db.batch(req.matches[1]);
            if (!a.human && b.actor != a.name) throw HttpError{403, "forbidden", "batch " + b.id + " belongs to " + b.actor};
            return b.to_json();
        }));

        // ---- writes ----
        srv.Post("/observe", wrap([this](const ActorConfig& a, Req, const json& body) {
            auto in = parse_source(body);
            require_own_domain(a, in.domain);
            return json{{"id", db.observe(in)}};
        }));
        srv.Post("/assert", wrap([this](const ActorConfig& a, Req, const json& body) {
            auto in = parse_claim(body);
            require_own_domain(a, in.domain);
            for (const auto& s : in.support) visible_node(a, s);
            const auto id = db.assert_claim(in);
            return after_write(a, id, in.text, in.subject, in.predicate, in.value);
        }));
        srv.Post("/derive", wrap([this](const ActorConfig& a, Req, const json& body) {
            auto in = parse_derive(body);
            require_own_domain(a, in.domain);
            for (const auto& f : in.from) visible_node(a, f);
            const auto id = db.derive(in);
            return after_write(a, id, in.text, in.subject, in.predicate, in.value);
        }));
        srv.Post("/hypothesize", wrap([this](const ActorConfig& a, Req, const json& body) {
            auto in = parse_hypothesis(body);
            require_own_domain(a, in.domain);
            for (const auto& f : in.from) visible_node(a, f);
            const auto id = db.hypothesize(in);
            return after_write(a, id, in.text, in.subject, in.predicate, in.value);
        }));
        srv.Post("/confirm", wrap([this](const ActorConfig& a, Req, const json& body) {
            ConfirmInput in;
            in.hypothesis = need_str(body, "hypothesis");
            in.support = ids_of(body, "support");
            in.credence = body.value("credence", 0.5);
            in.method = enum_or(credence_method_from(body.value("method", "stated")), "method", body.value("method", ""));
            const Node h = own_node(a, in.hypothesis);
            for (const auto& s : in.support) visible_node(a, s);
            const auto id = db.confirm(in);
            return after_write(a, id, h.payload.value("text", ""), h.payload.value("subject", ""), h.payload.value("predicate", ""),
                               Value::from_json(h.payload.value("value", json::object())));
        }));
        srv.Post("/link", wrap([this](const ActorConfig& a, Req, const json& body) {
            const auto src = need_str(body, "src"), dst = need_str(body, "dst");
            const auto type = enum_or(edge_type_from(body.value("type", "")), "edge type", body.value("type", ""));
            const Node s = visible_node(a, src), d = visible_node(a, dst);
            if (!owns(a, s.domain) && !owns(a, d.domain)) throw HttpError{403, "forbidden", "neither end of the edge is in a domain of " + a.name};
            std::optional<std::string> rule;
            if (body.contains("rule") && body["rule"].is_string()) rule = body["rule"].get<std::string>();
            db.link(src, dst, type, rule, body.value("reason", ""));
            return json{{"ok", true}};
        }));
        srv.Post("/retract", wrap([this](const ActorConfig& a, Req, const json& body) {
            const auto id = need_str(body, "id");
            own_node(a, id);
            db.retract(id, body.value("reason", ""));
            return json{{"ok", true}};
        }));
        srv.Post("/supersede", wrap([this](const ActorConfig& a, Req, const json& body) {
            const auto old_id = need_str(body, "old_id");
            const Node old = own_node(a, old_id);
            const auto reason = body.value("reason", "");
            std::string id;
            if (old.type == NodeType::source) { auto in = parse_source(body); require_own_domain(a, in.domain); id = db.supersede(old_id, in, reason); }
            else if (body.contains("from")) { auto in = parse_derive(body); require_own_domain(a, in.domain); id = db.supersede(old_id, in, reason); }
            else { auto in = parse_claim(body); require_own_domain(a, in.domain); id = db.supersede(old_id, in, reason); }
            return json{{"id", id}};
        }));
        srv.Post("/recompute", wrap([this](const ActorConfig& a, Req, const json& body) {
            const auto id = need_str(body, "id");
            own_node(a, id);
            std::optional<double> cr;
            if (body.contains("credence") && body["credence"].is_number()) cr = body["credence"].get<double>();
            db.recompute(id, cr);
            auto n = db.get(id);
            return json{{"id", id}, {"credence", n && n->credence ? json(n->credence->value) : json()}};
        }));
        srv.Post("/rule", wrap([this](const ActorConfig& a, Req, const json& body) {
            RuleInput in{need_str(body, "name"), body.value("description", ""), body.value("domain", "general")};
            require_own_domain(a, in.domain);
            return json{{"id", db.define_rule(in)}};
        }));
        srv.Post("/predicate", wrap([this](const ActorConfig&, Req, const json& body) {
            PredicateInput in;
            in.name = need_str(body, "name");
            in.value_type = enum_or(value_type_from(body.value("type", "text")), "value type", body.value("type", ""));
            in.unit = body.value("unit", "");
            in.enum_values = ids_of(body, "enum_values");
            in.note = body.value("note", "");
            db.define_predicate(in);
            return json{{"ok", true}};
        }));
        srv.Post("/ask", wrap([this](const ActorConfig& a, Req, const json& body) {
            QuestionInput in;
            in.text = need_str(body, "text");
            in.why_open = body.value("why_open", "");
            in.acceptance_criterion = body.value("acceptance_criterion", "");
            in.about = ids_of(body, "about");
            in.domain = body.value("domain", "general");
            in.sensitivity = enum_or(sensitivity_from(body.value("sensitivity", "domain")), "sensitivity", body.value("sensitivity", ""));
            require_own_domain(a, in.domain);
            for (const auto& x : in.about) visible_node(a, x);
            return json{{"id", db.ask(in)}};
        }));
        srv.Post("/progress", wrap([this](const ActorConfig& a, Req, const json& body) {
            const auto id = need_str(body, "id");
            own_node(a, id);
            db.note_progress(id, body.value("text", ""));
            return json{{"ok", true}};
        }));
        srv.Post("/close", wrap([this](const ActorConfig& a, Req, const json& body) {
            const auto id = need_str(body, "id");
            own_node(a, id);
            db.close_question(id, body.value("resolution", ""));
            return json{{"ok", true}};
        }));
        srv.Post("/dismiss", wrap([this](const ActorConfig& a, Req, const json& body) {
            const auto x = need_str(body, "a"), y = need_str(body, "b");
            const Node na = visible_node(a, x), nb = visible_node(a, y);
            if (!owns(a, na.domain) && !owns(a, nb.domain)) throw HttpError{403, "forbidden", "neither side is in a domain of " + a.name};
            db.dismiss_conflict(x, y, body.value("reason", ""));
            return json{{"ok", true}};
        }));
        srv.Post("/resolve", wrap([this](const ActorConfig& a, Req, const json& body) {
            const auto x = need_str(body, "a"), y = need_str(body, "b");
            const Node na = visible_node(a, x), nb = visible_node(a, y);
            if (!owns(a, na.domain) && !owns(a, nb.domain)) throw HttpError{403, "forbidden", "neither side is in a domain of " + a.name};
            db.resolve_conflict(x, y, body.value("reason", ""));
            return json{{"ok", true}};
        }));
        srv.Post("/batch/begin", wrap([this](const ActorConfig&, Req, const json& body) {
            db.leave_batch();
            return json{{"id", db.begin_batch(need_str(body, "purpose"))}};
        }));
        srv.Post("/batch/end", wrap([this](const ActorConfig& a, Req, const json& body) {
            const auto id = need_str(body, "id");
            auto b = db.batch(id);
            if (!a.human && b.actor != a.name) throw HttpError{403, "forbidden", "batch " + id + " belongs to " + b.actor};
            db.end_batch(id);
            return db.batch(id).to_json();
        }));
        srv.Post("/batch/reject", wrap([this](const ActorConfig& a, Req, const json& body) {
            if (!a.human) throw HttpError{403, "forbidden", "rejecting a batch is a human's verdict"};
            const auto id = need_str(body, "id");
            db.reject_batch(id, body.value("reason", ""));
            return db.batch(id).to_json();
        }));
        srv.Post("/reindex-embeddings", wrap([this](const ActorConfig& a, Req, const json&) {
            if (!a.human) throw HttpError{403, "forbidden", "reindexing is a human's call"};
            if (quiet_now()) throw HttpError{409, "quiet_hours", "the embedding GPU sleeps during " + cfg.quiet_hours};
            return json{{"written", db.reindex_embeddings()}, {"status", db.embedding_status().to_json()}};
        }));
    }
};

Server::Server(Config c) : impl_(std::make_unique<Impl>(std::move(c))) {}
Server::~Server() { stop(); }

int Server::start() {
    auto& s = impl_->srv;
    if (impl_->cfg.port == 0) impl_->port = s.bind_to_any_port(impl_->cfg.listen);
    else { if (!s.bind_to_port(impl_->cfg.listen, impl_->cfg.port)) throw Error(ErrorCode::bad_input, "cannot bind " + impl_->cfg.listen + ":" + std::to_string(impl_->cfg.port)); impl_->port = impl_->cfg.port; }
    impl_->thread = std::thread([this] { impl_->srv.listen_after_bind(); });
    s.wait_until_ready();
    return impl_->port;
}

void Server::stop() {
    if (impl_->thread.joinable()) { impl_->srv.stop(); impl_->thread.join(); }
}

}  // namespace beliefd
