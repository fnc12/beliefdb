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

// Acceptance for Ф1: a hand-built graph of ~20 claims; why() unrolls to sources.
TEST_CASE("acceptance: investment graph, why() reaches sources through every chain") {
    auto db = Store::open(":memory:", "test");
    // sources
    auto prospectus = db.observe({.kind = SourceKind::document, .uri = "https://kase.kz/.../prospectus.pdf", .content = "Купон 18% годовых, выплата раз в полгода, срок 3 года", .observed_at = "2026-09-01", .domain = "money"});
    auto rating = db.observe({.kind = SourceKind::document, .uri = "https://rating-agency/report", .content = "Рейтинг эмитента BB-, прогноз стабильный", .observed_at = "2026-08-15", .domain = "money"});
    auto nbk = db.observe({.kind = SourceKind::measurement, .uri = "https://nationalbank.kz/base-rate", .content = "Базовая ставка 15.5%", .observed_at = "2026-09-10", .domain = "money"});
    auto broker = db.observe({.kind = SourceKind::human_statement, .uri = "app:tabys-pro/account", .content = "Баланс $1000, 2026-10-14", .observed_at = "2026-10-14", .domain = "money"});
    auto telegram = db.observe({.kind = SourceKind::document, .uri = "telegram:investkz/1234", .content = "Говорят, золотодобытчики в этом году в плюсе", .observed_at = "2026-10-01", .domain = "money"});
    auto me = db.observe({.kind = SourceKind::human_statement, .uri = "owner", .content = "Горизонт инвестиций 3 года, терпимость к риску средняя", .observed_at = "2026-10-16", .domain = "money"});

    auto spread = db.define_rule({.name = "spread_over_base_rate", .description = "доходность минус базовая ставка = премия за риск"});
    auto fit = db.define_rule({.name = "fits_profile", .description = "инструмент подходит, если срок <= горизонт и риск <= терпимость"});
    auto afford = db.define_rule({.name = "affordable", .description = "можно купить, если минимальный лот <= свободный баланс"});
    auto verdict = db.define_rule({.name = "buy_verdict", .description = "подходит + доступно + премия положительная => кандидат на покупку"});

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
    auto c_coupon = A("Купон облигации 18% годовых", {prospectus}, 0.95, CredenceMethod::stated, "выпуск 2026, до погашения");
    auto c_term = A("Срок до погашения 3 года", {prospectus}, 0.95, CredenceMethod::stated, "выпуск 2026");
    auto c_payout = A("Выплата купона раз в полгода", {prospectus}, 0.95, CredenceMethod::stated, "выпуск 2026");
    auto c_rating = A("Рейтинг эмитента BB-", {rating}, 0.8, CredenceMethod::stated, "на 2026-08");
    auto c_outlook = A("Прогноз по рейтингу стабильный", {rating}, 0.65, CredenceMethod::stated, "на 2026-08");
    auto c_base = A("Базовая ставка НБК 15.5%", {nbk}, 0.95, CredenceMethod::measured, "на 2026-09-10");
    auto c_balance = A("Свободный баланс $1000", {broker}, 0.95, CredenceMethod::measured, "на 2026-10-14");
    auto c_rumor = A("Золотодобытчики в плюсе в 2026", {telegram}, 0.35, CredenceMethod::stated, "2026, слух");
    auto c_horizon = A("Горизонт инвестиций 3 года", {me}, 0.95, CredenceMethod::stated, "с 2026-10");
    auto c_risk = A("Терпимость к риску средняя", {me}, 0.8, CredenceMethod::stated, "с 2026-10");
    auto c_lot = A("Минимальный лот $100", {prospectus}, 0.8, CredenceMethod::stated, "выпуск 2026");

    // level 1
    auto c_premium = D("Премия за риск 2.5 п.п. над базовой ставкой", {c_coupon, c_base}, spread, "на 2026-09");
    auto c_term_ok = D("Срок укладывается в горизонт", {c_term, c_horizon}, fit, "с 2026-10");
    auto c_risk_ok = D("Риск BB- приемлем при средней терпимости", {c_rating, c_outlook, c_risk}, fit, "на 2026-10");
    auto c_afford = D("Хватает на минимум 10 лотов", {c_lot, c_balance}, afford, "на 2026-10-14");
    auto c_sector = D("Сектор в плюсе (по слухам)", {c_rumor}, fit, "2026");

    // level 2
    auto c_fits = D("Облигация подходит профилю", {c_term_ok, c_risk_ok}, fit, "на 2026-10");
    auto c_worth = D("Премия положительная при приемлемом риске", {c_premium, c_risk_ok}, verdict, "на 2026-10");

    // level 3
    auto c_buy = D("Кандидат на покупку: до 10 лотов", {c_fits, c_worth, c_afford}, verdict, "на 2026-10, до пересмотра ставки");
    auto c_buy_hyped = D("Кандидат на покупку (с учётом слухов)", {c_buy, c_sector}, verdict, "на 2026-10");

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
