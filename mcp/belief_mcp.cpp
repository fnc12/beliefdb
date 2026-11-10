// belief-mcp — beliefdb as an MCP server over stdio, for Claude Code, Claude Desktop, Cursor,
// ChatGPT connectors and any other MCP client. One process per client, the database opened directly;
// no beliefd needed.
//
//   belief-mcp --db ~/memory.sqlite [--actor NAME] [--no-quota]
//
// Protocol: JSON-RPC 2.0, one message per line on stdin/stdout (the MCP stdio transport).
// Methods: initialize, ping, tools/list, tools/call; notifications are accepted and not answered.
// Logging goes to stderr only: stdout belongs to the protocol.
//
// Embeddings (semantic search, the text duplicate gate) are on when $BELIEF_EMBEDDINGS points at an
// OpenAI-style /v1/embeddings server ($BELIEF_EMBEDDING_MODEL, default bge-m3); otherwise full-text only.
#include "json_io.hpp"

#include <beliefdb/store.hpp>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

using namespace beliefdb;
using beliefd::HttpError;
using nlohmann::json;

namespace {

const char* kVersion = "0.1.0";

const char* kInstructions =
    "beliefdb is the agent's memory of justified claims. Rules for using it:\n"
    "1. Before writing anything, call `search` (and `similar` for structured claims): if the claim is already there, "
    "add your evidence to it with `link` (supports) instead of writing a duplicate.\n"
    "2. Every claim needs evidence. First `observe` the source (what the user said, a document, a measurement), "
    "then `assert` the claim with that source id in `support`. Never assert your own opinion.\n"
    "3. Conclusions you draw yourself go in as `hypothesize` (from claim ids, with a rule name), not as claims; "
    "a hypothesis becomes a claim only when confirmed by evidence.\n"
    "4. When a fact changes, `retract` the old source or claim with a reason, or `assert` the new one and `link` it "
    "`replaces` the old one; nothing is deleted, dependents are flagged stale.\n"
    "5. `why` on any id shows what the claim stands on; `conflicts` lists contradictions to resolve with the user, "
    "never silently.\n"
    "Credence is one of 0.05 0.2 0.35 0.5 0.65 0.8 0.95; method `stated` for what someone said, `measured` for data.";

json schema(std::initializer_list<std::pair<const char*, json>> props, std::vector<std::string> required = {}) {
    json p = json::object();
    for (const auto& [k, v] : props) p[k] = v;
    json s{{"type", "object"}, {"properties", p}};
    if (!required.empty()) s["required"] = required;
    return s;
}
json str(const char* desc) { return json{{"type", "string"}, {"description", desc}}; }
json num(const char* desc) { return json{{"type", "number"}, {"description", desc}}; }
json ids(const char* desc) { return json{{"type", "array"}, {"items", {{"type", "string"}}}, {"description", desc}}; }

json tools() {
    return json::array({
        json{{"name", "observe"}, {"description", "Record a piece of evidence (a source): something the user said, a document, a measurement, an LLM output. Returns the source id to use as support. The same content or uri observed twice returns the same id."},
             {"inputSchema", schema({{"content", str("what the source says, verbatim or a close summary")}, {"kind", json{{"type", "string"}, {"enum", {"human_statement", "document", "measurement", "llm_output"}}, {"description", "nature of the evidence (default document)"}}},
                                     {"uri", str("where it came from: a URL, chat:…, file:…, or the conversation id")}, {"attribution", str("who said it, e.g. the user's name, an author, a site")},
                                     {"observed_at", str("ISO date of the observation (default today)")}, {"domain", str("domain of knowledge, e.g. health, work, home (default general)")},
                                     {"directness", json{{"type", "string"}, {"enum", {"primary", "secondary"}}, {"description", "secondary = a retelling of someone else's conclusion"}}}},
                                    {"content"})}},
        json{{"name", "assert"}, {"description", "Record a claim justified by evidence. `support` must hold at least one source id (from observe) or claim id. Refused as duplicate_claim when the same structured claim already exists: then link your source to it instead."},
             {"inputSchema", schema({{"text", str("the claim, one short statement")}, {"support", ids("source or claim ids this claim stands on")},
                                     {"credence", num("0.05 0.2 0.35 0.5 0.65 0.8 or 0.95 (default 0.5)")}, {"method", json{{"type", "string"}, {"enum", {"stated", "measured", "llm_judgment", "manual"}}, {"description", "how the credence was obtained (default stated)"}}},
                                     {"subject", str("optional structured subject, e.g. user, project_x")}, {"predicate", str("optional structured predicate, e.g. weight, deadline")},
                                     {"value", json{{"description", "optional typed value: a number, a bool, a string, or {type: number, value, unit} / {type: number, lo, hi, unit}"}}},
                                     {"domain", str("default general")}, {"scope", json{{"type", "object"}, {"description", "{valid_from, valid_to, conditions: {key: value}, note}"}}},
                                     {"despite", ids("ids of claims this one knowingly refines or contradicts (bypasses the duplicate gate)")}},
                                    {"text", "support"})}},
        json{{"name", "derive"}, {"description", "Record a claim derived from other claims through a named rule. Credence defaults to the minimum over the premises. The rule is created by name if it does not exist."},
             {"inputSchema", schema({{"text", str("the derived claim")}, {"from", ids("claim ids it is derived from")}, {"rule", str("rule name, e.g. dose_from_prescription")},
                                     {"credence", num("optional; with method min it is computed")}, {"method", json{{"type", "string"}, {"enum", {"min", "rule", "noisy_or", "llm_judgment", "manual"}}}},
                                     {"subject", str("")}, {"predicate", str("")}, {"value", json{{"description", "optional typed value"}}}, {"domain", str("default general")}},
                                    {"text", "from", "rule"})}},
        json{{"name", "hypothesize"}, {"description", "Record your own conclusion as a hypothesis built on claims (or other hypotheses). It justifies nothing until confirmed. The rule is created by name if it does not exist."},
             {"inputSchema", schema({{"text", str("the hypothesis")}, {"from", ids("claim or hypothesis ids")}, {"rule", str("rule name, e.g. generalization")}, {"domain", str("default general")}}, {"text", "from", "rule"})}},
        json{{"name", "search"}, {"description", "Find claims and hypotheses by words (and by meaning when embeddings are configured). Use before writing and when answering from memory."},
             {"inputSchema", schema({{"query", str("free text")}, {"domain", str("optional domain filter")}, {"limit", num("default 20")}}, {"query"})}},
        json{{"name", "similar"}, {"description", "What must be looked at before writing a claim: structural matches (same subject and predicate) first, then text matches."},
             {"inputSchema", schema({{"text", str("")}, {"subject", str("")}, {"predicate", str("")}, {"domain", str("")}, {"limit", num("default 10")}}, {"text"})}},
        json{{"name", "why"}, {"description", "The justification tree of a node down to its sources, with credence and method at every level, and the evidence summary (nutrition)."},
             {"inputSchema", schema({{"id", str("claim, hypothesis or source id")}, {"depth", num("levels to unfold (default all)")}}, {"id"})}},
        json{{"name", "conflicts"}, {"description", "Open contradictions between claims (same subject and predicate, overlapping scope, incompatible values), ranked by what stands on them."},
             {"inputSchema", schema({{"domain", str("optional domain filter")}})}},
        json{{"name", "retract"}, {"description", "Mark a source, claim, rule or hypothesis as retracted with a reason. Nothing is deleted; everything that depends on it is flagged stale."},
             {"inputSchema", schema({{"id", str("")}, {"reason", str("")}}, {"id", "reason"})}},
        json{{"name", "link"}, {"description", "Add an edge: supports (source→claim, more evidence for an existing claim), contradicts, refines (narrower scope), replaces (src is the new node, dst the old; the old becomes superseded)."},
             {"inputSchema", schema({{"src", str("")}, {"dst", str("")}, {"type", json{{"type", "string"}, {"enum", {"supports", "contradicts", "refines", "replaces", "derives_from", "assumes"}}}}, {"reason", str("for replaces")}}, {"src", "dst", "type"})}},
        json{{"name", "recent"}, {"description", "Claims and hypotheses written since a date, oldest first: what changed in memory lately."},
             {"inputSchema", schema({{"since", str("ISO date or datetime")}, {"domain", str("")}, {"limit", num("default 50")}}, {"since"})}},
        json{{"name", "predict"}, {"description", "Record a forecast about a claim or hypothesis BEFORE the outcome is known (p defaults to its credence). Settle it later with settle; this is how the memory learns which sources and rules to trust."},
             {"inputSchema", schema({{"id", str("claim or hypothesis id")}, {"p", num("probability it turns out true (default: the node's credence)")}, {"resolve_by", str("ISO date by which it should be known")}, {"note", str("")}}, {"id"})}},
        json{{"name", "settle"}, {"description", "Settle a forecast with what actually happened. Updates the Brier score and the rule's track record."},
             {"inputSchema", schema({{"id", str("prediction id (p_…)")}, {"outcome", json{{"type", "boolean"}, {"description", "true if the claim turned out true"}}}, {"note", str("")}}, {"id", "outcome"})}},
        json{{"name", "calibration"}, {"description", "How past forecasts fared, grouped by credence step, method, rule and attribution: which sources and rules deserve their credence."},
             {"inputSchema", schema({{"domain", str("optional domain filter")}})}},
        json{{"name", "orphans"}, {"description", "Live claims whose every source is older than N months with nothing newer supporting them: knowledge that went stale silently and needs reconfirmation."},
             {"inputSchema", schema({{"months", num("default 6")}, {"domain", str("optional domain filter")}})}},
        json{{"name", "stats"}, {"description", "Counts of sources, claims, hypotheses, open conflicts, and the embedding status."}, {"inputSchema", schema({})}},
    });
}

std::string today() {
    char buf[16];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

struct Mcp {
    Store db;

    explicit Mcp(Store s) : db(std::move(s)) {}

    void ensure_rule(const std::string& name) {
        if (name.empty() || name.rfind("r_", 0) == 0) return;
        if (!db.find_rule(name)) db.define_rule({.name = name, .description = name});
    }

    json call(const std::string& name, const json& a) {
        if (name == "observe") {
            json j = a;
            if (!j.contains("observed_at") || j["observed_at"].get<std::string>().empty()) j["observed_at"] = today();
            if (!j.contains("kind")) j["kind"] = "document";
            const std::string id = db.observe(beliefd::parse_source(j));
            return json{{"id", id}};
        }
        if (name == "assert") {
            const std::string id = db.assert_claim(beliefd::parse_claim(a));
            json out{{"id", id}};
            auto cs = db.conflicts_of(id);
            if (!cs.empty()) { out["conflicts"] = json::array(); for (const auto& c : cs) out["conflicts"].push_back(c.to_json()); }
            return out;
        }
        if (name == "derive") {
            ensure_rule(a.value("rule", ""));
            return json{{"id", db.derive(beliefd::parse_derive(a))}};
        }
        if (name == "hypothesize") {
            ensure_rule(a.value("rule", ""));
            return json{{"id", db.hypothesize(beliefd::parse_hypothesis(a))}};
        }
        if (name == "search") return db.search(beliefd::need_str(a, "query"), a.value("domain", ""), a.value("limit", 20)).to_json();
        if (name == "similar") {
            SimilarQuery q;
            q.text = beliefd::need_str(a, "text");
            q.subject = a.value("subject", "");
            q.predicate = a.value("predicate", "");
            q.domain = a.value("domain", "");
            q.limit = a.value("limit", 10);
            return db.similar(q).to_json();
        }
        if (name == "why") {
            const std::string id = beliefd::need_str(a, "id");
            json j = db.why(id, a.value("depth", -1)).to_json();
            try { j["nutrition"] = db.nutrition(id).to_json(); } catch (const Error&) {}
            return j;
        }
        if (name == "conflicts") {
            json arr = json::array();
            for (const auto& c : db.conflicts(a.value("domain", ""))) arr.push_back(c.to_json());
            return json{{"entries", arr}};
        }
        if (name == "retract") {
            db.retract(beliefd::need_str(a, "id"), beliefd::need_str(a, "reason"));
            return json{{"ok", true}};
        }
        if (name == "link") {
            const auto type = edge_type_from(a.value("type", ""));
            if (!type) throw HttpError{400, "bad_input", "unknown edge type: " + a.value("type", "")};
            db.link(beliefd::need_str(a, "src"), beliefd::need_str(a, "dst"), *type, std::nullopt, a.value("reason", ""));
            return json{{"ok", true}};
        }
        if (name == "recent") {
            json arr = json::array();
            for (const auto& e : db.recent(a.value("domain", ""), beliefd::need_str(a, "since"), a.value("limit", 50))) arr.push_back(e.to_json());
            return json{{"entries", arr}};
        }
        if (name == "predict") {
            PredictionInput in;
            in.node_id = beliefd::need_str(a, "id");
            if (a.contains("p") && a["p"].is_number()) in.p = a["p"].get<double>();
            in.resolve_by = a.value("resolve_by", "");
            in.note = a.value("note", "");
            return json{{"id", db.predict(in)}};
        }
        if (name == "settle") {
            if (!a.contains("outcome") || !a["outcome"].is_boolean()) throw HttpError{400, "bad_input", "outcome must be true or false"};
            db.settle(beliefd::need_str(a, "id"), a["outcome"].get<bool>(), a.value("note", ""));
            return db.prediction(a["id"].get<std::string>()).to_json();
        }
        if (name == "calibration") return db.calibration(a.value("domain", "")).to_json();
        if (name == "orphans") {
            json arr = json::array();
            for (const auto& o : db.orphans(a.value("months", 6.0), a.value("domain", ""))) arr.push_back(o.to_json());
            return json{{"entries", arr}};
        }
        if (name == "stats") {
            const auto c = db.counts();
            json j{{"nodes", c.nodes}, {"edges", c.edges}, {"sources", c.sources}, {"claims", c.claims}, {"hypotheses", c.hypotheses}, {"rules", c.rules}, {"open_batches", c.open_batches}};
            j["conflicts"] = db.triage_stats().to_json();
            j["embeddings"] = db.embedding_status().to_json();
            return j;
        }
        throw HttpError{404, "not_found", "unknown tool: " + name};
    }
};

json rpc_result(const json& id, json result) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }
json rpc_error(const json& id, int code, const std::string& message) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}}; }
json tool_text(const json& payload, bool is_error = false) {
    json r{{"content", json::array({json{{"type", "text"}, {"text", payload.is_string() ? payload.get<std::string>() : payload.dump(2)}}})}};
    if (is_error) r["isError"] = true;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::string db_path, actor = "mcp";
    bool no_quota = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", arg.c_str()); std::exit(2); } return argv[++i]; };
        if (arg == "--db") db_path = next();
        else if (arg == "--actor") actor = next();
        else if (arg == "--no-quota") no_quota = true;
        else if (arg == "--version") { std::printf("belief-mcp %s\n", kVersion); return 0; }
        else if (arg == "-h" || arg == "--help") { std::printf("usage: belief-mcp --db PATH [--actor NAME] [--no-quota]\n"); return 0; }
        else { std::fprintf(stderr, "unknown argument %s\n", arg.c_str()); return 2; }
    }
    if (db_path.empty()) { if (const char* e = std::getenv("BELIEF_DB")) db_path = e; else db_path = "beliefdb.sqlite"; }

    Mcp mcp(Store::open(db_path, actor));
    if (no_quota) {
        Policy p = mcp.db.policy();
        p.daily_claims = 1000000; p.daily_hypotheses = 1000000; p.open_hypotheses = 0; p.batch_claims = 1000000; p.batch_hypotheses = 1000000;
        mcp.db.set_policy(p);
    }
    if (const char* e = std::getenv("BELIEF_EMBEDDINGS"); e && *e) {
        EmbeddingConfig cfg;
        cfg.base_url = e;
        if (const char* m = std::getenv("BELIEF_EMBEDDING_MODEL"); m && *m) cfg.model = m;
        mcp.db.set_embeddings(cfg);
    }
    std::fprintf(stderr, "belief-mcp %s: %s as %s\n", kVersion, db_path.c_str(), actor.c_str());

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json msg = json::parse(line, nullptr, false);
        if (!msg.is_object()) { std::cout << rpc_error(nullptr, -32700, "parse error").dump() << '\n' << std::flush; continue; }
        const json id = msg.value("id", json());
        const std::string method = msg.value("method", "");
        const json params = msg.value("params", json::object());
        if (id.is_null()) continue;  // a notification (initialized, cancelled, progress): nothing to answer
        json reply;
        try {
            if (method == "initialize") {
                reply = rpc_result(id, json{{"protocolVersion", params.value("protocolVersion", "2025-06-18")},
                                             {"capabilities", {{"tools", {{"listChanged", false}}}}},
                                             {"serverInfo", {{"name", "beliefdb"}, {"version", kVersion}}},
                                             {"instructions", kInstructions}});
            } else if (method == "ping") {
                reply = rpc_result(id, json::object());
            } else if (method == "tools/list") {
                reply = rpc_result(id, json{{"tools", tools()}});
            } else if (method == "tools/call") {
                const std::string name = params.value("name", "");
                const json args = params.value("arguments", json::object());
                try {
                    reply = rpc_result(id, tool_text(mcp.call(name, args.is_object() ? args : json::object())));
                } catch (const Error& e) {
                    reply = rpc_result(id, tool_text("error[" + std::string(to_string(e.code)) + "]: " + e.what(), true));
                } catch (const HttpError& e) {
                    reply = rpc_result(id, tool_text("error[" + e.code + "]: " + e.message, true));
                } catch (const std::exception& e) {
                    reply = rpc_result(id, tool_text(std::string("error: ") + e.what(), true));
                }
            } else {
                reply = rpc_error(id, -32601, "method not found: " + method);
            }
        } catch (const std::exception& e) {
            reply = rpc_error(id, -32603, e.what());
        }
        std::cout << reply.dump() << '\n' << std::flush;
    }
    return 0;
}
