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

usage: belief [--db PATH] [--actor NAME] <command> [options]

  init                                   create the database
  observe  --kind K --uri U (--content C | --file F) [--observed-at T] [--attribution WHO] [--trust-hint X]
           [--directness primary|secondary] [--domain D] [--sensitivity S]
           K: measurement|document|human_statement|llm_output; secondary = a retelling of somebody else's conclusion
  rule     --name N [--description D] [--domain D]
  predicate --name N --type T [--unit U] [--enum-values A,B,..] [--note TEXT]     T: bool|number|enum|ref|text
  predicates                             list the registry
  assert   --text T --support ID[,ID..] --credence X --method M [<scope>] [--subject S] [--predicate P] [<value>]
           [--domain D] [--sensitivity S]
  derive   --text T --from ID[,ID..] --rule R [--credence X] [--method M] [<scope>] [--subject S] [--predicate P] [<value>]
           [--domain D]
  link     SRC DST TYPE [--rule R]        TYPE: supports|contradicts|derives_from|assumes|answers|refines|replaces
                                          (contradicts is symmetric; replaces: SRC is the new node, DST the old)
  why      ID [--depth N] [--json]        justification tree; the header line is the node's nutrition
  nutrition ID [--json]                   what the node stands on: sources, heads, weight, concentration
  show     ID [--json]
  stats

  <scope>: --valid-from T, --valid-to T, --cond key=value (repeatable), --scope-note TEXT
           assert: valid_from defaults to the earliest source's observed_at
           derive: scope is the intersection of the premises', the flags may only narrow it
  <value>: --value-type bool|number|enum|ref|text (default text) and --value V,
           for number either --value X or --lo X --hi Y, plus --unit U (kg/g/mg, m/cm, s/min/h/d, %, USD/KZT/EUR/..)
  credence: 0.05 0.2 0.35 0.5 0.65 0.8 0.95
  method:   stated measured llm_judgment manual (assert); min rule noisy_or llm_judgment manual (derive)
  DB path:  --db, else $BELIEF_DB, else ./beliefdb.sqlite
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
    static const std::vector<std::string> flags{"json"};
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
    line += fmt_credence(w.credence);
    line += "  " + w.label;
    if (w.truncated) line += "  …";
    std::cout << line << '\n';
    const std::string child_prefix = root ? "" : prefix + (last ? "   " : "│  ");
    for (size_t i = 0; i < w.children.size(); ++i)
        print_tree(w.children[i], child_prefix, i + 1 == w.children.size(), false);
}

int run(const Args& a) {
    if (a.positional.empty()) usage_error("missing command");
    const std::string cmd = a.positional[0];
    std::string db_path = a.get("db");
    if (db_path.empty()) {
        if (const char* env = std::getenv("BELIEF_DB")) db_path = env;
        else db_path = "beliefdb.sqlite";
    }
    auto db = Store::open(db_path, a.get("actor", "cli"));

    if (cmd == "init") {
        std::cout << "ok " << db_path << '\n';
    } else if (cmd == "observe") {
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
        std::cout << db.observe(in) << '\n';
    } else if (cmd == "rule") {
        std::cout << db.define_rule({need(a, "name"), a.get("description"), a.get("domain", "general")}) << '\n';
    } else if (cmd == "assert") {
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
        std::cout << db.assert_claim(in) << '\n';
    } else if (cmd == "derive") {
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
        std::cout << db.derive(in) << '\n';
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
        db.link(a.positional[1], a.positional[2], *t, rule);
        std::cout << "ok\n";
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
        std::cout << "nodes " << k.nodes << "  sources " << k.sources << "  claims " << k.claims << "  rules " << k.rules
                  << "  edges " << k.edges << '\n';
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
