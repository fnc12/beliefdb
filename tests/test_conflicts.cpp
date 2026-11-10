// phase 4: conflicts (explicit and detected), refinement candidates, questions.
#include <beliefdb/store.hpp>
#include <doctest/doctest.h>

#include <algorithm>

using namespace beliefdb;

namespace {
struct World {
    Store db = Store::open(":memory:", "test");
    std::string rule = db.define_rule({.name = "r"});
    std::string s1 = db.observe({.kind = SourceKind::measurement, .uri = "scale", .content = "86.0", .observed_at = "2026-10-01", .attribution = "scale"});
    std::string s2 = db.observe({.kind = SourceKind::human_statement, .uri = "me", .content = "about 80", .observed_at = "2026-10-02", .attribution = "me"});
    std::string s3 = db.observe({.kind = SourceKind::document, .uri = "doc", .content = "x", .observed_at = "2026-10-03", .attribution = "doc"});
    Scope sc(const char* from, const char* to) { Scope s; s.valid_from = from; s.valid_to = to; return s; }
    // explicit, otherwise assert fills valid_from from the source date and two claims
    // from sources observed a day apart would nest instead of coincide
    Scope any() { Scope s; s.valid_from = "2026-01-01"; return s; }
};
}  // namespace

TEST_CASE("conflicts: two incompatible typed claims in overlapping scopes are found and ranked by dependents") {
    World w;
    auto a = w.db.assert_claim({.text = "weight 86 kg", .subject = "me", .predicate = "weight", .value = Value::point(86, "kg"),
                                .support = {w.s1}, .credence = 0.95, .method = CredenceMethod::measured, .scope = w.sc("2026-10-01", "2026-10-31")});
    auto b = w.db.assert_claim({.text = "weight about 80", .subject = "me", .predicate = "weight", .value = Value::number(78, 82, "kg"),
                                .support = {w.s2}, .credence = 0.5, .scope = w.sc("2026-09-20", "2026-10-20")});
    // a compatible third one: no conflict
    auto c = w.db.assert_claim({.text = "weight 85-87", .subject = "me", .predicate = "weight", .value = Value::number(85, 87, "kg"),
                                .support = {w.s3}, .credence = 0.65, .scope = w.sc("2026-10-05", "2026-11-10"), .despite = {a}});
    // a disjoint-in-time one: no conflict either
    auto d = w.db.assert_claim({.text = "weight 70 in 2020", .subject = "me", .predicate = "weight", .value = Value::point(70, "kg"),
                                .support = {w.s3}, .credence = 0.5, .scope = w.sc("2020-01-01", "2020-12-31")});
    // something stands on b: the b-c pair should rank first
    auto e = w.db.derive({.text = "E", .from = {b}, .rule = w.rule});
    auto f = w.db.derive({.text = "F", .from = {e}, .rule = w.rule});
    (void)d; (void)f;

    auto cs = w.db.conflicts();
    REQUIRE(cs.size() == 2);  // a-b and b-c
    CHECK(cs[0].n_dependents == 2);
    CHECK(cs[1].n_dependents == 2);
    for (const auto& x : cs) {
        CHECK(x.detected);
        CHECK_FALSE(x.explicit_edge);
        CHECK(x.subject == "me");
        CHECK(x.predicate == "weight");
        CHECK(x.a.id < x.b.id);
        CHECK((x.a.id == b || x.b.id == b));
        REQUIRE(x.overlap);
    }
    // ids are time-ordered ULIDs, but two created in the same millisecond may sort either way
    auto ab = std::find_if(cs.begin(), cs.end(), [&](const ConflictEntry& x) { return x.a.id == a || x.b.id == a; });
    REQUIRE(ab != cs.end());
    const ConflictSide& sa = ab->a.id == a ? ab->a : ab->b;
    const ConflictSide& sb = ab->a.id == a ? ab->b : ab->a;
    CHECK(ab->overlap->valid_from == "2026-10-01");
    CHECK(ab->overlap->valid_to == "2026-10-20");
    CHECK(sa.n_sources == 1);
    CHECK(sb.n_dependents == 2);
    CHECK(sa.credence->value == doctest::Approx(0.95));
    CHECK(sa.value.describe() == "86 kg");
    CHECK(sb.value.describe() == "78..82 kg");

    CHECK(w.db.conflicts_of(a).size() == 1);
    CHECK(w.db.conflicts_of(b).size() == 2);
    CHECK(w.db.conflicts_of(d).empty());
    CHECK(w.db.conflicts("health").empty());

    // ranking: give a its own tower of dependents and the a-b pair goes first
    auto g = w.db.derive({.text = "G", .from = {a}, .rule = w.rule});
    auto h = w.db.derive({.text = "H", .from = {g}, .rule = w.rule});
    auto i = w.db.derive({.text = "I", .from = {h}, .rule = w.rule});
    (void)i;
    cs = w.db.conflicts();
    CHECK((cs[0].a.id == a || cs[0].b.id == a));
    CHECK(cs[0].n_dependents == 5);

    // retracting one side removes its conflicts from the list; nothing else is touched
    w.db.retract(w.s2, "weighed on a broken scale");  // b goes stale, still in force
    CHECK(w.db.conflicts().size() == 2);
    w.db.retract(b, "wrong");
    CHECK(w.db.conflicts().empty());
}

TEST_CASE("conflicts: text is never compared; explicit contradicts edges are listed regardless") {
    World w;
    auto a = w.db.assert_claim({.text = "X works", .subject = "X", .predicate = "works", .value = Value::text("yes"), .support = {w.s1}, .credence = 0.65, .scope = w.any()});
    auto b = w.db.assert_claim({.text = "X does not work", .subject = "X", .predicate = "works", .value = Value::text("no"), .support = {w.s2}, .credence = 0.65, .scope = w.any()});
    CHECK(w.db.conflicts().empty());
    w.db.link(a, b, EdgeType::contradicts);
    auto cs = w.db.conflicts();
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].explicit_edge);
    CHECK_FALSE(cs[0].detected);
    CHECK_FALSE(cs[0].overlap);
    CHECK(cs[0].subject == "X");
    // the same pair found both ways is one entry
    w.db.define_predicate({.name = "ok", .value_type = ValueType::boolean});
    auto c = w.db.assert_claim({.text = "ok", .subject = "Y", .predicate = "ok", .value = Value::boolean(true), .support = {w.s1}, .credence = 0.65, .scope = w.any()});
    auto d = w.db.assert_claim({.text = "not ok", .subject = "Y", .predicate = "ok", .value = Value::boolean(false), .support = {w.s2}, .credence = 0.65, .scope = w.any()});
    w.db.link(d, c, EdgeType::contradicts);
    cs = w.db.conflicts();
    REQUIRE(cs.size() == 2);
    auto cd = std::find_if(cs.begin(), cs.end(), [&](const ConflictEntry& x) { return x.subject == "Y"; });
    REQUIRE(cd != cs.end());
    CHECK(cd->explicit_edge);
    CHECK(cd->detected);
    // different units are not comparable, so not a conflict (and not silently equal either)
    auto e = w.db.assert_claim({.text = "86 kg", .subject = "Z", .predicate = "mass", .value = Value::point(86, "kg"), .support = {w.s1}, .credence = 0.65, .scope = w.any()});
    (void)e;
    try {
        w.db.assert_claim({.text = "190 lb", .subject = "Z", .predicate = "mass", .value = Value::point(190, "lb"), .support = {w.s2}, .credence = 0.65, .scope = w.any()});
    } catch (const Error& err) { CHECK(err.code == ErrorCode::type_mismatch); }
    CHECK(w.db.conflicts().size() == 2);
}

TEST_CASE("refinements: a nested scope with a different value is a refinement candidate, not a conflict") {
    World w;
    Scope general = w.any(); general.note = "in general";
    Scope winter = w.sc("2026-12-01", "2027-02-28");
    auto g = w.db.assert_claim({.text = "water norm 2 l", .subject = "me", .predicate = "water_norm", .value = Value::point(2, "l"),
                                .support = {w.s3}, .credence = 0.65, .scope = general});
    auto s = w.db.assert_claim({.text = "in winter 1.5 l", .subject = "me", .predicate = "water_norm", .value = Value::point(1.5, "l"),
                                .support = {w.s2}, .credence = 0.5, .scope = winter});
    CHECK(w.db.conflicts().empty());
    auto rs = w.db.refinements();
    REQUIRE(rs.size() == 1);
    CHECK(rs[0].narrower.id == s);
    CHECK(rs[0].wider.id == g);
    CHECK(rs[0].predicate == "water_norm");
    // once somebody links them, the suggestion is gone
    w.db.link(s, g, EdgeType::refines);
    CHECK(w.db.refinements().empty());
    CHECK(w.db.conflicts().empty());
    // ...and if somebody insists it is a contradiction, it is listed as one, explicitly
    Scope summer = w.sc("2026-06-01", "2026-08-31");
    auto t = w.db.assert_claim({.text = "in summer 3 l", .subject = "me", .predicate = "water_norm", .value = Value::point(3, "l"),
                                .support = {w.s1}, .credence = 0.5, .scope = summer});
    CHECK(w.db.refinements().size() == 1);
    w.db.link(t, g, EdgeType::contradicts);
    CHECK(w.db.refinements().empty());
    CHECK(w.db.conflicts().size() == 1);
    // equal scopes are a plain contradiction
    auto u = w.db.assert_claim({.text = "in winter 1 l", .subject = "me", .predicate = "water_norm", .value = Value::point(1, "l"),
                                .support = {w.s1}, .credence = 0.5, .scope = winter});
    (void)u;
    CHECK(w.db.conflicts().size() == 2);
    CHECK(w.db.refinements().size() == 1);  // u vs g
}

TEST_CASE("questions: first-class objects with a criterion, progress, answers and a resolution") {
    World w;
    auto a = w.db.assert_claim({.text = "86", .subject = "me", .predicate = "weight", .value = Value::point(86, "kg"), .support = {w.s1}, .credence = 0.95, .method = CredenceMethod::measured, .scope = w.any()});
    auto b = w.db.assert_claim({.text = "80", .subject = "me", .predicate = "weight", .value = Value::point(80, "kg"), .support = {w.s2}, .credence = 0.5, .scope = w.any()});
    try { w.db.ask({.text = "how much do I weigh?"}); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    try { w.db.ask({.text = "x", .acceptance_criterion = "y", .about = {"c_NOPE"}}); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::not_found); }
    auto q = w.db.ask({.text = "how much do I weigh?", .why_open = "the scales and the feeling disagree", .acceptance_criterion = "two weighings on the same scales in one day",
                       .about = {a, b}, .domain = "health"});
    CHECK(q.rfind("q_", 0) == 0);
    CHECK(w.db.get(q)->type == NodeType::question);
    CHECK(w.db.conflicts()[0].questions == std::vector<std::string>{q});

    auto qs = w.db.questions();
    REQUIRE(qs.size() == 1);
    CHECK(qs[0].id == q);
    CHECK(qs[0].status == Status::active);
    CHECK(qs[0].about.size() == 2);
    CHECK(qs[0].answers.empty());
    CHECK(qs[0].progress.empty());
    CHECK(w.db.questions("money").empty());

    w.db.note_progress(q, "weighed in the morning: 85.5");
    auto s4 = w.db.observe({.kind = SourceKind::measurement, .uri = "scale", .content = "85.5", .observed_at = "2026-10-05"});
    auto ans = w.db.assert_claim({.text = "85.5", .subject = "me", .predicate = "weight", .value = Value::point(85.5, "kg"), .support = {s4}, .credence = 0.95, .method = CredenceMethod::measured, .scope = w.any()});
    w.db.link(ans, q, EdgeType::answers);
    auto qe = w.db.question(q);
    REQUIRE(qe.progress.size() == 1);
    CHECK(qe.progress[0].text == "weighed in the morning: 85.5");
    CHECK(qe.progress[0].actor == "test");
    CHECK(qe.answers == std::vector<std::string>{ans});

    try { w.db.close_question(q, ""); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    w.db.close_question(q, "two weighings agreed on 85.5; retract b");
    CHECK(w.db.get(q)->status == Status::closed);
    CHECK(w.db.questions().empty());
    REQUIRE(w.db.questions("", true).size() == 1);
    CHECK(w.db.questions("", true)[0].resolution.find("85.5") != std::string::npos);
    try { w.db.note_progress(q, "late"); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    try { w.db.close_question(q, "again"); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    CHECK(w.db.conflicts()[0].questions.empty());  // closed questions no longer count as "being looked into"
    CHECK(w.db.question(q).to_json()["status"] == "closed");
    // a question is not a justification
    try { w.db.assert_claim({.text = "x", .support = {q}, .credence = 0.5}); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::invalid_support); }
}

TEST_CASE("triage: decisions persist, expire when a side changes, pairs vanish and come back") {
    World w;
    auto a = w.db.assert_claim({.text = "86", .subject = "me", .predicate = "weight", .value = Value::point(86, "kg"), .support = {w.s1}, .credence = 0.95, .method = CredenceMethod::measured, .scope = w.any()});
    auto b = w.db.assert_claim({.text = "80", .subject = "me", .predicate = "weight", .value = Value::point(80, "kg"), .support = {w.s2}, .credence = 0.5, .scope = w.any()});
    auto cs = w.db.conflicts();
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].triage.state == TriageState::open);
    CHECK(cs[0].triage.times_seen == 1);
    CHECK(cs[0].triage.open_days == 0);
    CHECK(w.db.conflicts()[0].triage.times_seen == 2);

    // not a conflict: dismissed, hidden by default, visible with include_decided, reason kept
    try { w.db.dismiss_conflict(a, b, ""); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    w.db.dismiss_conflict(b, a, "80 was hearsay, on other scales");
    CHECK(w.db.conflicts().empty());
    auto all = w.db.conflicts("", true);
    REQUIRE(all.size() == 1);
    CHECK(all[0].triage.state == TriageState::dismissed);
    CHECK(all[0].triage.decided_by == "test");
    CHECK(all[0].triage.reason == "80 was hearsay, on other scales");
    CHECK(all[0].triage.expired_at.empty());
    auto st = w.db.triage_stats();
    CHECK(st.dismissed == 1);
    CHECK(st.precision == 0.0);

    // a side changes (b goes stale: status is part of the fingerprint): the decision expires, the reason stays
    w.db.retract(w.s2, "the scales were broken");
    cs = w.db.conflicts();
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].triage.state == TriageState::open);
    CHECK_FALSE(cs[0].triage.expired_at.empty());
    CHECK(cs[0].triage.reason == "80 was hearsay, on other scales");
    CHECK(cs[0].triage.times_seen >= 3);  // every scan counts, including the ones that hid the pair

    // resolved by scope: narrow b into the past, the pair is no longer found and vanishes on the next scan
    w.db.resolve_conflict(a, b, "b refers to September, narrowing the scope");
    CHECK(w.db.conflicts().empty());
    CHECK(w.db.triage_stats().resolved == 1);
    auto b2 = w.db.supersede(b, ClaimInput{.text = "80 in September", .subject = "me", .predicate = "weight", .value = Value::point(80, "kg"),
                                           .support = {w.s3}, .credence = 0.5, .scope = w.sc("2026-09-01", "2026-09-30")}, "scope");
    (void)b2;
    CHECK(w.db.conflicts().empty());
    CHECK(w.db.conflicts("", true).empty());       // the pair is no longer found at all...
    CHECK(w.db.triage_stats().resolved == 1);     // ...and the resolved decision is kept, not turned into vanished

    // an open pair that stops being found vanishes, and comes back as open if found again
    auto c = w.db.assert_claim({.text = "90", .subject = "me", .predicate = "weight", .value = Value::point(90, "kg"), .support = {w.s3}, .credence = 0.5, .scope = w.any()});
    REQUIRE(w.db.conflicts().size() == 1);
    w.db.retract(c, "typo");
    CHECK(w.db.conflicts().empty());
    CHECK(w.db.triage_stats().vanished == 1);

    // a decision about a pair the detector does not find is refused
    try { w.db.dismiss_conflict(a, b2, "x"); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::not_a_conflict); }

    // an explicit edge promotes the pair; promoted pairs are always listed and cannot be dismissed
    auto d = w.db.assert_claim({.text = "70", .subject = "me", .predicate = "weight", .value = Value::point(70, "kg"), .support = {w.s3}, .credence = 0.5, .scope = w.any()});
    w.db.link(a, d, EdgeType::contradicts);
    cs = w.db.conflicts();
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].triage.state == TriageState::promoted);
    CHECK(cs[0].explicit_edge);
    try { w.db.dismiss_conflict(a, d, "x"); FAIL("x"); } catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    st = w.db.triage_stats();
    CHECK(st.promoted == 1);
    CHECK(st.resolved == 1);
    CHECK(st.vanished == 1);
    CHECK(st.precision == doctest::Approx(1.0));
    CHECK(st.to_json()["precision"] == 1.0);
}

TEST_CASE("nesting by start date alone is a contradiction, not a refinement") {
    World w;
    // no explicit scope: valid_from comes from the source dates, a day apart, both open-ended
    auto a = w.db.assert_claim({.text = "86", .subject = "me", .predicate = "weight", .value = Value::point(86, "kg"), .support = {w.s1}, .credence = 0.95, .method = CredenceMethod::measured});
    auto b = w.db.assert_claim({.text = "80", .subject = "me", .predicate = "weight", .value = Value::point(80, "kg"), .support = {w.s2}, .credence = 0.5});
    CHECK(w.db.get(a)->payload["scope_valid_from_inferred"] == true);
    CHECK(w.db.get(a)->payload["scope"]["valid_from"] == "2026-10-01");
    auto cs = w.db.conflicts();
    REQUIRE(cs.size() == 1);
    CHECK(w.db.refinements().empty());
    // a bounded later claim is a refinement
    // (compatible with b in an overlapping scope: the dedup gate wants that acknowledged)
    auto c = w.db.assert_claim({.text = "80 this week", .subject = "me", .predicate = "weight", .value = Value::point(80, "kg"), .support = {w.s3}, .credence = 0.5, .scope = w.sc("2026-10-03", "2026-10-10"), .despite = {b}});
    (void)c;
    CHECK(w.db.conflicts().size() == 1);
    REQUIRE(w.db.refinements().size() == 1);
    CHECK(w.db.refinements()[0].wider.id == a);
    // extra conditions make it narrower too, even open-ended
    Scope fasting; fasting.valid_from = "2026-10-05"; fasting.conditions["state"] = "fasting";
    auto d = w.db.assert_claim({.text = "84 fasting", .subject = "me", .predicate = "weight", .value = Value::point(84, "kg"), .support = {w.s3}, .credence = 0.5, .scope = fasting});
    (void)d;
    CHECK(w.db.conflicts().size() == 2);     // a/b as before, plus c/d: partial overlap (c bounded in time, d by a condition), neither nested
    CHECK(w.db.refinements().size() == 3);   // c/a, d/a, d/b (b is open-ended from 10-02 and 84 != 80)
    // an explicit scope is not flagged as inferred
    CHECK_FALSE(w.db.get(c)->payload.contains("scope_valid_from_inferred"));
}
