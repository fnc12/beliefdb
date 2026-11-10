// Ф5 (3/3): the HTTP service: actors and tokens, own-domain writes, sensitivity, batches per request, error mapping.
#include "server.hpp"

#include <doctest/doctest.h>

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

#include <map>

using namespace beliefdb;
using nlohmann::json;

namespace {
const std::map<std::string, std::string> kEnv{{"BELIEFD_TOKEN_OWNER", "tok-human"}, {"BELIEFD_UI_PASSWORD", "ui-secret"}, {"BELIEFD_TOKEN_HEALTH", "tok-health"}, {"BELIEFD_TOKEN_MONEY", "tok-money"}};
std::string fake_getenv(const std::string& k) { auto it = kEnv.find(k); return it == kEnv.end() ? "" : it->second; }

json base_config() {
    return json::parse(R"({
      "db": ":memory:", "listen": "127.0.0.1", "port": 0,
      "policy": {"require_batch": true, "batch_claims": 2},
      "actors": [
        {"name": "owner", "human": true, "daily_claims": 500, "daily_hypotheses": 40},
        {"name": "health", "domains": ["health"]},
        {"name": "money", "domains": ["money"]},
        {"name": "no-token", "domains": ["x"]}
      ]
    })");
}

struct Client {
    httplib::Client cli;
    std::string token, batch;
    Client(int port, std::string tok) : cli("127.0.0.1", port), token(std::move(tok)) { cli.set_connection_timeout(std::chrono::seconds(2)); }
    httplib::Headers headers() const {
        httplib::Headers h{{"Authorization", "Bearer " + token}};
        if (!batch.empty()) h.emplace("X-Belief-Batch", batch);
        return h;
    }
    std::pair<int, json> post(const std::string& path, const json& body) {
        auto r = cli.Post(path, headers(), body.dump(), "application/json");
        REQUIRE(r);
        return {r->status, json::parse(r->body, nullptr, false)};
    }
    std::pair<int, json> get(const std::string& path) {
        auto r = cli.Get(path, headers());
        REQUIRE(r);
        return {r->status, json::parse(r->body, nullptr, false)};
    }
};

struct Running {
    beliefd::Config cfg = beliefd::Config::from_json(base_config(), fake_getenv);
    beliefd::Server server{cfg};
    int port = server.start();
    Client human{port, "tok-human"}, health{port, "tok-health"}, money{port, "tok-money"};
};
}  // namespace

TEST_CASE("config: tokens come from the environment only; actors without one are dropped") {
    auto j = base_config();
    j["actors"][1]["token"] = "in-the-file";
    auto cfg = beliefd::Config::from_json(j, fake_getenv);
    REQUIRE(cfg.actors.size() == 3);
    CHECK(cfg.actors[1].name == "health");
    CHECK(cfg.actors[1].token == "tok-health");
    CHECK(cfg.actors[0].human);
    CHECK(cfg.policy.require_batch);
    CHECK(cfg.policy.batch_claims == 2);
    CHECK(cfg.policy.daily_claims == 50);  // untouched default
    CHECK(cfg.actors[0].daily_claims == 500);  // per-actor override
    CHECK(cfg.actors[0].daily_hypotheses == 40);
    CHECK(!cfg.actors[1].daily_claims);
    CHECK(cfg.weights.order_factor == doctest::Approx(0.95));  // default without a "nutrition" section
    j["nutrition"] = {{"order_factor", 0.9}, {"document", 0.7}};
    auto tuned = beliefd::Config::from_json(j, fake_getenv);
    CHECK(tuned.weights.order_factor == doctest::Approx(0.9));
    CHECK(tuned.weights.document == doctest::Approx(0.7));
    CHECK(tuned.weights.measurement == doctest::Approx(1.0));
    j["nutrition"]["order_factor"] = 1.5;
    auto bad = beliefd::Config::from_json(j, fake_getenv);
    CHECK(bad.weights.order_factor == doctest::Approx(0.95));
    bool out_of_range = false;
    for (const auto& w : bad.warnings) out_of_range |= w.find("order_factor") != std::string::npos;
    CHECK(out_of_range);
    bool dropped = false, scolded = false;
    for (const auto& w : cfg.warnings) { dropped |= w.find("no-token") != std::string::npos; scolded |= w.find("BELIEFD_TOKEN_HEALTH") != std::string::npos; }
    CHECK(dropped);
    CHECK(scolded);
    CHECK(beliefd::in_quiet_hours("23:00-08:00", 23 * 60 + 30));
    CHECK(beliefd::in_quiet_hours("23:00-08:00", 3 * 60));
    CHECK_FALSE(beliefd::in_quiet_hours("23:00-08:00", 12 * 60));
    CHECK(beliefd::in_quiet_hours("01:00-02:00", 90));
    CHECK_FALSE(beliefd::in_quiet_hours("garbage", 90));
}

TEST_CASE("http: authentication, own-domain writes, batches per request, error mapping") {
    Running r;
    // no or wrong token
    httplib::Client anon("127.0.0.1", r.port);
    auto res = anon.Get("/health");
    REQUIRE(res);
    CHECK(res->status == 401);
    res = anon.Get("/health", httplib::Headers{{"Authorization", "Bearer nope"}});
    CHECK(res->status == 401);
    auto [hs, hj] = r.human.get("/health");
    CHECK(hs == 200);
    CHECK(hj["ok"] == true);
    CHECK(hj["embeddings"]["configured"] == false);
    CHECK(r.health.get("/whoami").second["domains"] == json::array({"health"}));

    // require_batch: an agent writes nothing, not even a source, outside a batch
    json source{{"kind", "document"}, {"uri", "visit"}, {"content", "ибупрофен 400"}, {"observed_at", "2026-10-10"}, {"domain", "health"}};
    auto [s0, j0] = r.health.post("/observe", source);
    CHECK(s0 == 409);
    CHECK(j0["error"] == "no_batch");
    auto [s4, j4] = r.health.post("/batch/begin", {{"purpose", "разбираю визит"}});
    REQUIRE(s4 == 200);
    r.health.batch = j4["id"];
    // an agent writes only to its domains
    auto [s1, j1] = r.health.post("/observe", source);
    REQUIRE(s1 == 200);
    const std::string src = j1["id"];
    auto [s2, j2] = r.health.post("/observe", {{"kind", "document"}, {"content", "x"}, {"domain", "money"}});
    CHECK(s2 == 403);
    CHECK(j2["error"] == "forbidden");

    json claim{{"text", "ибупрофен 400 мг"}, {"subject", "me"}, {"predicate", "ibuprofen_dose"}, {"value", {{"type", "number"}, {"value", 400}, {"unit", "mg"}}},
               {"support", json::array({src})}, {"credence", 0.8}, {"method", "stated"}, {"domain", "health"}};
    auto [s5, j5] = r.health.post("/assert", claim);
    REQUIRE(s5 == 200);
    const std::string c1 = j5["id"];
    CHECK(j5["conflicts"].empty());
    CHECK(j5["search_mode"] == "fts");
    // duplicate -> 409 with the id named
    auto [s6, j6] = r.health.post("/assert", claim);
    CHECK(s6 == 409);
    CHECK(j6["error"] == "duplicate_claim");
    CHECK(j6["message"].get<std::string>().find(c1) != std::string::npos);
    // a contradiction is written and reported back
    json other = claim;
    other["text"] = "ибупрофен 200 мг";
    other["value"]["value"] = 200;
    auto [s7, j7] = r.health.post("/assert", other);
    REQUIRE(s7 == 200);
    REQUIRE(j7["conflicts"].size() == 1);
    CHECK(j7["conflicts"][0]["id"] == c1);
    // batch quota (2 claims) -> 429, the batch is closed, the header now yields 409
    other["text"] = "ибупрофен 300 мг";
    other["value"]["value"] = 300;
    auto [s8, j8] = r.health.post("/assert", other);
    CHECK(s8 == 429);
    CHECK(j8["error"] == "quota_exceeded");
    auto [s9, j9] = r.health.post("/assert", other);
    CHECK(s9 == 409);
    CHECK(j9["error"] == "no_batch");
    CHECK(r.health.get("/batch/" + r.health.batch).second["closed_reason"] == "quota: 2 claims");
    // another actor may not see that batch; the human may, and may reject it
    CHECK(r.money.get("/batch/" + r.health.batch).first == 403);
    CHECK(r.money.post("/batch/reject", {{"id", r.health.batch}, {"reason", "x"}}).first == 403);
    auto [s10, j10] = r.human.post("/batch/reject", {{"id", r.health.batch}, {"reason", "мусор"}});
    CHECK(s10 == 200);
    CHECK(r.human.get("/node/" + c1).second["status"] == "retracted");
    r.health.batch.clear();
    // humans write without a batch (the rejected batch took its source down too, so a fresh one)
    claim["support"] = json::array({r.human.post("/observe", {{"kind", "document"}, {"content", "visit again"}, {"observed_at", "2026-10-11"}, {"domain", "health"}}).second["id"]});
    auto [s11, j11] = r.human.post("/assert", claim);
    CHECK_MESSAGE(s11 == 200, j11.dump());
    // not found and bad input
    CHECK(r.human.get("/node/c_nope").first == 404);
    CHECK(r.human.post("/assert", {{"text", "x"}}).first == 400);
    auto raw = r.human.cli.Post("/assert", r.human.headers(), "not json", "application/json");
    REQUIRE(raw);
    CHECK(raw->status == 400);
}

TEST_CASE("http: sensitivity — private nodes of another domain are invisible, domain ones are readable and usable") {
    Running r;
    auto src = r.human.post("/observe", {{"kind", "document"}, {"content", "x"}, {"observed_at", "2026-10-01"}, {"domain", "health"}}).second["id"].get<std::string>();
    auto priv = r.human.post("/assert", {{"text", "диагноз"}, {"subject", "me"}, {"predicate", "dx"}, {"value", "secret"}, {"support", json::array({src})}, {"credence", 0.8},
                                         {"domain", "health"}, {"sensitivity", "private"}}).second["id"].get<std::string>();
    auto open = r.human.post("/assert", {{"text", "вес 86"}, {"subject", "me"}, {"predicate", "weight"}, {"value", {{"type", "number"}, {"value", 86}, {"unit", "kg"}}},
                                         {"support", json::array({src})}, {"credence", 0.8}, {"domain", "health"}}).second["id"].get<std::string>();
    auto rule = r.human.post("/rule", {{"name", "r"}, {"domain", "general"}}).second["id"].get<std::string>();
    auto [ds, dj] = r.human.post("/derive", {{"text", "из диагноза"}, {"from", json::array({priv})}, {"rule", "r"}, {"domain", "health"}});
    REQUIRE(ds == 200);
    const std::string from_priv = dj["id"];

    // health sees its own private claim, money does not
    CHECK(r.health.get("/node/" + priv).first == 200);
    CHECK(r.money.get("/node/" + priv).first == 403);
    CHECK(r.money.get("/node/" + open).first == 200);
    // search and lists are filtered ("диагноз" is a substring of the derived claim's text too)
    auto found = r.money.get("/search?q=диагноз").second["entries"];
    REQUIRE(found.size() == 1);
    CHECK(found[0]["id"] == from_priv);
    CHECK(r.human.get("/search?q=диагноз").second["entries"].size() == 2);
    // why() across domains: the private subtree is a stub
    auto why = r.money.get("/why/" + from_priv).second;
    REQUIRE(why["children"].size() == 1);
    CHECK(why["children"][0]["label"].get<std::string>().find("private") != std::string::npos);
    CHECK(why["children"][0]["children"].empty());
    CHECK(r.health.get("/why/" + from_priv).second["children"][0]["label"] == "диагноз");
    // money may build on the open health claim (cross-domain justification is an edge), not on the private one
    r.money.batch = r.money.post("/batch/begin", {{"purpose", "cross"}}).second["id"];
    auto [cs, cj] = r.money.post("/derive", {{"text", "страховка от веса"}, {"from", json::array({open})}, {"rule", "r"}, {"domain", "money"}});
    CHECK(cs == 200);
    CHECK(r.money.post("/derive", {{"text", "x"}, {"from", json::array({priv})}, {"rule", "r"}, {"domain", "money"}}).first == 403);
    // and may not change health's nodes
    CHECK(r.money.post("/retract", {{"id", open}, {"reason", "x"}}).first == 403);
    CHECK(r.health.post("/retract", {{"id", open}, {"reason", "x"}}).first == 200);
    (void)rule;
}

TEST_CASE("http: quiet hours keep the embedding GPU asleep") {
    auto j = base_config();
    j["embeddings"] = {{"base_url", "http://127.0.0.1:1"}, {"model", "bge-m3"}, {"quiet_hours", "00:00-23:59"}};
    beliefd::Config cfg = beliefd::Config::from_json(j, fake_getenv);
    beliefd::Server server{cfg};
    const int port = server.start();
    Client human{port, "tok-human"};
    auto [s, h] = human.get("/health");
    REQUIRE(s == 200);
    CHECK(h["quiet_now"] == true);
    CHECK(h["embeddings"]["configured"] == false);  // switched off for the night, no call is ever made
    CHECK(human.post("/reindex-embeddings", json::object()).first == 409);
}

TEST_CASE("ui: the owner reads through the web page with the UI password; Basic auth is GET only; domains and paged listings") {
    Running r;
    httplib::Client anon("127.0.0.1", r.port);
    auto page = anon.Get("/ui");
    REQUIRE(page);
    CHECK(page->status == 401);
    CHECK(page->get_header_value("WWW-Authenticate").rfind("Basic realm=", 0) == 0);
    // any user name, the password decides
    httplib::Headers basic{{"Authorization", "Basic " + httplib::detail::base64_encode("owner:ui-secret")}};
    httplib::Headers wrong{{"Authorization", "Basic " + httplib::detail::base64_encode("owner:nope")}};
    page = anon.Get("/ui", basic);
    REQUIRE(page);
    CHECK(page->status == 200);
    CHECK(page->get_header_value("Content-Type").rfind("text/html", 0) == 0);
    CHECK(page->body.find("<title>beliefdb</title>") != std::string::npos);
    CHECK(anon.Get("/ui", wrong)->status == 401);
    auto root = anon.Get("/");
    REQUIRE(root);
    CHECK(root->status == 302);
    CHECK(root->get_header_value("Location") == "/ui");
    // reads yes, writes no
    auto h = anon.Get("/health", basic);
    REQUIRE(h);
    CHECK(h->status == 200);
    auto w = anon.Post("/observe", basic, json{{"kind", "document"}, {"content", "x"}, {"domain", "health"}}.dump(), "application/json");
    REQUIRE(w);
    CHECK(w->status == 401);
    // the shape of the store: counts per domain, listings newest first and paged
    auto [s1, j1] = r.human.post("/observe", {{"kind", "document"}, {"uri", "visit"}, {"content", "ибупрофен 400"}, {"attribution", "врач"}, {"observed_at", "2026-10-10"}, {"domain", "health"}});
    REQUIRE(s1 == 200);
    const std::string src = j1["id"];
    for (int i = 0; i < 3; ++i) {
        auto [s2, j2] = r.human.post("/assert", {{"text", "утверждение " + std::to_string(i)}, {"subject", "me"}, {"predicate", "p" + std::to_string(i)}, {"value", {{"type", "number"}, {"value", i}}},
                                                 {"support", json::array({src})}, {"credence", 0.8}, {"method", "stated"}, {"domain", "health"}});
        REQUIRE(s2 == 200);
    }
    auto [s3, j3] = r.human.post("/observe", {{"kind", "document"}, {"content", "деньги"}, {"domain", "money"}});
    REQUIRE(s3 == 200);
    auto d = anon.Get("/domains", basic);
    REQUIRE(d);
    const json dj = json::parse(d->body);
    REQUIRE(dj["entries"].size() == 2);
    CHECK(dj["entries"][0]["domain"] == "health");
    CHECK(dj["entries"][0]["claims"] == 3);
    CHECK(dj["entries"][0]["sources"] == 1);
    CHECK(dj["entries"][1]["domain"] == "money");
    CHECK(dj["entries"][1]["sources"] == 1);
    CHECK(dj["entries"][1]["claims"] == 0);
    auto l = anon.Get("/nodes?type=claim&domain=health&limit=2", basic);
    REQUIRE(l);
    json lj = json::parse(l->body);
    CHECK(lj["total"] == 3);
    REQUIRE(lj["entries"].size() == 2);
    CHECK(lj["entries"][0]["label"] == "утверждение 2");  // newest first
    CHECK(lj["entries"][0]["credence"]["value"] == 0.8);
    l = anon.Get("/nodes?type=claim&domain=health&limit=2&offset=2", basic);
    lj = json::parse(l->body);
    REQUIRE(lj["entries"].size() == 1);
    CHECK(lj["entries"][0]["label"] == "утверждение 0");
    l = anon.Get("/nodes?type=source", basic);
    lj = json::parse(l->body);
    CHECK(lj["total"] == 2);
    CHECK(lj["entries"][1]["label"] == "document: visit");
    CHECK(lj["entries"][1]["payload"]["attribution"] == "врач");
    // without the password there is no page at all, and Basic auth is not an entry
    auto cfg = beliefd::Config::from_json(base_config(), [](const std::string& k) { return k == "BELIEFD_UI_PASSWORD" ? "" : fake_getenv(k); });
    beliefd::Server quiet{cfg};
    httplib::Client c2("127.0.0.1", quiet.start());
    CHECK(c2.Get("/ui", basic)->status == 404);
    CHECK(c2.Get("/health", basic)->status == 401);
    CHECK(c2.Get("/health", basic)->get_header_value("WWW-Authenticate").empty());
}
