// belief — command-line front end for beliefdb.
#include <beliefdb/store.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace beliefdb;
using nlohmann::json;

namespace {

const char* kUsage = R"(belief — graph of justified claims

usage: belief [--db PATH] [--actor NAME] [--batch ID] <command> [options]

  init                                   create the database
  observe  --kind K --uri U (--content C | --file F) [--observed-at T] [--attribution WHO] [--trust-hint X]
           [--directness primary|secondary] [--domain D] [--sensitivity S]
           K: measurement|document|human_statement|llm_output; secondary = a retelling of somebody else's conclusion
  rule     --name N [--description D] [--domain D]
  predicate --name N --type T [--unit U] [--enum-values A,B,..] [--note TEXT]     T: bool|number|enum|ref|text
  predicates                             list the registry
  assert   --text T --support ID[,ID..] --credence X --method M [<scope>] [--subject S] [--predicate P] [<value>]
           [--domain D] [--sensitivity S] [--despite ID,..]
  derive   --text T --from ID[,ID..] --rule R [--credence X] [--method M] [<scope>] [--subject S] [--predicate P] [<value>]
           [--domain D] [--despite ID,..]
  hypothesize --text T --from ID[,ID..] --rule R [<scope>] [--subject S] [--predicate P] [<value>] [--domain D] [--despite ID,..]
                                          a machine inference awaiting review: premises are claims only, credence = min over them,
                                          it justifies nothing and expires unreviewed; quotas apply
  confirm  HID --support ID[,ID..] --credence X --method M
                                          evidence turns it into a claim (keeps the derivation, adds the supports)
  hypotheses [--domain D] [--all] [--json] awaiting review, oldest first; rejecting one is `retract HID --reason R`
  link     SRC DST TYPE [--rule R] [--reason R]
                                          TYPE: supports|contradicts|derives_from|assumes|answers|refines|replaces
                                          (contradicts is symmetric; replaces: SRC is the new node, DST the old,
                                          it also supersedes DST and flags its dependents)
  retract  ID --reason R                 source, claim or rule; dependents are flagged stale, nothing is deleted
  supersede OLD_ID --reason R <observe | assert | derive options>
                                          records the correction, links it replaces OLD_ID, flags dependents
  recompute ID [--credence X]            explicit recompute of a stale claim: follows replaces, drops retracted
                                          premises; min computes the value, other methods need --credence
  stale    [--domain D] [--json]         flagged claims, shallow first, with the cause of each flag
  what-if  ID --credence X [--json]      what would change below ID; nothing is written
  conflicts [--domain D] [--all] [--older-than DAYS] [--json]
                                          contradictions, ranked by what stands on them: explicit edges plus detected pairs
                                          (same subject+predicate, overlapping scope, incompatible typed value); each scan
                                          updates the review state; --all shows dismissed/resolved/vanished pairs too
  dismiss  ID_A ID_B --reason R          "not a conflict"; expires if either side changes
  resolve  ID_A ID_B --reason R          settled by refines / scope / retract; same expiry
  refinements [--domain D] [--json]     pairs that disagree only inside a nested scope: link them refines (or contradicts)
  ask      --text T --criterion C [--why-open W] [--about ID,ID..] [--domain D] [--sensitivity S]
  progress QID --note TEXT               dated progress note on an open question
  close    QID --resolution TEXT         what answered it, or why it is moot
  questions [--domain D] [--all] [--json] open questions (--all: closed too), oldest first
  batch begin --purpose TEXT             open a batch for this actor; prints its id, export BELIEF_BATCH=ID for the next calls
  batch end [ID]                         close the current batch (or ID)
  batch show ID [--json]                 what was written in it
  batch reject ID --reason R             review verdict: retract everything created in it
  batches [--all] [--json]               open batches (--all: closed too), newest first
  search   QUERY [--domain D] [--limit N] [--json]
                                          full-text (trigram) and, with embeddings, semantic search over claims and hypotheses
  similar  --text T [--subject S] [--predicate P] [<value>] [<scope>] [--domain D] [--limit N] [--json]
                                          what an agent must look at before writing: structural matches first, then text
  reindex-embeddings                      vectors for claims written while the provider was down, or after a model change
  why      ID [--depth N] [--json]        justification tree; the header line is the node's nutrition
  nutrition ID [--json]                   what the node stands on: sources, heads, weight, concentration
  show     ID [--json]
  stats
  predict  ID [--p X] [--by DATE] [--note T]  a forecast about a claim or hypothesis, recorded before the outcome; p = its credence
  resolve  PID --outcome yes|no [--note T] what happened; brier = (p - outcome)^2, the rule's track record is updated
  predictions [--domain D] [--all] [--json] open forecasts (--all: resolved too), oldest first
  calibration [--domain D] [--json]      how forecasts fared by credence step, method, rule and attribution
  orphans  [--months N] [--domain D] [--json]  live claims whose every source is older than N months (6) with nothing newer supporting them

  <scope>: --valid-from T, --valid-to T, --cond key=value (repeatable), --scope-note TEXT
           assert: valid_from defaults to the earliest source's observed_at
           derive: scope is the intersection of the premises', the flags may only narrow it
  <value>: --value-type bool|number|enum|ref|text (default text) and --value V,
           for number either --value X or --lo X --hi Y, plus --unit U (kg/g/mg, m/cm, s/min/h/d, %, USD/KZT/EUR/..)
  credence: 0.05 0.2 0.35 0.5 0.65 0.8 0.95
  method:   stated measured llm_judgment manual (assert); min rule noisy_or llm_judgment manual (derive)
  DB path:  --db, else $BELIEF_DB, else ./beliefdb.sqlite
  batch:    --batch, else $BELIEF_BATCH; writes inside it are counted against its quota (20 claims, 5 hypotheses)
            and the daily quota per domain and actor (50 claims, 10 hypotheses); a full or idle batch closes itself
  dedup:    a claim with the same subject and predicate, overlapping scope and compatible value already present is
            refused (duplicate_claim): add supports to it, or --despite ID and then link refines/contradicts
  embeddings: $BELIEF_EMBEDDINGS = base URL of an OpenAI-style /v1/embeddings server (e.g. http://localhost:12000),
            $BELIEF_EMBEDDING_MODEL (default bge-m3); unset = full-text only; unreachable = full-text only, and search says so
)";

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::vector<std::string>> opts;

    bool has(const std::string& k) const { return opts.count(k) > 0; }
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = opts.find(k);
        return it == opts.end() ? def : it->second.back();
    }
    const std::vector<std::string>& all(const std::string& k) const {
        static const std::vector<std::string> none;
        auto it = opts.find(k);
        return it == opts.end() ? none : it->second;
    }
};

Args parse(int argc, char** argv) {
    Args a;
    static const std::vector<std::string> flags{"json", "all"};  // boolean switches; everything else takes a value
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string k = s.substr(2);
            auto eq = k.find('=');
            if (eq != std::string::npos) {
                a.opts[k.substr(0, eq)].push_back(k.substr(eq + 1));
            } else if (std::find(flags.begin(), flags.end(), k) != flags.end() || i + 1 >= argc) {
                a.opts[k].push_back("1");
            } else {
                a.opts[k].push_back(argv[++i]);
            }
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

std::vector<std::string> split_ids(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!item.empty()) out.push_back(item);
    return out;
}

[[noreturn]] void usage_error(const std::string& msg) {
    std::cerr << "error: " << msg << "\n\n" << kUsage;
    std::exit(1);
}

std::string need(const Args& a, const std::string& k) {
    if (!a.has(k)) usage_error("--" + k + " is required");
    return a.get(k);
}

Scope scope_from(const Args& a) {
    Scope s;
    if (a.has("valid-from")) s.valid_from = a.get("valid-from");
    if (a.has("valid-to")) s.valid_to = a.get("valid-to");
    if (a.has("scope-note")) s.note = a.get("scope-note");
    for (const auto& kv : a.all("cond")) {
        auto eq = kv.find('=');
        if (eq == std::string::npos) usage_error("--cond expects key=value, got: " + kv);
        s.conditions[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    return s;
}

Value value_from(const Args& a) {
    const auto type = value_type_from(a.get("value-type", "text"));
    if (!type) usage_error("unknown value type: " + a.get("value-type"));
    switch (*type) {
        case ValueType::boolean: {
            const auto v = a.get("value");
            if (v == "true" || v == "1" || v == "yes" || v == "да") return Value::boolean(true);
            if (v == "false" || v == "0" || v == "no" || v == "нет") return Value::boolean(false);
            usage_error("bool value must be true/false, got: " + v);
        }
        case ValueType::number: {
            if (a.has("lo") || a.has("hi")) {
                if (!(a.has("lo") && a.has("hi"))) usage_error("number interval needs both --lo and --hi");
                return Value::number(std::strtod(a.get("lo").c_str(), nullptr), std::strtod(a.get("hi").c_str(), nullptr), a.get("unit"));
            }
            if (!a.has("value")) usage_error("number needs --value X or --lo X --hi Y");
            return Value::point(std::strtod(a.get("value").c_str(), nullptr), a.get("unit"));
        }
        case ValueType::enumeration: return Value::enum_member(need(a, "value"));
        case ValueType::ref: return Value::reference(need(a, "value"));
        case ValueType::text: return Value::text(a.get("value"));
    }
    return Value::text("");
}

Sensitivity sens_from(const Args& a) {
    if (!a.has("sensitivity")) return Sensitivity::domain;
    auto v = sensitivity_from(a.get("sensitivity"));
    if (!v) usage_error("unknown sensitivity: " + a.get("sensitivity"));
    return *v;
}

std::string fmt_nutrition(const Nutrition& n) {
    std::ostringstream os;
    os.precision(2);
    os << std::fixed;
    os << "nutrition: " << n.n_sources << (n.n_sources == 1 ? " source" : " sources") << ", " << n.n_attributions
       << (n.n_attributions == 1 ? " head" : " heads");
    if (n.n_unattributed) os << " (" << n.n_unattributed << " unattributed)";
    if (n.n_secondary) os << ", " << n.n_secondary << " secondary";
    os << ", weight " << n.weighted << ", concentration " << n.concentration << ", depth " << n.depth << ", dilution " << n.dilution;
    if (!n.oldest_observed_at.empty()) os << ", observed " << n.oldest_observed_at.substr(0, 10) << ".." << n.newest_observed_at.substr(0, 10);
    return os.str();
}

std::string fmt_credence(const std::optional<Credence>& c) {
    if (!c) return "";
    std::ostringstream os;
    os << ' ' << c->value << ' ' << to_string(c->method);
    if (c->stale) os << " STALE";
    return os.str();
}

void print_tree(const WhyNode& w, const std::string& prefix, bool last, bool root) {
    std::string line;
    if (!root) {
        line += prefix + (last ? "└─ " : "├─ ");
        if (w.via) line += std::string(to_string(*w.via));
        if (w.rule) line += " [" + *w.rule + "]";
        line += " ";
    }
    line += w.id + " " + std::string(to_string(w.type));
    if (w.status != Status::active) line += " (" + std::string(to_string(w.status)) + ")";
    if (!root && w.notice) line += " ✗";
    line += fmt_credence(w.credence);
    line += "  " + w.label;
    if (w.truncated) line += "  …";
    if (!root && w.notice) line += "  [" + *w.notice + "]";
    std::cout << line << '\n';
    const std::string child_prefix = root ? "" : prefix + (last ? "   " : "│  ");
    for (size_t i = 0; i < w.children.size(); ++i)
        print_tree(w.children[i], child_prefix, i + 1 == w.children.size(), false);
}

std::string fmt_scope(const Scope& s) {
    std::string out;
    if (s.valid_from || s.valid_to) out += (s.valid_from ? *s.valid_from : "..") + ".." + (s.valid_to ? *s.valid_to : "");
    for (const auto& [k, v] : s.conditions.items()) out += (out.empty() ? "" : " ") + k + "=" + (v.is_string() ? v.get<std::string>() : v.dump());
    if (out.empty() && !s.note.empty()) out = s.note;
    return out.empty() ? "(any)" : out;
}

void print_side(const ConflictSide& s) {
    std::cout << "    " << s.id << (s.status != Status::active ? " (" + std::string(to_string(s.status)) + ")" : "") << fmt_credence(s.credence)
              << "  [" << s.value.describe() << "] " << fmt_scope(s.scope) << "  " << s.text << "\n"
              << "        " << s.n_sources << " sources, " << s.n_dependents << " dependents, domain " << s.domain << '\n';
}

void print_conflict(const ConflictEntry& c) {
    std::cout << c.subject << " " << c.predicate << ": " << c.n_dependents << " dependents"
              << (c.explicit_edge ? ", explicit" : "") << (c.detected ? ", detected" : "") << ", " << to_string(c.triage.state);
    if (c.triage.state == TriageState::open) std::cout << " " << c.triage.open_days << "d, seen " << c.triage.times_seen << "x";
    if (!c.triage.reason.empty()) std::cout << (c.triage.expired_at.empty() ? ", " : ", EXPIRED ") << c.triage.decided_by << ": " << c.triage.reason;
    if (c.overlap) std::cout << ", overlap " << fmt_scope(*c.overlap);
    if (!c.questions.empty()) { std::cout << ", open question"; for (const auto& q : c.questions) std::cout << ' ' << q; }
    std::cout << '\n';
    print_side(c.a);
    print_side(c.b);
}

void warn_conflicts(Store& db, const std::string& id) {
    for (const auto& c : db.conflicts_of(id)) {
        const auto& other = c.a.id == id ? c.b : c.a;
        std::cerr << "warning: " << id << " contradicts " << other.id << " [" << other.value.describe() << "] " << other.text
                  << "; see `belief conflicts`\n";
    }
}

SourceInput source_input(const Args& a) {
    SourceInput in;
    auto kind = source_kind_from(need(a, "kind"));
    if (!kind) usage_error("unknown kind: " + a.get("kind"));
    in.kind = *kind;
    if (a.has("directness")) {
        auto d = directness_from(a.get("directness"));
        if (!d) usage_error("unknown directness: " + a.get("directness"));
        in.directness = *d;
    }
    in.attribution = a.get("attribution");
    if (a.has("trust-hint")) in.trust_hint = std::strtod(a.get("trust-hint").c_str(), nullptr);
    in.uri = a.get("uri");
    if (a.has("file")) {
        std::ifstream f(a.get("file"), std::ios::binary);
        if (!f) usage_error("cannot read --file " + a.get("file"));
        std::stringstream ss;
        ss << f.rdbuf();
        in.content = ss.str();
    } else {
        in.content = need(a, "content");
    }
    in.observed_at = a.get("observed-at");
    in.domain = a.get("domain", "general");
    in.sensitivity = sens_from(a);
    return in;
}

std::string fmt_similar_line(const SimilarEntry& e) {
    std::ostringstream os;
    os << e.id << " (" << to_string(e.status) << ")";
    if (e.structural) os << " structural" << (e.compatible ? (*e.compatible ? ", compatible" : ", incompatible") : "");
    if (e.cosine) { os.precision(2); os << std::fixed << " cos " << *e.cosine; }
    if (e.fts_rank) { os.precision(3); os << std::defaultfloat << " bm25 " << *e.fts_rank; }
    os << "  " << e.domain << "  " << e.text;
    if (!e.predicate.empty()) os << "  [" << e.subject << " " << e.predicate << " = " << e.value.describe() << ", " << fmt_scope(e.scope) << "]";
    return os.str();
}

void print_similar(const SimilarResult& r, bool as_json) {
    if (as_json) { std::cout << r.to_json().dump(2) << '\n'; return; }
    for (const auto& e : r.entries) std::cout << fmt_similar_line(e) << '\n';
    if (r.entries.empty()) std::cout << "nothing found\n";
    std::cerr << "mode: " << r.mode << (r.notice.empty() ? "" : " (" + r.notice + ")") << '\n';
}

/// After a write: what else says something like it (the agent is expected to have looked already).
void note_similar(Store& db, const std::string& id, const std::string& text, const std::string& subject, const std::string& predicate, const Value& value) {
    SimilarQuery q;
    q.text = text;
    q.subject = subject;
    q.predicate = predicate;
    q.value = value;
    q.limit = 4;
    auto r = db.similar(q);
    for (const auto& e : r.entries) {
        if (e.id == id) continue;
        std::cerr << "note: similar " << fmt_similar_line(e) << '\n';
    }
}

ClaimInput claim_input(const Args& a) {
    ClaimInput in;
    in.text = need(a, "text");
    in.subject = a.get("subject");
    in.predicate = a.get("predicate");
    in.value = value_from(a);
    in.support = split_ids(need(a, "support"));
    in.credence = std::strtod(need(a, "credence").c_str(), nullptr);
    auto m = credence_method_from(need(a, "method"));
    if (!m) usage_error("unknown method: " + a.get("method"));
    in.method = *m;
    in.scope = scope_from(a);
    in.domain = a.get("domain", "general");
    in.sensitivity = sens_from(a);
    in.despite = split_ids(a.get("despite"));
    return in;
}

DeriveInput derive_input(const Args& a) {
    DeriveInput in;
    in.text = need(a, "text");
    in.subject = a.get("subject");
    in.predicate = a.get("predicate");
    in.value = value_from(a);
    in.from = split_ids(need(a, "from"));
    in.rule = need(a, "rule");
    if (a.has("credence")) in.credence = std::strtod(a.get("credence").c_str(), nullptr);
    auto m = credence_method_from(a.get("method", "min"));
    if (!m) usage_error("unknown method: " + a.get("method"));
    in.method = *m;
    in.scope = scope_from(a);
    in.domain = a.get("domain", "general");
    in.sensitivity = sens_from(a);
    in.despite = split_ids(a.get("despite"));
    return in;
}

HypothesisInput hypothesis_input(const Args& a) {
    HypothesisInput in;
    in.text = need(a, "text");
    in.subject = a.get("subject");
    in.predicate = a.get("predicate");
    in.value = value_from(a);
    in.from = split_ids(need(a, "from"));
    in.rule = need(a, "rule");
    in.scope = scope_from(a);
    in.domain = a.get("domain", "general");
    in.sensitivity = sens_from(a);
    in.despite = split_ids(a.get("despite"));
    return in;
}

void print_batch(const BatchEntry& b) {
    std::cout << b.id << ' ' << (b.closed_at.empty() ? "open" : "closed: " + b.closed_reason) << "  " << b.actor << "  " << b.purpose << '\n'
              << "    " << b.n_claims << " claims, " << b.n_hypotheses << " hypotheses, " << b.n_sources << " sources; started "
              << b.started_at.substr(0, 16) << ", last write " << b.last_write_at.substr(0, 16) << '\n';
}

void print_hypothesis(const HypothesisEntry& h) {
    std::cout << h.id << " (" << to_string(h.status) << ")" << fmt_credence(h.credence) << "  " << h.domain << "  " << h.text << '\n';
    if (!h.predicate.empty()) std::cout << "    " << h.subject << ' ' << h.predicate << " = " << h.value.describe() << ", " << fmt_scope(h.scope) << '\n';
    std::cout << "    from:";
    for (const auto& f : h.from) std::cout << ' ' << f;
    std::cout << "  rule " << h.rule;
    if (h.order > 1) std::cout << "  order " << h.order;
    if (!h.batch_id.empty()) std::cout << "  batch " << h.batch_id;
    std::cout << '\n';
    if (!h.outcome.empty()) std::cout << "    " << h.outcome << '\n';
    else if (!h.expires_at.empty()) std::cout << "    expires " << h.expires_at.substr(0, 10) << " (" << static_cast<int>(h.days_left) << " days left)\n";
}

int run(const Args& a) {
    if (a.has("help") || a.has("h")) { std::cout << kUsage; return 0; }
    if (a.positional.empty()) usage_error("missing command");
    const std::string cmd = a.positional[0];
    std::string db_path = a.get("db");
    if (db_path.empty()) {
        if (const char* env = std::getenv("BELIEF_DB")) db_path = env;
        else db_path = "beliefdb.sqlite";
    }
    auto db = Store::open(db_path, a.get("actor", "cli"));
    if (const char* env = std::getenv("BELIEF_EMBEDDINGS"); env && *env) {
        EmbeddingConfig cfg;
        cfg.base_url = env;
        if (const char* m = std::getenv("BELIEF_EMBEDDING_MODEL"); m && *m) cfg.model = m;
        db.set_embeddings(cfg);
    }
    // the batch is per invocation: resume it for writes, so they are counted and tagged
    static const std::vector<std::string> writes{"observe", "assert", "derive", "hypothesize", "confirm", "supersede", "link", "retract", "recompute", "ask", "progress", "close", "dismiss", "resolve"};
    if (std::find(writes.begin(), writes.end(), cmd) != writes.end()) {
        std::string batch = a.get("batch");
        if (batch.empty()) if (const char* env = std::getenv("BELIEF_BATCH")) batch = env;
        if (!batch.empty()) db.resume_batch(batch);
    }

    if (cmd == "init") {
        std::cout << "ok " << db_path << '\n';
    } else if (cmd == "observe") {
        std::cout << db.observe(source_input(a)) << '\n';
    } else if (cmd == "rule") {
        std::cout << db.define_rule({need(a, "name"), a.get("description"), a.get("domain", "general")}) << '\n';
    } else if (cmd == "assert") {
        const auto in = claim_input(a);
        const auto id = db.assert_claim(in);
        std::cout << id << '\n';
        warn_conflicts(db, id);
        note_similar(db, id, in.text, in.subject, in.predicate, in.value);
    } else if (cmd == "derive") {
        const auto in = derive_input(a);
        const auto id = db.derive(in);
        std::cout << id << '\n';
        warn_conflicts(db, id);
        note_similar(db, id, in.text, in.subject, in.predicate, in.value);
    } else if (cmd == "hypothesize") {
        const auto in = hypothesis_input(a);
        const auto id = db.hypothesize(in);
        std::cout << id << '\n';
        note_similar(db, id, in.text, in.subject, in.predicate, in.value);
    } else if (cmd == "search") {
        if (a.positional.size() != 2) usage_error("search QUERY");
        print_similar(db.search(a.positional[1], a.get("domain"), std::atoi(a.get("limit", "20").c_str())), a.has("json"));
    } else if (cmd == "similar") {
        SimilarQuery q;
        q.text = need(a, "text");
        q.subject = a.get("subject");
        q.predicate = a.get("predicate");
        if (a.has("valid-from") || a.has("valid-to") || a.has("cond")) q.scope = scope_from(a);
        if (a.has("value-type") || a.has("value") || a.has("lo")) q.value = value_from(a);
        q.domain = a.get("domain");
        q.limit = std::atoi(a.get("limit", "10").c_str());
        print_similar(db.similar(q), a.has("json"));
    } else if (cmd == "reindex-embeddings") {
        std::cout << db.reindex_embeddings() << " vectors written\n";
        auto st = db.embedding_status();
        if (!st.available) std::cerr << "provider unavailable: " << st.last_error << "; " << st.n_missing << " still missing\n";
    } else if (cmd == "confirm") {
        if (a.positional.size() != 2) usage_error("confirm HID --support ID[,ID..] --credence X --method M");
        ConfirmInput in;
        in.hypothesis = a.positional[1];
        in.support = split_ids(need(a, "support"));
        in.credence = std::strtod(need(a, "credence").c_str(), nullptr);
        auto m = credence_method_from(need(a, "method"));
        if (!m) usage_error("unknown method: " + a.get("method"));
        in.method = *m;
        const auto id = db.confirm(in);
        std::cout << id << '\n';
        warn_conflicts(db, id);
    } else if (cmd == "hypotheses") {
        auto hs = db.hypotheses(a.get("domain"), a.has("all"));
        if (a.has("json")) {
            auto arr = json::array();
            for (const auto& h : hs) arr.push_back(h.to_json());
            std::cout << arr.dump(2) << '\n';
        } else {
            for (const auto& h : hs) print_hypothesis(h);
            if (hs.empty()) std::cout << "no hypotheses awaiting review\n";
        }
    } else if (cmd == "predict") {
        if (a.positional.size() != 2) usage_error("predict ID");
        PredictionInput in;
        in.node_id = a.positional[1];
        if (a.has("p")) in.p = std::atof(a.get("p").c_str());
        in.resolve_by = a.get("by");
        in.note = a.get("note");
        std::cout << db.predict(in) << '\n';
    } else if (cmd == "resolve") {
        if (a.positional.size() != 2) usage_error("resolve PID --outcome yes|no");
        const std::string o = a.get("outcome");
        bool outcome = false;
        if (o == "yes" || o == "true" || o == "1") outcome = true;
        else if (o == "no" || o == "false" || o == "0") outcome = false;
        else usage_error("--outcome yes|no");
        db.resolve(a.positional[1], outcome, a.get("note"));
        const auto p = db.prediction(a.positional[1]);
        std::cout << p.id << " p " << p.p << " -> " << (outcome ? "yes" : "no") << "  brier " << *p.brier << '\n';
    } else if (cmd == "predictions") {
        auto ps = db.predictions(a.get("domain"), a.has("all"));
        if (a.has("json")) {
            auto arr = json::array();
            for (const auto& p : ps) arr.push_back(p.to_json());
            std::cout << arr.dump(2) << '\n';
        } else {
            for (const auto& p : ps) {
                std::cout << p.id << "  " << p.node_id << "  p " << p.p << "  " << p.method << (p.rule.empty() ? "" : " [" + p.rule + "]")
                          << "  " << p.made_at.substr(0, 10) << (p.resolve_by.empty() ? "" : " by " + p.resolve_by)
                          << (p.outcome ? std::string("  -> ") + (*p.outcome ? "yes" : "no") + " brier " + std::to_string(*p.brier) : "") << "  " << p.text << '\n';
            }
            if (ps.empty()) std::cout << "no open predictions\n";
        }
    } else if (cmd == "calibration") {
        const auto r = db.calibration(a.get("domain"));
        if (a.has("json")) { std::cout << r.to_json().dump(2) << '\n'; }
        else {
            std::cout << "predictions " << r.predictions << "  resolved " << r.resolved;
            if (r.brier) std::cout << "  brier " << *r.brier;
            std::cout << '\n';
            auto table = [](const char* title, const std::vector<CalibrationBucket>& v) {
                if (v.empty()) return;
                std::cout << title << '\n';
                for (const auto& b : v) std::cout << "  " << b.key << ": n " << b.n << "  forecast " << b.mean_p << "  happened " << b.hit_rate << "  brier " << b.brier << '\n';
            };
            table("by credence step", r.by_credence);
            table("by method", r.by_method);
            table("by rule", r.by_rule);
            table("by attribution", r.by_attribution);
            if (r.resolved == 0) std::cout << "nothing resolved yet: record forecasts with `predict`, settle them with `resolve`\n";
        }
    } else if (cmd == "orphans") {
        const auto os = db.orphans(a.has("months") ? std::atof(a.get("months").c_str()) : 6.0, a.get("domain"));
        if (a.has("json")) {
            auto arr = json::array();
            for (const auto& o : os) arr.push_back(o.to_json());
            std::cout << arr.dump(2) << '\n';
        } else {
            for (const auto& o : os) std::cout << o.id << "  " << static_cast<int>(o.months) << " months  " << o.n_sources << " sources  newest " << o.newest_observed_at.substr(0, 10) << "  " << o.text << '\n';
            if (os.empty()) std::cout << "no orphans\n";
        }
    } else if (cmd == "batch") {
        const std::string sub = a.positional.size() > 1 ? a.positional[1] : "";
        if (sub == "begin") {
            const auto id = db.begin_batch(need(a, "purpose"));
            std::cout << id << '\n';
            std::cerr << "export BELIEF_BATCH=" << id << '\n';
        } else if (sub == "end") {
            std::string id = a.positional.size() > 2 ? a.positional[2] : a.get("batch");
            if (id.empty()) if (const char* env = std::getenv("BELIEF_BATCH")) id = env;
            if (id.empty()) usage_error("batch end ID (or --batch / $BELIEF_BATCH)");
            db.end_batch(id);
            print_batch(db.batch(id));
        } else if (sub == "show") {
            if (a.positional.size() != 3) usage_error("batch show ID");
            auto b = db.batch(a.positional[2]);
            if (a.has("json")) { std::cout << b.to_json().dump(2) << '\n'; }
            else {
                print_batch(b);
                for (const auto& n : b.nodes) {
                    auto node = db.get(n);
                    std::cout << "    " << n << (node && node->status != Status::active ? " (" + std::string(to_string(node->status)) + ")" : "") << '\n';
                }
            }
        } else if (sub == "reject") {
            if (a.positional.size() != 3) usage_error("batch reject ID --reason R");
            db.reject_batch(a.positional[2], need(a, "reason"));
            print_batch(db.batch(a.positional[2]));
        } else {
            usage_error("batch begin|end|show|reject");
        }
    } else if (cmd == "batches") {
        auto bs = db.batches(a.has("all"));
        if (a.has("json")) {
            auto arr = json::array();
            for (const auto& b : bs) arr.push_back(b.to_json());
            std::cout << arr.dump(2) << '\n';
        } else {
            for (const auto& b : bs) print_batch(b);
            if (bs.empty()) std::cout << "no open batches\n";
        }
    } else if (cmd == "predicate") {
        PredicateInput in;
        in.name = need(a, "name");
        auto t = value_type_from(need(a, "type"));
        if (!t) usage_error("unknown value type: " + a.get("type"));
        in.value_type = *t;
        in.unit = a.get("unit");
        in.enum_values = split_ids(a.get("enum-values"));
        in.note = a.get("note");
        db.define_predicate(in);
        std::cout << "ok\n";
    } else if (cmd == "predicates") {
        for (const auto& p : db.list_predicates()) {
            std::cout << p.name << "  " << to_string(p.value_type);
            if (!p.unit.empty()) std::cout << " [" << p.unit << "]";
            if (!p.enum_values.empty()) {
                std::cout << " {";
                for (size_t i = 0; i < p.enum_values.size(); ++i) std::cout << (i ? ", " : "") << p.enum_values[i];
                std::cout << "}";
            }
            if (!p.note.empty()) std::cout << "  " << p.note;
            std::cout << '\n';
        }
    } else if (cmd == "link") {
        if (a.positional.size() != 4) usage_error("link SRC DST TYPE");
        auto t = edge_type_from(a.positional[3]);
        if (!t) usage_error("unknown edge type: " + a.positional[3]);
        std::optional<std::string> rule;
        if (a.has("rule")) rule = a.get("rule");
        db.link(a.positional[1], a.positional[2], *t, rule, a.get("reason"));
        std::cout << "ok\n";
    } else if (cmd == "retract") {
        if (a.positional.size() != 2) usage_error("retract ID --reason R");
        db.retract(a.positional[1], need(a, "reason"));
        std::cout << "ok\n";
    } else if (cmd == "supersede") {
        if (a.positional.size() != 2) usage_error("supersede OLD_ID --reason R <observe|assert|derive options>");
        const auto& old_id = a.positional[1];
        auto old = db.get(old_id);
        if (!old) throw Error(ErrorCode::not_found, "node not found: " + old_id);
        const auto reason = need(a, "reason");
        if (old->type == NodeType::source) std::cout << db.supersede(old_id, source_input(a), reason) << '\n';
        else if (a.has("from")) std::cout << db.supersede(old_id, derive_input(a), reason) << '\n';
        else std::cout << db.supersede(old_id, claim_input(a), reason) << '\n';
    } else if (cmd == "recompute") {
        if (a.positional.size() != 2) usage_error("recompute ID [--credence X]");
        std::optional<double> cr;
        if (a.has("credence")) cr = std::strtod(a.get("credence").c_str(), nullptr);
        db.recompute(a.positional[1], cr);
        auto n = db.get(a.positional[1]);
        std::cout << "ok " << a.positional[1] << fmt_credence(n->credence) << '\n';
    } else if (cmd == "stale") {
        auto st = db.stale(a.get("domain"));
        if (a.has("json")) {
            auto arr = json::array();
            for (const auto& e : st) arr.push_back(e.to_json());
            std::cout << arr.dump(2) << '\n';
        } else {
            for (const auto& e : st) {
                std::cout << e.id << " d" << e.depth << fmt_credence(e.credence) << "  " << e.label << '\n';
                for (const auto& c : e.causes) std::cout << "    because " << c.cause << ": " << c.reason << " (" << c.at.substr(0, 10) << ")\n";
            }
            if (st.empty()) std::cout << "nothing is stale\n";
        }
    } else if (cmd == "what-if") {
        if (a.positional.size() != 2) usage_error("what-if ID --credence X");
        auto r = db.what_if(a.positional[1], std::strtod(need(a, "credence").c_str(), nullptr));
        if (a.has("json")) {
            auto arr = json::array();
            for (const auto& e : r) arr.push_back(e.to_json());
            std::cout << arr.dump(2) << '\n';
        } else {
            for (const auto& e : r) {
                std::cout << e.id << ' ' << to_string(e.method) << ' ' << e.current << " -> ";
                if (e.projected) std::cout << *e.projected; else std::cout << "stale";
                std::cout << "  " << e.label << '\n';
            }
            if (r.empty()) std::cout << "nothing depends on " << a.positional[1] << '\n';
        }
    } else if (cmd == "ask") {
        QuestionInput in;
        in.text = need(a, "text");
        in.why_open = a.get("why-open");
        in.acceptance_criterion = need(a, "criterion");
        in.about = split_ids(a.get("about"));
        in.domain = a.get("domain", "general");
        in.sensitivity = sens_from(a);
        std::cout << db.ask(in) << '\n';
    } else if (cmd == "progress") {
        if (a.positional.size() != 2) usage_error("progress QID --note TEXT");
        db.note_progress(a.positional[1], need(a, "note"));
        std::cout << "ok\n";
    } else if (cmd == "close") {
        if (a.positional.size() != 2) usage_error("close QID --resolution TEXT");
        db.close_question(a.positional[1], need(a, "resolution"));
        std::cout << "ok\n";
    } else if (cmd == "questions") {
        auto qs = db.questions(a.get("domain"), a.has("all"));
        if (a.has("json")) {
            auto arr = json::array();
            for (const auto& q : qs) arr.push_back(q.to_json());
            std::cout << arr.dump(2) << '\n';
        } else {
            for (const auto& q : qs) {
                std::cout << q.id << " (" << to_string(q.status) << ") " << q.domain << "  " << q.text << '\n';
                if (!q.why_open.empty()) std::cout << "    why open: " << q.why_open << '\n';
                std::cout << "    closes when: " << q.acceptance_criterion << '\n';
                if (!q.about.empty()) { std::cout << "    about:"; for (const auto& x : q.about) std::cout << ' ' << x; std::cout << '\n'; }
                if (!q.answers.empty()) { std::cout << "    answers:"; for (const auto& x : q.answers) std::cout << ' ' << x; std::cout << '\n'; }
                for (const auto& p : q.progress) std::cout << "    " << p.at.substr(0, 10) << ' ' << p.actor << ": " << p.text << '\n';
                if (!q.resolution.empty()) std::cout << "    resolution: " << q.resolution << '\n';
            }
            if (qs.empty()) std::cout << "no open questions\n";
        }
    } else if (cmd == "conflicts") {
        auto cs = db.conflicts(a.get("domain"), a.has("all"));
        if (a.has("older-than")) {
            const int days = std::atoi(a.get("older-than").c_str());
            cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const ConflictEntry& c) { return c.triage.open_days < days; }), cs.end());
        }
        if (a.has("json")) {
            auto arr = json::array();
            for (const auto& c : cs) arr.push_back(c.to_json());
            std::cout << arr.dump(2) << '\n';
        } else {
            for (const auto& c : cs) print_conflict(c);
            if (cs.empty()) std::cout << "no conflicts\n";
        }
    } else if (cmd == "dismiss" || cmd == "resolve") {
        if (a.positional.size() != 3) usage_error(cmd + " ID_A ID_B --reason R");
        if (cmd == "dismiss") db.dismiss_conflict(a.positional[1], a.positional[2], need(a, "reason"));
        else db.resolve_conflict(a.positional[1], a.positional[2], need(a, "reason"));
        std::cout << "ok\n";
    } else if (cmd == "refinements") {
        auto rs = db.refinements(a.get("domain"));
        if (a.has("json")) {
            auto arr = json::array();
            for (const auto& r : rs) arr.push_back(r.to_json());
            std::cout << arr.dump(2) << '\n';
        } else {
            for (const auto& r : rs) {
                std::cout << r.subject << " " << r.predicate << ": " << r.narrower.id << " [" << r.narrower.value.describe() << "] is narrower than "
                          << r.wider.id << " [" << r.wider.value.describe() << "]\n"
                          << "    suggested: belief link " << r.narrower.id << " " << r.wider.id << " refines   (or contradicts, if it really is one)\n";
            }
            if (rs.empty()) std::cout << "no refinement candidates\n";
        }
    } else if (cmd == "why") {
        if (a.positional.size() != 2) usage_error("why ID");
        const int depth = a.has("depth") ? std::atoi(a.get("depth").c_str()) : -1;
        auto w = db.why(a.positional[1], depth);
        auto n = db.nutrition(a.positional[1]);
        if (a.has("json")) {
            auto j = w.to_json();
            j["nutrition"] = n.to_json();
            std::cout << j.dump(2) << '\n';
        } else {
            if (w.notice) std::cout << "NOTICE: " << *w.notice << '\n';
            std::cout << fmt_nutrition(n) << '\n';
            print_tree(w, "", true, true);
        }
    } else if (cmd == "nutrition") {
        if (a.positional.size() != 2) usage_error("nutrition ID");
        auto n = db.nutrition(a.positional[1]);
        if (a.has("json")) { std::cout << n.to_json().dump(2) << '\n'; }
        else {
            std::cout << fmt_nutrition(n) << '\n';
            for (const auto& [k, c] : n.by_kind) std::cout << "  " << to_string(k) << ": " << c << '\n';
            std::cout << "  heads:";
            for (const auto& h : n.attributions) std::cout << ' ' << h;
            std::cout << "\n  sources:";
            for (const auto& sid : n.sources) std::cout << ' ' << sid;
            std::cout << '\n';
        }
    } else if (cmd == "show") {
        if (a.positional.size() != 2) usage_error("show ID");
        auto n = db.get(a.positional[1]);
        if (!n) throw Error(ErrorCode::not_found, "node not found: " + a.positional[1]);
        json j{{"id", n->id},         {"type", to_string(n->type)},   {"domain", n->domain},
               {"status", to_string(n->status)}, {"sensitivity", to_string(n->sensitivity)},
               {"created_at", n->created_at}, {"updated_at", n->updated_at}, {"payload", n->payload}};
        if (n->credence)
            j["credence"] = {{"value", n->credence->value}, {"method", to_string(n->credence->method)},
                             {"inputs", n->credence->inputs}, {"computed_at", n->credence->computed_at}, {"stale", n->credence->stale}};
        auto edges = json::array();
        for (const auto& e : db.edges_from(n->id)) edges.push_back({{"dir", "out"}, {"type", to_string(e.type)}, {"to", e.dst}, {"rule", e.rule_id ? json(*e.rule_id) : json()}});
        for (const auto& e : db.edges_to(n->id)) edges.push_back({{"dir", "in"}, {"type", to_string(e.type)}, {"from", e.src}, {"rule", e.rule_id ? json(*e.rule_id) : json()}});
        j["edges"] = edges;
        std::cout << j.dump(2) << '\n';
    } else if (cmd == "stats") {
        auto k = db.counts();
        std::cout << "nodes " << k.nodes << "  sources " << k.sources << "  claims " << k.claims << "  hypotheses " << k.hypotheses
                  << "  rules " << k.rules << "  edges " << k.edges << "  open batches " << k.open_batches << '\n';
        auto t = db.triage_stats();
        std::cout << "conflicts: open " << t.open << "  promoted " << t.promoted << "  resolved " << t.resolved << "  dismissed " << t.dismissed
                  << "  vanished " << t.vanished;
        if (t.precision) std::cout << "  detector precision " << *t.precision;
        std::cout << '\n';
        auto es = db.embedding_status();
        if (es.configured)
            std::cout << "embeddings: " << es.model << " dim " << es.dim << ", " << es.n_vectors << " vectors, " << es.n_missing << " missing"
                      << (es.available ? "" : ", provider unavailable: " + es.last_error) << '\n';
        else
            std::cout << "embeddings: not configured (full-text only)\n";
    } else {
        usage_error("unknown command: " + cmd);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse(argc, argv));
    } catch (const Error& e) {
        std::cerr << "error[" << to_string(e.code) << "]: " << e.what() << '\n';
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 3;
    }
}
