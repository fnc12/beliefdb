#include <beliefdb/store.hpp>
#include <doctest/doctest.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>

using namespace beliefdb;

TEST_CASE("nutrition: retellings of one source stay one source; derivation adds nothing") {
    auto db = Store::open(":memory:", "test");
    auto rule = db.define_rule({.name = "r"});
    auto s = db.observe({.kind = SourceKind::document, .uri = "prospectus", .content = "coupon 18%", .observed_at = "2026-09-01", .attribution = "KASE"});
    auto a = db.assert_claim({.text = "A", .support = {s}, .credence = 0.8});
    auto b = db.assert_claim({.text = "B", .support = {s}, .credence = 0.8});
    auto c = db.assert_claim({.text = "C", .support = {s}, .credence = 0.8});
    auto d = db.derive({.text = "D", .from = {a, b, c}, .rule = rule});
    auto n = db.nutrition(d);
    CHECK(n.n_sources == 1);
    CHECK(n.n_attributions == 1);
    CHECK(n.sources == std::vector<std::string>{s});
    CHECK(n.depth == 2);
    CHECK(n.weighted == doctest::Approx(0.8));
    CHECK(n.concentration == doctest::Approx(1.0));

    // a five-step chain on the same base: calories constant, depth and dilution grow
    std::string cur = d;
    for (int i = 0; i < 5; ++i) cur = db.derive({.text = "chain", .from = {cur}, .rule = rule});
    auto deep = db.nutrition(cur);
    CHECK(deep.n_sources == 1);
    CHECK(deep.weighted == doctest::Approx(0.8));
    CHECK(deep.depth == 7);
    CHECK(deep.dilution == doctest::Approx(7.0));
    CHECK(db.nutrition(a).depth == 1);
    CHECK(db.nutrition(s).depth == 0);
    CHECK(db.nutrition(s).n_sources == 1);
}

TEST_CASE("nutrition: three posts quoting one doctor are three sources but one head") {
    auto db = Store::open(":memory:", "test");
    auto p1 = db.observe({.kind = SourceKind::document, .uri = "tg:1", .content = "the doctor said to drink water", .observed_at = "2026-10-01", .attribution = "Dr. Ivanov"});
    auto p2 = db.observe({.kind = SourceKind::document, .uri = "tg:2", .content = "Ivanov: drink water", .observed_at = "2026-10-02", .attribution = "Dr. Ivanov"});
    auto p3 = db.observe({.kind = SourceKind::human_statement, .uri = "wife", .content = "he did say, water", .observed_at = "2026-10-03", .attribution = "Dr. Ivanov"});
    auto c = db.assert_claim({.text = "drink water", .support = {p1, p2, p3}, .credence = 0.65});
    auto n = db.nutrition(c);
    CHECK(n.n_sources == 3);
    CHECK(n.n_attributions == 1);
    CHECK(n.attributions == std::vector<std::string>{"Dr. Ivanov"});
    CHECK(n.n_unattributed == 0);
    CHECK(n.weighted == doctest::Approx(0.8 + 0.8 + 0.5));
    CHECK(n.concentration == doctest::Approx(0.8 / 2.1));
    CHECK(n.oldest_observed_at == "2026-10-01");
    CHECK(n.newest_observed_at == "2026-10-03");
    REQUIRE(n.by_kind.size() == 2);
}

TEST_CASE("nutrition: unattributed sources count as their own heads; weights are configurable") {
    auto db = Store::open(":memory:", "test");
    auto m = db.observe({.kind = SourceKind::measurement, .uri = "scale", .content = "86", .observed_at = "2026-10-16"});
    auto l = db.observe({.kind = SourceKind::llm_output, .uri = "claude", .content = "probably 85", .observed_at = "2026-10-16"});
    auto c = db.assert_claim({.text = "weight", .support = {m, l}, .credence = 0.8, .method = CredenceMethod::measured});
    auto n = db.nutrition(c);
    CHECK(n.n_sources == 2);
    CHECK(n.n_attributions == 2);
    CHECK(n.n_unattributed == 2);
    CHECK(n.weighted == doctest::Approx(1.1));
    CHECK(n.concentration == doctest::Approx(1.0 / 1.1));
    NutritionWeights w;
    w.llm_output = 0.0;
    db.set_weights(w);
    CHECK(db.nutrition(c).weighted == doctest::Approx(1.0));
    CHECK(db.nutrition(c).concentration == doctest::Approx(1.0));
}

TEST_CASE("nutrition: a new supporting edge propagates downstream") {
    auto db = Store::open(":memory:", "test");
    auto rule = db.define_rule({.name = "r"});
    auto s1 = db.observe({.kind = SourceKind::document, .uri = "1", .content = "one", .observed_at = "2026-01-01", .attribution = "x"});
    auto s2 = db.observe({.kind = SourceKind::measurement, .uri = "2", .content = "two", .observed_at = "2026-01-02", .attribution = "y"});
    auto a = db.assert_claim({.text = "A", .support = {s1}, .credence = 0.8});
    auto b = db.derive({.text = "B", .from = {a}, .rule = rule});
    auto c = db.derive({.text = "C", .from = {b}, .rule = rule});
    CHECK(db.nutrition(c).n_sources == 1);
    db.link(s2, a, EdgeType::supports);
    auto n = db.nutrition(c);
    CHECK(n.n_sources == 2);
    CHECK(n.n_attributions == 2);
    CHECK(n.weighted == doctest::Approx(1.8));
    CHECK(db.dependents_of(a).size() == 2);
    // a derives_from edge added later also propagates
    auto s3 = db.observe({.kind = SourceKind::document, .uri = "3", .content = "three", .observed_at = "2026-01-03", .attribution = "z"});
    auto d = db.assert_claim({.text = "D", .support = {s3}, .credence = 0.8});
    db.link(b, d, EdgeType::derives_from, rule);
    CHECK(db.nutrition(c).n_sources == 3);
    CHECK(db.nutrition(b).n_sources == 3);
    CHECK(db.nutrition(a).n_sources == 2);
    auto j = n.to_json();
    CHECK(j["n_sources"] == 2);
    CHECK(j["by_kind"]["measurement"] == 1);
}

TEST_CASE("nutrition: a secondary source weighs half of its kind; llm_output ignores directness") {
    auto db = Store::open(":memory:", "test");
    // "our analysis shows X" with no visible evidence: for us it is a document we take on faith
    auto report = db.observe({.kind = SourceKind::document, .directness = Directness::secondary, .uri = "report.pdf",
                              .content = "our analysis shows X", .observed_at = "2026-10-01", .attribution = "agency"});
    auto retold = db.observe({.kind = SourceKind::human_statement, .directness = Directness::secondary, .uri = "tg:friend",
                              .content = "a friend retold the report", .observed_at = "2026-10-02", .attribution = "friend"});
    auto llm = db.observe({.kind = SourceKind::llm_output, .directness = Directness::secondary, .uri = "claude",
                           .content = "most likely X", .observed_at = "2026-10-03"});
    auto c = db.assert_claim({.text = "X", .support = {report, retold, llm}, .credence = 0.5});
    auto n = db.nutrition(c);
    CHECK(n.n_sources == 3);
    CHECK(n.n_secondary == 2);
    CHECK(n.weighted == doctest::Approx(0.4 + 0.25 + 0.1));
    CHECK(n.concentration == doctest::Approx(0.4 / 0.75));
    CHECK(n.to_json()["n_secondary"] == 2);
    CHECK(db.get(report)->payload["directness"] == "secondary");
    CHECK(db.get(llm)->payload["directness"] == "secondary");  // stored as given, just not weighed

    NutritionWeights w;
    CHECK(w.of(SourceKind::measurement, Directness::secondary) == doctest::Approx(0.5));
    CHECK(w.of(SourceKind::llm_output, Directness::secondary) == doctest::Approx(0.1));
}

TEST_CASE("nutrition: every order of a hypothesis tower costs 5% of the leaves' calories; the factor is configurable") {
    auto db = Store::open(":memory:", "test");
    auto rule = db.define_rule({.name = "r"});
    auto s1 = db.observe({.kind = SourceKind::measurement, .uri = "m1", .content = "x", .observed_at = "2026-09-01", .attribution = "lab"});
    auto s2 = db.observe({.kind = SourceKind::document, .uri = "d1", .content = "y", .observed_at = "2026-09-02", .attribution = "paper"});
    auto a = db.assert_claim({.text = "A", .support = {s1}, .credence = 0.8});
    auto b = db.assert_claim({.text = "B", .support = {s2}, .credence = 0.8});
    auto h1 = db.hypothesize({.text = "H1", .from = {a, b}, .rule = rule});
    auto h2 = db.hypothesize({.text = "H2", .from = {h1}, .rule = rule});
    auto h3 = db.hypothesize({.text = "H3", .from = {h2, a}, .rule = rule});

    auto n1 = db.nutrition(h1);
    CHECK(n1.order == 1);
    CHECK(n1.order_multiplier == doctest::Approx(1.0));
    CHECK(n1.weighted == doctest::Approx(1.8));  // the leaves as they are: a first-order hypothesis pays nothing
    auto n2 = db.nutrition(h2);
    CHECK(n2.order == 2);
    CHECK(n2.n_sources == 2);  // same leaves
    CHECK(n2.order_multiplier == doctest::Approx(0.95));
    CHECK(n2.weighted == doctest::Approx(1.8 * 0.95));
    CHECK(n2.concentration == doctest::Approx(1.0 / 1.8));  // the share is of the raw sum, the penalty does not move it
    auto n3 = db.nutrition(h3);
    CHECK(n3.order == 3);
    CHECK(n3.weighted == doctest::Approx(1.8 * 0.95 * 0.95));
    CHECK(n3.to_json()["order"] == 3);
    CHECK(n3.to_json()["order_multiplier"] == doctest::Approx(0.9025));
    // claims and sources carry no order
    CHECK(db.nutrition(a).order == 0);
    CHECK(db.nutrition(a).order_multiplier == doctest::Approx(1.0));
    CHECK(db.nutrition(a).weighted == doctest::Approx(1.0));

    NutritionWeights w;
    w.order_factor = 0.5;
    db.set_weights(w);
    CHECK(db.nutrition(h3).weighted == doctest::Approx(1.8 * 0.25));
    CHECK(db.nutrition(h1).weighted == doctest::Approx(1.8));
    CHECK(w.order_multiplier(0) == doctest::Approx(1.0));
    CHECK(w.order_multiplier(1) == doctest::Approx(1.0));
    CHECK(w.order_multiplier(4) == doctest::Approx(0.125));
}

TEST_CASE("migration: a hypothesis written before `order` existed gets one on open, from its premises") {
    const std::string path = (std::filesystem::temp_directory_path() /
                              ("beliefdb-order-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite")).string();
    std::string h1, h2, h3;
    {
        auto db = Store::open(path, "test");
        auto rule = db.define_rule({.name = "r"});
        auto s = db.observe({.kind = SourceKind::document, .uri = "u", .content = "x", .observed_at = "2026-09-01"});
        auto a = db.assert_claim({.text = "A", .support = {s}, .credence = 0.8});
        h1 = db.hypothesize({.text = "H1", .from = {a}, .rule = rule});
        h2 = db.hypothesize({.text = "H2", .from = {h1}, .rule = rule});
        h3 = db.hypothesize({.text = "H3", .from = {h2}, .rule = rule});
    }
    {   // the old world: no `order` anywhere in the payloads
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw, "UPDATE nodes SET payload = json_remove(payload, '$.order') WHERE type = 'hypothesis'", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw);
    }
    {
        auto db = Store::open(path, "test");
        CHECK(db.get(h1)->payload["order"] == 1);
        CHECK(db.get(h2)->payload["order"] == 2);
        CHECK(db.get(h3)->payload["order"] == 3);
        CHECK(db.hypothesis(h3).order == 3);
        CHECK(db.nutrition(h3).weighted == doctest::Approx(0.8 * 0.95 * 0.95));
        // idempotent: a second open changes nothing
        auto before = db.get(h3)->payload.dump();
        db = Store::open(path, "test");
        CHECK(db.get(h3)->payload.dump() == before);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST_CASE("observe: there is no derived kind, and a source may not wrap a node of this store") {
    auto db = Store::open(":memory:", "test");
    CHECK(!source_kind_from("derived"));
    CHECK(source_kind_from("document").has_value());

    auto s = db.observe({.kind = SourceKind::document, .uri = "u", .content = "x", .observed_at = "2026-10-01", .domain = "health"});
    auto c = db.assert_claim({.text = "water norm 2 l", .support = {s}, .credence = 0.65, .domain = "health"});

    // the travel domain wants to lean on the health conclusion: that is an edge, not a source
    try { db.observe({.kind = SourceKind::document, .uri = "belief:" + c, .content = "water norm 2 l (from the health domain)", .domain = "travel"}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::bridge_source); CHECK(std::string(e.what()).find(c) != std::string::npos); }
    try { db.observe({.kind = SourceKind::document, .uri = "note", .content = "see " + c + ", it is all there", .domain = "travel"}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::bridge_source); }
    // the right way: derives_from across the domain border keeps provenance intact
    auto rule = db.define_rule({.name = "pack", .domain = "travel"});
    auto t = db.derive({.text = "take 2 l of water a day", .from = {c}, .rule = rule, .domain = "travel"});
    CHECK(db.nutrition(t).sources == std::vector<std::string>{s});

    // an id-shaped token that is not a node here is ordinary text
    CHECK_NOTHROW(db.observe({.kind = SourceKind::document, .uri = "y", .content = "c_01ARZ3NDEKTSV4RRFFQ69G5FAV", .observed_at = "2026-10-01"}));
    // and an embedded id inside a longer word is not an id
    CHECK_NOTHROW(db.observe({.kind = SourceKind::document, .uri = "z", .content = "x" + c + "y", .observed_at = "2026-10-01"}));
}
