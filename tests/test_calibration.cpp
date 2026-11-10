// Phase 6: predictions recorded before the outcome, resolution with Brier, calibration by group, orphans.
#include <doctest/doctest.h>
#include <beliefdb/store.hpp>

#include <chrono>
#include <ctime>
#include <thread>

using namespace beliefdb;

namespace {
struct World {
    Store db = Store::open(":memory:", "test");
    std::string s_doc, s_say, rule;
    World() {
        s_doc = db.observe({.kind = SourceKind::document, .uri = "doc:1", .content = "the forecast says rain", .observed_at = "2026-01-01", .attribution = "weather service"});
        s_say = db.observe({.kind = SourceKind::human_statement, .uri = "chat:1", .content = "my knee says rain", .observed_at = "2026-01-01", .attribution = "grandpa"});
        rule = db.define_rule({.name = "knee_rule", .description = "the knee knows"});
    }
};
}  // namespace

TEST_CASE("a prediction snapshots credence, method, rule and attributions; it resolves once with a Brier score; the rule's record moves") {
    World w;
    auto a = w.db.assert_claim({.text = "it rains tomorrow", .support = {w.s_doc, w.s_say}, .credence = 0.8, .method = CredenceMethod::stated});
    auto d = w.db.derive({.text = "the picnic is off", .from = {a}, .rule = "knee_rule"});
    CHECK_THROWS_AS(w.db.predict({.node_id = w.s_doc}), Error);  // sources are not forecasts
    auto p1 = w.db.predict({.node_id = a});
    auto p2 = w.db.predict({.node_id = d, .p = 0.95, .resolve_by = "2026-01-02"});
    CHECK(p1.rfind("p_", 0) == 0);
    auto e1 = w.db.prediction(p1);
    CHECK(e1.p == doctest::Approx(0.8));
    CHECK(e1.method == "stated");
    CHECK(e1.rule.empty());
    CHECK(e1.attributions == std::vector<std::string>{"grandpa", "weather service"});
    CHECK_FALSE(e1.outcome.has_value());
    auto e2 = w.db.prediction(p2);
    CHECK(e2.method == "min");
    CHECK(e2.rule == "knee_rule");
    CHECK(e2.resolve_by == "2026-01-02");
    CHECK(w.db.predictions().size() == 2);

    w.db.resolve(p1, true);
    w.db.resolve(p2, false, "sun all day");
    CHECK_THROWS_AS(w.db.resolve(p1, false), Error);
    CHECK(*w.db.prediction(p1).brier == doctest::Approx(0.04));
    CHECK(*w.db.prediction(p2).brier == doctest::Approx(0.9025));
    CHECK(w.db.prediction(p2).note == "sun all day");
    CHECK(w.db.predictions().empty());
    CHECK(w.db.predictions("", true).size() == 2);
    const auto r = w.db.get(w.rule);
    CHECK(r->payload["n_resolved"] == 1);
    CHECK(r->payload["n_correct"] == 0);
    CHECK(r->payload["brier"].get<double>() == doctest::Approx(0.9025));
}

TEST_CASE("calibration groups resolved predictions by credence step, method, rule and attribution") {
    World w;
    auto a = w.db.assert_claim({.text = "rain", .support = {w.s_doc}, .credence = 0.8, .method = CredenceMethod::stated});
    auto b = w.db.assert_claim({.text = "rain says the knee", .support = {w.s_say}, .credence = 0.8, .method = CredenceMethod::stated});
    auto c = w.db.assert_claim({.text = "measured 12 mm", .support = {w.s_doc}, .credence = 0.95, .method = CredenceMethod::measured});
    auto d = w.db.derive({.text = "picnic off", .from = {a}, .rule = "knee_rule"});
    auto pa = w.db.predict({.node_id = a}), pb = w.db.predict({.node_id = b}), pc = w.db.predict({.node_id = c}), pd = w.db.predict({.node_id = d});
    CHECK(w.db.calibration().resolved == 0);
    CHECK_FALSE(w.db.calibration().brier.has_value());
    w.db.resolve(pa, true);
    w.db.resolve(pb, false);
    w.db.resolve(pc, true);
    w.db.resolve(pd, true);
    const auto r = w.db.calibration();
    CHECK(r.predictions == 4);
    CHECK(r.resolved == 4);
    REQUIRE(r.brier.has_value());
    CHECK(*r.brier == doctest::Approx((0.04 + 0.64 + 0.0025 + 0.04) / 4));
    auto find = [](const std::vector<CalibrationBucket>& v, const std::string& k) -> const CalibrationBucket* { for (const auto& b : v) if (b.key == k) return &b; return nullptr; };
    REQUIRE(find(r.by_credence, "0.8"));
    CHECK(find(r.by_credence, "0.8")->n == 3);  // a, b and d (min over a = 0.8)
    CHECK(find(r.by_credence, "0.8")->hit_rate == doctest::Approx(2.0 / 3));
    CHECK(find(r.by_credence, "0.95")->n == 1);
    CHECK(find(r.by_method, "stated")->n == 2);
    CHECK(find(r.by_method, "measured")->hit_rate == doctest::Approx(1.0));
    CHECK(find(r.by_method, "min")->n == 1);
    CHECK(find(r.by_rule, "knee_rule")->n == 1);
    CHECK(find(r.by_attribution, "grandpa")->hit_rate == doctest::Approx(0.0));
    CHECK(find(r.by_attribution, "weather service")->n == 3);  // a, c, d
    CHECK(find(r.by_attribution, "weather service")->hit_rate == doctest::Approx(1.0));
    CHECK(w.db.calibration("nowhere").predictions == 0);
}

TEST_CASE("orphans: claims whose sources are older than the horizon and that gained no support since") {
    World w;
    auto old = w.db.observe({.kind = SourceKind::document, .uri = "doc:old", .content = "an old survey", .observed_at = "2020-01-01", .attribution = "survey"});
    auto stale_claim = w.db.assert_claim({.text = "most users prefer dark mode", .support = {old}, .credence = 0.65});
    char today[16];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(today, sizeof today, "%Y-%m-%d", &tm);
    auto fresh_src = w.db.observe({.kind = SourceKind::measurement, .uri = "sensor:now", .content = "12 mm today", .observed_at = today, .attribution = "sensor"});
    auto fresh_claim = w.db.assert_claim({.text = "it rains today", .support = {fresh_src}, .credence = 0.8, .method = CredenceMethod::measured});
    auto os = w.db.orphans(6);
    REQUIRE(os.size() >= 1);
    CHECK(os[0].id == stale_claim);
    CHECK(os[0].months > 60);
    CHECK(os[0].n_sources == 1);
    bool fresh_listed = false;
    for (const auto& o : os) if (o.id == fresh_claim) fresh_listed = true;
    CHECK_FALSE(fresh_listed);
    // reconfirmation: a new supports edge onto the stale claim takes it off the list even though the source is old
    // (an edge written in the same second as the claim is its original support, so wait past that)
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    w.db.link(w.s_say, stale_claim, EdgeType::supports);
    bool still = false;
    for (const auto& o : w.db.orphans(6)) if (o.id == stale_claim) still = true;
    CHECK_FALSE(still);
}
