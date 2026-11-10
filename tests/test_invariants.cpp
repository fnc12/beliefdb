#include <beliefdb/store.hpp>
#include <doctest/doctest.h>

#include <algorithm>

using namespace beliefdb;

namespace {
Scope scope_note(const char* note) { Scope s; s.note = note; return s; }

struct Fixture {
    Store db = Store::open(":memory:", "test");
    std::string src = db.observe({.kind = SourceKind::document, .uri = "file:///visit.txt", .content = "Doctor: ibuprofen 400mg 2x/day 5 days", .observed_at = "2026-10-10"});
};
}  // namespace

TEST_CASE("inv1: a claim without support is rejected") {
    Fixture f;
    ClaimInput c{.text = "ibuprofen 400mg twice a day", .scope = scope_note("course 2026-10-10..15")};
    CHECK_THROWS_WITH_AS(f.db.assert_claim(c), doctest::Contains("support"), Error);
    try { f.db.assert_claim(c); } catch (const Error& e) { CHECK(e.code == ErrorCode::no_support); }
    CHECK(f.db.counts().claims == 0);
}

TEST_CASE("inv1: support must exist and be a source or claim") {
    Fixture f;
    ClaimInput c{.text = "x", .support = {"s_NOPE"}, .scope = scope_note("n")};
    try { f.db.assert_claim(c); FAIL("expected throw"); } catch (const Error& e) { CHECK(e.code == ErrorCode::invalid_support); }
    auto rule = f.db.define_rule({.name = "r"});
    c.support = {rule};
    try { f.db.assert_claim(c); FAIL("expected throw"); } catch (const Error& e) { CHECK(e.code == ErrorCode::invalid_support); }
}

TEST_CASE("inv2: justification graph stays acyclic") {
    Fixture f;
    auto rule = f.db.define_rule({.name = "dose_from_prescription", .description = "dose follows prescription"});
    auto a = f.db.assert_claim({.text = "A", .support = {f.src}, .credence = 0.8, .scope = scope_note("n")});
    auto b = f.db.derive({.text = "B", .from = {a}, .rule = rule, .scope = scope_note("n")});
    auto c = f.db.derive({.text = "C", .from = {b}, .rule = "dose_from_prescription", .scope = scope_note("n")});
    // C depends on B depends on A. Making A depend on C closes a cycle.
    try { f.db.link(a, c, EdgeType::derives_from, rule); FAIL("expected cycle"); } catch (const Error& e) { CHECK(e.code == ErrorCode::cycle); }
    try { f.db.link(a, c, EdgeType::assumes); FAIL("expected cycle"); } catch (const Error& e) { CHECK(e.code == ErrorCode::cycle); }
    // supports points the other way: "C supports A" also means A depends on C.
    try { f.db.link(c, a, EdgeType::supports); FAIL("expected cycle"); } catch (const Error& e) { CHECK(e.code == ErrorCode::cycle); }
    try { f.db.link(a, a, EdgeType::assumes); FAIL("expected cycle"); } catch (const Error& e) { CHECK(e.code == ErrorCode::cycle); }
    // the legal direction still works
    CHECK_NOTHROW(f.db.link(c, a, EdgeType::assumes));
    auto deps = f.db.depends_on(c);
    CHECK(deps.size() == 3);  // b, a, src
}

TEST_CASE("inv2: a cycle alternating edge types is caught (one DAG over supports/derives_from/assumes)") {
    Fixture f;
    auto rule = f.db.define_rule({.name = "r"});
    auto a = f.db.assert_claim({.text = "A", .support = {f.src}, .credence = 0.8, .scope = scope_note("n")});
    auto b = f.db.assert_claim({.text = "B", .support = {f.src}, .credence = 0.8, .scope = scope_note("n")});
    f.db.link(b, a, EdgeType::derives_from, rule);  // B depends on A
    try { f.db.link(b, a, EdgeType::supports); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::cycle); }  // A would depend on B
    try { f.db.link(a, b, EdgeType::assumes); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::cycle); }
    // non-evidential edges are outside the DAG check
    CHECK_NOTHROW(f.db.link(a, b, EdgeType::contradicts));
}

TEST_CASE("contradicts: symmetric, stored once in canonical order") {
    Fixture f;
    auto a = f.db.assert_claim({.text = "A", .support = {f.src}, .credence = 0.8, .scope = scope_note("n")});
    auto b = f.db.assert_claim({.text = "B", .support = {f.src}, .credence = 0.8, .scope = scope_note("n")});
    const std::string lo = std::min(a, b), hi = std::max(a, b);
    f.db.link(hi, lo, EdgeType::contradicts);
    try { f.db.link(lo, hi, EdgeType::contradicts); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::duplicate); }
    auto out = f.db.edges_from(lo);
    REQUIRE(out.size() == 1);
    CHECK(out[0].dst == hi);
    CHECK(out[0].type == EdgeType::contradicts);
}

TEST_CASE("credence: the error names the two nearest steps") {
    Fixture f;
    ClaimInput c{.text = "x", .support = {f.src}, .credence = 0.7, .scope = scope_note("n")};
    CHECK_THROWS_WITH_AS(f.db.assert_claim(c), doctest::Contains("0.65 and 0.8"), Error);
    c.credence = 1.5;
    CHECK_THROWS_WITH_AS(f.db.assert_claim(c), doctest::Contains("outside"), Error);
}

TEST_CASE("source: attribution and manual trust_hint are stored, kind is an enum") {
    Fixture f;
    auto id = f.db.observe({.kind = SourceKind::human_statement, .uri = "tg:doctor", .content = "drink more water",
                            .observed_at = "2026-10-01", .attribution = "Dr. Ivanov", .trust_hint = 0.8});
    auto n = f.db.get(id);
    REQUIRE(n.has_value());
    CHECK(n->payload["kind"] == "human_statement");
    CHECK(n->payload["attribution"] == "Dr. Ivanov");
    CHECK(n->payload["trust_hint"]["value"] == 0.8);
    CHECK(n->payload["trust_hint"]["method"] == "manual");
    try { f.db.observe({.kind = SourceKind::document, .content = "x", .trust_hint = 2.0}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
}

TEST_CASE("inv3: sources are immutable and deduplicated by content") {
    Fixture f;
    auto again = f.db.observe({.kind = SourceKind::document, .uri = "other-uri", .content = "Doctor: ibuprofen 400mg 2x/day 5 days", .observed_at = "2026-10-11"});
    CHECK(again == f.src);
    CHECK(f.db.counts().sources == 1);
    auto n = f.db.get(f.src);
    REQUIRE(n.has_value());
    CHECK(n->payload["uri"] == "file:///visit.txt");  // first observation wins
}

TEST_CASE("inv5: credence needs a method and sits on the ladder") {
    Fixture f;
    ClaimInput c{.text = "x", .support = {f.src}, .credence = 0.7, .method = CredenceMethod::stated, .scope = scope_note("n")};
    try { f.db.assert_claim(c); FAIL("expected throw"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_credence); }
    c.credence = 0.65;
    c.method = CredenceMethod::min;  // derivation method on an assert
    try { f.db.assert_claim(c); FAIL("expected throw"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_credence); }
    c.method = CredenceMethod::measured;
    auto id = f.db.assert_claim(c);
    auto n = f.db.get(id);
    REQUIRE(n.has_value());
    REQUIRE(n->credence.has_value());
    CHECK(n->credence->value == doctest::Approx(0.65));
    CHECK(n->credence->method == CredenceMethod::measured);
    CHECK_FALSE(n->credence->stale);
}

TEST_CASE("scope: empty scope is rejected, structured scope round-trips") {
    Fixture f;
    // supported by a source: valid_from is filled from its observed_at, so the scope is never empty
    auto auto_id = f.db.assert_claim({.text = "auto", .support = {f.src}, .credence = 0.8});
    CHECK(Scope::from_json(f.db.get(auto_id)->payload["scope"]).valid_from == "2026-10-10");
    // supported only by a claim and nothing said about scope: rejected
    ClaimInput c{.text = "x", .support = {auto_id}, .credence = 0.8};
    try { f.db.assert_claim(c); FAIL("expected throw"); } catch (const Error& e) { CHECK(e.code == ErrorCode::empty_scope); }
    c.support = {f.src};
    c.scope.valid_from = "2026-10-10";
    c.scope.valid_to = "2026-10-15";
    c.scope.conditions = {{"patient", "me"}};
    auto id = f.db.assert_claim(c);
    auto s = Scope::from_json(f.db.get(id)->payload["scope"]);
    CHECK(s.valid_from == "2026-10-10");
    CHECK(s.valid_to == "2026-10-15");
    CHECK(s.conditions["patient"] == "me");
}

TEST_CASE("derive: MIN by default, rule track record increments") {
    Fixture f;
    auto rule = f.db.define_rule({.name = "combine"});
    auto a = f.db.assert_claim({.text = "A", .support = {f.src}, .credence = 0.8, .scope = scope_note("n")});
    auto b = f.db.assert_claim({.text = "B", .support = {f.src}, .credence = 0.35, .scope = scope_note("n")});
    auto c = f.db.derive({.text = "C", .from = {a, b}, .rule = rule, .scope = scope_note("n")});
    auto n = f.db.get(c);
    REQUIRE(n.has_value());
    REQUIRE(n->credence.has_value());
    CHECK(n->credence->value == doctest::Approx(0.35));
    CHECK(n->credence->method == CredenceMethod::min);
    CHECK(n->credence->inputs["from"].size() == 2);
    CHECK(f.db.get(rule)->payload["n_applied"] == 1);
    // explicit credence with a non-min method is allowed; min without credence only
    try { f.db.derive({.text = "D", .from = {a}, .rule = rule, .method = CredenceMethod::rule, .scope = scope_note("n")}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::bad_credence); }
    CHECK_NOTHROW(f.db.derive({.text = "D", .from = {a}, .rule = rule, .credence = 0.5, .method = CredenceMethod::rule, .scope = scope_note("n")}));
    // derive from a source is not allowed: sources support, claims derive
    try { f.db.derive({.text = "E", .from = {f.src}, .rule = rule, .scope = scope_note("n")}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::invalid_support); }
    // unknown rule
    try { f.db.derive({.text = "E", .from = {a}, .rule = "nope", .scope = scope_note("n")}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::not_found); }
    // duplicate rule name
    try { f.db.define_rule({.name = "combine"}); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::duplicate); }
}

TEST_CASE("link: endpoint types are enforced, duplicates rejected") {
    Fixture f;
    auto a = f.db.assert_claim({.text = "A", .support = {f.src}, .credence = 0.8, .scope = scope_note("n")});
    auto b = f.db.assert_claim({.text = "B", .support = {f.src}, .credence = 0.8, .scope = scope_note("n")});
    try { f.db.link(f.src, a, EdgeType::contradicts); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    try { f.db.link(a, b, EdgeType::derives_from); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    CHECK_NOTHROW(f.db.link(a, b, EdgeType::contradicts));
    try { f.db.link(a, b, EdgeType::contradicts); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::duplicate); }
    try { f.db.link(f.src, a, EdgeType::supports); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::duplicate); }
    const std::string lo = std::min(a, b), hi = std::max(a, b);
    CHECK(f.db.edges_from(lo).size() == 1);   // the canonical contradicts row
    CHECK(f.db.edges_to(hi).size() == 2);     // supports from src + contradicts from lo
}
