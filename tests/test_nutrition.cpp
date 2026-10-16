#include <beliefdb/store.hpp>
#include <doctest/doctest.h>

using namespace beliefdb;

TEST_CASE("nutrition: retellings of one source stay one source; derivation adds nothing") {
    auto db = Store::open(":memory:", "test");
    auto rule = db.define_rule({.name = "r"});
    auto s = db.observe({.kind = SourceKind::document, .uri = "prospectus", .content = "купон 18%", .observed_at = "2026-09-01", .attribution = "KASE"});
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
    auto p1 = db.observe({.kind = SourceKind::document, .uri = "tg:1", .content = "врач сказал пить воду", .observed_at = "2026-10-01", .attribution = "врач Иванов"});
    auto p2 = db.observe({.kind = SourceKind::document, .uri = "tg:2", .content = "Иванов: пейте воду", .observed_at = "2026-10-02", .attribution = "врач Иванов"});
    auto p3 = db.observe({.kind = SourceKind::human_statement, .uri = "wife", .content = "он же говорил, вода", .observed_at = "2026-10-03", .attribution = "врач Иванов"});
    auto c = db.assert_claim({.text = "пить воду", .support = {p1, p2, p3}, .credence = 0.65});
    auto n = db.nutrition(c);
    CHECK(n.n_sources == 3);
    CHECK(n.n_attributions == 1);
    CHECK(n.attributions == std::vector<std::string>{"врач Иванов"});
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
    auto l = db.observe({.kind = SourceKind::llm_output, .uri = "claude", .content = "наверное 85", .observed_at = "2026-10-16"});
    auto c = db.assert_claim({.text = "вес", .support = {m, l}, .credence = 0.8, .method = CredenceMethod::measured});
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
