// Ф3: retract, supersede, lazy staleness, recompute, what_if.
#include <beliefdb/store.hpp>
#include <doctest/doctest.h>

#include <algorithm>

using namespace beliefdb;

namespace {
bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

/// s1 -> A -> B -> C (min chain), plus s2 -> A2 -> B (B derives from A and A2)
struct Chain {
    Store db = Store::open(":memory:", "test");
    std::string rule = db.define_rule({.name = "r"});
    std::string s1 = db.observe({.kind = SourceKind::document, .uri = "u1", .content = "one", .observed_at = "2026-01-01", .attribution = "x"});
    std::string s2 = db.observe({.kind = SourceKind::measurement, .uri = "u2", .content = "two", .observed_at = "2026-01-02", .attribution = "y"});
    std::string a = db.assert_claim({.text = "A", .support = {s1}, .credence = 0.8});
    std::string a2 = db.assert_claim({.text = "A2", .support = {s2}, .credence = 0.95, .method = CredenceMethod::measured});
    std::string b = db.derive({.text = "B", .from = {a, a2}, .rule = rule});
    std::string c = db.derive({.text = "C", .from = {b}, .rule = rule});
};
}  // namespace

TEST_CASE("retract: consequences are flagged, nothing is deleted, why shows the knocked-out support") {
    Chain f;
    f.db.retract(f.s1, "документ оказался подделкой");
    CHECK(f.db.get(f.s1)->status == Status::retracted);
    CHECK(f.db.get(f.s1)->payload["retracted"]["reason"] == "документ оказался подделкой");
    for (const auto& id : {f.a, f.b, f.c}) {
        auto n = f.db.get(id);
        CHECK(n->status == Status::stale);
        CHECK(n->credence->stale);
        CHECK(n->credence->value > 0);  // the old value is still served
    }
    CHECK_FALSE(f.db.get(f.a2)->credence->stale);
    CHECK(f.db.counts().nodes == 7);  // 4 claims + 2 sources + 1 rule, all still there
    CHECK(f.db.counts().edges == 5);

    auto st = f.db.stale();
    REQUIRE(st.size() == 3);
    CHECK(st[0].id == f.a);  // shallow first
    CHECK(st[0].depth == 0);  // depth is over live justification: A has none left
    CHECK(st[2].id == f.c);
    REQUIRE(st[0].causes.size() == 1);
    CHECK(st[0].causes[0].cause == f.s1);
    CHECK(st[0].causes[0].reason == "retracted: документ оказался подделкой");

    auto w = f.db.why(f.b);
    CHECK_FALSE(w.notice);
    const WhyNode* wa = nullptr;
    for (const auto& ch : w.children) if (ch.id == f.a) wa = &ch;
    REQUIRE(wa);
    CHECK(wa->status == Status::stale);
    REQUIRE(wa->children.size() == 1);
    CHECK(wa->children[0].status == Status::retracted);
    REQUIRE(wa->children[0].notice);
    CHECK(wa->children[0].notice->find("подделкой") != std::string::npos);
    // why on the retracted node itself says so first
    auto ws = f.db.why(f.s1);
    REQUIRE(ws.notice);
    CHECK(ws.notice->rfind("retracted", 0) == 0);

    // a dead justifier contributes no sources any more
    CHECK(f.db.nutrition(f.a).n_sources == 0);
    CHECK(f.db.nutrition(f.c).sources == std::vector<std::string>{f.s2});

    // a retracted node cannot justify anything new
    try { f.db.assert_claim({.text = "X", .support = {f.s1}, .credence = 0.5}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::invalid_support); }
    try { f.db.retract(f.s1, "again"); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    try { f.db.retract(f.c, ""); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
}

TEST_CASE("retract a rule: every application and everything above it is flagged") {
    Chain f;
    auto other = f.db.define_rule({.name = "other"});
    auto d = f.db.derive({.text = "D", .from = {f.a}, .rule = other});
    f.db.retract(f.rule, "правило неверно");
    CHECK(f.db.get(f.b)->credence->stale);
    CHECK(f.db.get(f.c)->credence->stale);
    CHECK_FALSE(f.db.get(f.a)->credence->stale);
    CHECK_FALSE(f.db.get(d)->credence->stale);
    CHECK(f.db.stale()[0].causes[0].cause == f.rule);
    try { f.db.derive({.text = "E", .from = {f.a}, .rule = f.rule}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::invalid_support); }
    auto w = f.db.why(f.c);
    REQUIRE(w.children.size() == 1);
    CHECK(w.children[0].rule == "r (retracted)");
}

TEST_CASE("supersede a source: new node, replaces edge, old superseded, consequences flagged") {
    Chain f;
    auto s1b = f.db.supersede(f.s1, {.kind = SourceKind::document, .uri = "u1-v2", .content = "one, corrected", .observed_at = "2026-01-05", .attribution = "x"}, "опечатка в оригинале");
    CHECK(s1b != f.s1);
    CHECK(f.db.get(f.s1)->status == Status::superseded);
    CHECK(f.db.get(f.s1)->payload["superseded_by"]["id"] == s1b);
    CHECK(f.db.get(s1b)->status == Status::active);
    auto out = f.db.edges_from(s1b);
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == EdgeType::replaces);
    CHECK(out[0].dst == f.s1);
    CHECK(f.db.get(f.a)->credence->stale);
    CHECK(f.db.get(f.c)->credence->stale);
    CHECK(f.db.stale()[0].causes[0].cause == f.s1);

    auto w = f.db.why(f.s1);
    REQUIRE(w.notice);
    CHECK(w.notice->find("superseded by " + s1b) == 0);
    CHECK(w.notice->find("опечатка") != std::string::npos);

    // only one active node per chain
    try { f.db.supersede(f.s1, {.kind = SourceKind::document, .uri = "u1-v3", .content = "one, again"}, "x"); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::already_replaced); CHECK(std::string(e.what()).find(s1b) != std::string::npos); }
    // the chain continues from the active head
    auto s1c = f.db.supersede(s1b, {.kind = SourceKind::document, .uri = "u1-v3", .content = "one, again"}, "ещё раз");
    CHECK(f.db.get(s1b)->status == Status::superseded);
    CHECK(f.db.get(s1c)->status == Status::active);
    // the same content as the old node is not a correction
    try { f.db.supersede(s1c, {.kind = SourceKind::document, .uri = "u1-v3", .content = "one, again"}, "x"); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::duplicate); }
    // link replaces by hand behaves the same (and rejects a replacement that depends on the old node)
    auto x = f.db.assert_claim({.text = "X", .support = {s1c}, .credence = 0.5});
    auto y = f.db.derive({.text = "Y", .from = {x}, .rule = f.rule});
    try { f.db.link(y, x, EdgeType::replaces); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    auto x2 = f.db.assert_claim({.text = "X2", .support = {s1c}, .credence = 0.65});
    f.db.link(x2, x, EdgeType::replaces, std::nullopt, "уточнение");
    CHECK(f.db.get(x)->status == Status::superseded);
    CHECK(f.db.get(y)->credence->stale);
    // a stale node is still in force and may be corrected; one node may replace several
    f.db.link(x2, f.a, EdgeType::replaces);
    CHECK(f.db.get(f.a)->status == Status::superseded);
}

TEST_CASE("recompute: follows replaces chains, drops retracted premises, min recomputes itself") {
    Chain f;
    // A is corrected; B and C stand on the old A
    auto a_new = f.db.supersede(f.a, ClaimInput{.text = "A, corrected", .support = {f.s1}, .credence = 0.5}, "перечитал");
    CHECK(f.db.get(f.b)->credence->stale);
    CHECK(f.db.get(f.b)->credence->value == doctest::Approx(0.8));  // old value still served
    // bottom-up: C before B is refused
    try { f.db.recompute(f.c); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::stale_premise); }
    f.db.recompute(f.b);
    auto b = f.db.get(f.b);
    CHECK_FALSE(b->credence->stale);
    CHECK(b->status == Status::active);
    CHECK(b->credence->value == doctest::Approx(0.5));
    // the edge moved to the replacement
    auto bases = f.db.edges_from(f.b);
    std::vector<std::string> dsts;
    for (const auto& e : bases) if (e.type == EdgeType::derives_from) dsts.push_back(e.dst);
    CHECK(has(dsts, a_new));
    CHECK_FALSE(has(dsts, f.a));
    CHECK(f.db.nutrition(f.b).n_sources == 2);
    // C is still stale (B's value changed), now with two causes
    auto st = f.db.stale();
    REQUIRE(st.size() == 1);
    CHECK(st[0].id == f.c);
    CHECK(st[0].causes.size() == 2);
    f.db.recompute(f.c);
    CHECK(f.db.get(f.c)->credence->value == doctest::Approx(0.5));
    CHECK(f.db.stale().empty());

    // a retracted premise is dropped; min goes over what is left
    f.db.retract(a_new, "и это неверно");
    f.db.recompute(f.b);
    CHECK(f.db.get(f.b)->credence->value == doctest::Approx(0.95));
    CHECK(f.db.nutrition(f.b).sources == std::vector<std::string>{f.s2});
    // when nothing is left the call fails and the node stays stale
    f.db.retract(f.a2, "и это");
    try { f.db.recompute(f.b); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::no_support); }
    CHECK(f.db.get(f.b)->credence->stale);
    CHECK(f.db.get(f.b)->credence->value == doctest::Approx(0.95));
}

TEST_CASE("recompute: non-min methods need the caller's judgement; min refuses one") {
    Chain f;
    auto s3 = f.db.observe({.kind = SourceKind::document, .uri = "u3", .content = "three", .observed_at = "2026-01-03"});
    auto j = f.db.derive({.text = "J", .from = {f.a}, .rule = f.rule, .credence = 0.65, .method = CredenceMethod::llm_judgment});
    f.db.retract(f.s1, "x");
    f.db.link(s3, f.a, EdgeType::supports);  // A gets a new leg to stand on
    try { f.db.recompute(f.a); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_credence); }
    try { f.db.recompute(f.a, 0.7); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_credence); }
    f.db.recompute(f.a, 0.65);
    CHECK(f.db.get(f.a)->credence->value == doctest::Approx(0.65));
    CHECK(f.db.get(f.a)->credence->method == CredenceMethod::stated);
    CHECK(f.db.nutrition(f.a).sources == std::vector<std::string>{s3});
    try { f.db.recompute(f.b, 0.5); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    f.db.recompute(f.b);
    CHECK(f.db.get(f.b)->credence->value == doctest::Approx(0.65));
    try { f.db.recompute(j); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_credence); }
    f.db.recompute(j, 0.35);
    CHECK(f.db.get(j)->credence->value == doctest::Approx(0.35));
}

TEST_CASE("a claim built on a stale premise is stale from birth, for the same cause") {
    Chain f;
    f.db.retract(f.s1, "x");
    auto d = f.db.derive({.text = "D", .from = {f.b}, .rule = f.rule});
    CHECK(f.db.get(d)->credence->stale);
    CHECK(f.db.get(d)->status == Status::stale);
    auto st = f.db.stale();
    auto it = std::find_if(st.begin(), st.end(), [&](const StaleEntry& e) { return e.id == d; });
    REQUIRE(it != st.end());
    CHECK(it->causes[0].cause == f.s1);
}

TEST_CASE("what_if: projected values along min steps, stale-only elsewhere, nothing written") {
    Chain f;
    auto j = f.db.derive({.text = "J", .from = {f.b}, .rule = f.rule, .credence = 0.8, .method = CredenceMethod::llm_judgment});
    auto k = f.db.derive({.text = "K", .from = {j}, .rule = f.rule});
    auto r = f.db.what_if(f.a, 0.35);
    REQUIRE(r.size() == 4);
    auto find = [&](const std::string& id) { return *std::find_if(r.begin(), r.end(), [&](const WhatIfEntry& e) { return e.id == id; }); };
    CHECK(find(f.b).current == doctest::Approx(0.8));
    CHECK(find(f.b).projected == 0.35);
    CHECK(find(f.c).projected == 0.35);
    CHECK_FALSE(find(j).projected);  // llm_judgment: would go stale, no projection
    CHECK_FALSE(find(k).projected);  // min, but over a stale-only premise
    CHECK(find(j).method == CredenceMethod::llm_judgment);
    // raising A does not raise B above A2's min partner
    CHECK(f.db.what_if(f.a, 0.95)[0].projected == 0.95);
    CHECK(f.db.what_if(f.a2, 0.5)[0].projected == 0.5);
    CHECK(f.db.get(f.b)->credence->value == doctest::Approx(0.8));
    CHECK(f.db.stale().empty());
    try { f.db.what_if(f.a, 0.7); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_credence); }
    CHECK(f.db.what_if(f.c, 0.5).empty());
}

TEST_CASE("addendum 2: an import is superseded by the real evidence, nutrition does not double count") {
    auto db = Store::open(":memory:", "test");
    auto rule = db.define_rule({.name = "r"});
    auto report = db.observe({.kind = SourceKind::document, .directness = Directness::secondary, .uri = "report", .content = "анализ: X", .observed_at = "2026-09-01", .attribution = "agency"});
    auto imported = db.assert_claim({.text = "X", .support = {report}, .credence = 0.65});
    auto d = db.derive({.text = "D from X", .from = {imported}, .rule = rule});
    CHECK(db.nutrition(d).n_attributions == 1);
    // later the report's own sources turn up
    auto s1 = db.observe({.kind = SourceKind::measurement, .uri = "m1", .content = "x=1", .observed_at = "2026-08-01", .attribution = "lab A"});
    auto s2 = db.observe({.kind = SourceKind::measurement, .uri = "m2", .content = "x=1 again", .observed_at = "2026-08-02", .attribution = "lab B"});
    auto real = db.supersede(imported, ClaimInput{.text = "X", .support = {s1, s2}, .credence = 0.8, .method = CredenceMethod::measured}, "нашли исходные измерения");
    db.recompute(d);
    auto n = db.nutrition(d);
    std::vector<std::string> expected{s1, s2};
    std::sort(expected.begin(), expected.end());
    CHECK(n.sources == expected);
    CHECK(n.n_attributions == 2);  // not 3: the report was a retelling of these
    CHECK(n.n_secondary == 0);
    CHECK(db.get(d)->credence->value == doctest::Approx(0.8));
    CHECK(db.get(imported)->status == Status::superseded);
    CHECK(db.why(imported).notice->find(real) != std::string::npos);
}
