#include <beliefdb/store.hpp>
#include <doctest/doctest.h>

#include <functional>

using namespace beliefdb;

namespace {
Scope note(const char* n) { Scope s; s.note = n; return s; }

int count_nodes(const WhyNode& w) {
    int n = 1;
    for (const auto& c : w.children) n += count_nodes(c);
    return n;
}
int max_depth(const WhyNode& w) {
    int d = 0;
    for (const auto& c : w.children) d = std::max(d, 1 + max_depth(c));
    return d;
}
bool all_leaves_are_sources(const WhyNode& w) {
    if (w.children.empty()) return w.type == NodeType::source;
    for (const auto& c : w.children)
        if (!all_leaves_are_sources(c)) return false;
    return true;
}
}  // namespace

// Acceptance for phase 1: a hand-built graph of ~20 claims; why() unrolls to sources.
TEST_CASE("acceptance: investment graph, why() reaches sources through every chain") {
    auto db = Store::open(":memory:", "test");
    // sources
    auto prospectus = db.observe({.kind = SourceKind::document, .uri = "https://kase.kz/.../prospectus.pdf", .content = "Coupon 18% a year, paid twice a year, 3-year term", .observed_at = "2026-09-01", .domain = "money"});
    auto rating = db.observe({.kind = SourceKind::document, .uri = "https://rating-agency/report", .content = "Issuer rating BB-, outlook stable", .observed_at = "2026-08-15", .domain = "money"});
    auto nbk = db.observe({.kind = SourceKind::measurement, .uri = "https://nationalbank.kz/base-rate", .content = "Base rate 15.5%", .observed_at = "2026-09-10", .domain = "money"});
    auto broker = db.observe({.kind = SourceKind::human_statement, .uri = "app:tabys-pro/account", .content = "Balance $1000, 2026-10-14", .observed_at = "2026-10-14", .domain = "money"});
    auto telegram = db.observe({.kind = SourceKind::document, .uri = "telegram:investkz/1234", .content = "They say gold miners are in the black this year", .observed_at = "2026-10-01", .domain = "money"});
    auto me = db.observe({.kind = SourceKind::human_statement, .uri = "owner", .content = "Investment horizon 3 years, risk tolerance medium", .observed_at = "2026-10-16", .domain = "money"});

    auto spread = db.define_rule({.name = "spread_over_base_rate", .description = "yield minus base rate = risk premium"});
    auto fit = db.define_rule({.name = "fits_profile", .description = "an instrument fits if term <= horizon and risk <= tolerance"});
    auto afford = db.define_rule({.name = "affordable", .description = "affordable if the minimum lot <= free balance"});
    auto verdict = db.define_rule({.name = "buy_verdict", .description = "fits + affordable + positive premium => buy candidate"});

    std::vector<std::string> claims;
    auto A = [&](const char* text, std::vector<std::string> support, double cr, CredenceMethod m, const char* sc) {
        ClaimInput c{.text = text, .support = std::move(support), .credence = cr, .method = m, .scope = note(sc), .domain = "money"};
        claims.push_back(db.assert_claim(c));
        return claims.back();
    };
    auto D = [&](const char* text, std::vector<std::string> from, const std::string& rule, const char* sc) {
        DeriveInput d{.text = text, .from = std::move(from), .rule = rule, .scope = note(sc), .domain = "money"};
        claims.push_back(db.derive(d));
        return claims.back();
    };

    // level 0: read straight from sources
    auto c_coupon = A("Bond coupon 18% a year", {prospectus}, 0.95, CredenceMethod::stated, "issue 2026, until maturity");
    auto c_term = A("Term to maturity 3 years", {prospectus}, 0.95, CredenceMethod::stated, "issue 2026");
    auto c_payout = A("Coupon paid twice a year", {prospectus}, 0.95, CredenceMethod::stated, "issue 2026");
    auto c_rating = A("Issuer rating BB-", {rating}, 0.8, CredenceMethod::stated, "as of 2026-08");
    auto c_outlook = A("Rating outlook stable", {rating}, 0.65, CredenceMethod::stated, "as of 2026-08");
    auto c_base = A("NBK base rate 15.5%", {nbk}, 0.95, CredenceMethod::measured, "as of 2026-09-10");
    auto c_balance = A("Free balance $1000", {broker}, 0.95, CredenceMethod::measured, "as of 2026-10-14");
    auto c_rumor = A("Gold miners in the black in 2026", {telegram}, 0.35, CredenceMethod::stated, "2026, rumour");
    auto c_horizon = A("Investment horizon 3 years", {me}, 0.95, CredenceMethod::stated, "since 2026-10");
    auto c_risk = A("Risk tolerance medium", {me}, 0.8, CredenceMethod::stated, "since 2026-10");
    auto c_lot = A("Minimum lot $100", {prospectus}, 0.8, CredenceMethod::stated, "issue 2026");

    // level 1
    auto c_premium = D("Risk premium 2.5 pp over the base rate", {c_coupon, c_base}, spread, "as of 2026-09");
    auto c_term_ok = D("Term fits the horizon", {c_term, c_horizon}, fit, "since 2026-10");
    auto c_risk_ok = D("BB- risk acceptable at medium tolerance", {c_rating, c_outlook, c_risk}, fit, "as of 2026-10");
    auto c_afford = D("Enough for at least 10 lots", {c_lot, c_balance}, afford, "as of 2026-10-14");
    auto c_sector = D("Sector in the black (by rumour)", {c_rumor}, fit, "2026");

    // level 2
    auto c_fits = D("The bond fits the profile", {c_term_ok, c_risk_ok}, fit, "as of 2026-10");
    auto c_worth = D("Premium positive at acceptable risk", {c_premium, c_risk_ok}, verdict, "as of 2026-10");

    // level 3
    auto c_buy = D("Buy candidate: up to 10 lots", {c_fits, c_worth, c_afford}, verdict, "as of 2026-10, until the rate is revised");
    auto c_buy_hyped = D("Buy candidate (rumours included)", {c_buy, c_sector}, verdict, "as of 2026-10");

    CHECK(claims.size() == 20);
    CHECK(db.counts().claims == 20);
    CHECK(db.counts().sources == 6);

    const auto w = db.why(c_buy_hyped);
    CHECK(all_leaves_are_sources(w));
    CHECK(max_depth(w) == 5);  // hyped -> buy -> fits -> term_ok -> c_term -> prospectus
    CHECK(count_nodes(w) > 20);  // shared bases appear in every chain they justify
    REQUIRE(w.credence.has_value());
    CHECK(w.credence->value == doctest::Approx(0.35));  // MIN drags the rumor through the whole chain
    CHECK(db.get(c_buy)->credence->value == doctest::Approx(0.65));  // without the rumor: bounded by the outlook

    // every child edge carries the rule name for derivations
    for (const auto& ch : w.children) {
        CHECK(ch.via == EdgeType::derives_from);
        CHECK(ch.rule == "buy_verdict");
    }

    // depth limit truncates instead of lying
    const auto shallow = db.why(c_buy_hyped, 1);
    CHECK(max_depth(shallow) == 1);
    bool any_truncated = false;
    for (const auto& ch : shallow.children) any_truncated = any_truncated || ch.truncated;
    CHECK(any_truncated);

    // depends_on: the whole upstream set, without duplicates
    auto deps = db.depends_on(c_buy_hyped);
    CHECK(deps.size() == 18 + 6);  // every claim except c_payout (nothing is derived from it) + all sources

    // JSON view is well-formed
    auto j = w.to_json();
    CHECK(j["id"] == c_buy_hyped);
    CHECK(j["children"].size() == 2);
    CHECK(j["credence"]["method"] == "min");
}

TEST_CASE("why: a source is a leaf; a claim with mixed justification lists supports first") {
    auto db = Store::open(":memory:", "test");
    auto s1 = db.observe({.kind = SourceKind::document, .uri = "u1", .content = "one", .observed_at = "2026-01-01"});
    auto s2 = db.observe({.kind = SourceKind::document, .uri = "u2", .content = "two", .observed_at = "2026-01-02"});
    auto rule = db.define_rule({.name = "r"});
    auto a = db.assert_claim({.text = "A", .support = {s1}, .credence = 0.8, .scope = note("n")});
    auto b = db.derive({.text = "B", .from = {a}, .rule = rule, .scope = note("n")});
    db.link(s2, b, EdgeType::supports);
    auto w = db.why(b);
    REQUIRE(w.children.size() == 2);
    CHECK(w.children[0].via == EdgeType::supports);
    CHECK(w.children[0].id == s2);
    CHECK(w.children[1].via == EdgeType::derives_from);
    CHECK(w.children[1].id == a);
    CHECK(w.children[1].rule == "r");
    CHECK(db.why(s1).children.empty());
    CHECK(db.why(s1).type == NodeType::source);
}
