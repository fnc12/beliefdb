#include <beliefdb/store.hpp>
#include <doctest/doctest.h>

using namespace beliefdb;

namespace {
Scope sc(const char* from, const char* to, nlohmann::json conds = nlohmann::json::object()) {
    Scope s;
    if (from) s.valid_from = from;
    if (to) s.valid_to = to;
    s.conditions = std::move(conds);
    return s;
}
}  // namespace

TEST_CASE("scope: intersection of intervals and conditions") {
    auto r = Scope::intersect(sc("2026-01-01", "2026-12-31"), sc("2026-06-01", nullptr));
    REQUIRE(r.has_value());
    CHECK(r->valid_from == "2026-06-01");
    CHECK(r->valid_to == "2026-12-31");
    CHECK_FALSE(Scope::intersect(sc("2026-01-01", "2026-03-01"), sc("2026-06-01", nullptr)).has_value());
    // unset ends are unbounded
    auto open = Scope::intersect(sc(nullptr, nullptr), sc(nullptr, nullptr));
    REQUIRE(open.has_value());
    CHECK_FALSE(open->valid_from.has_value());
    // conditions merge; a shared key with another value empties the intersection
    auto c = Scope::intersect(sc(nullptr, nullptr, {{"patient", "me"}}), sc(nullptr, nullptr, {{"dose", "400mg"}}));
    REQUIRE(c.has_value());
    CHECK(c->conditions["patient"] == "me");
    CHECK(c->conditions["dose"] == "400mg");
    CHECK_FALSE(Scope::intersect(sc(nullptr, nullptr, {{"patient", "me"}}), sc(nullptr, nullptr, {{"patient", "wife"}})).has_value());
}

TEST_CASE("scope: containment; a missing key is a wildcard") {
    const Scope general = sc(nullptr, nullptr);
    const Scope mine = sc(nullptr, nullptr, {{"patient", "me"}});
    CHECK(Scope::contains(general, mine));
    CHECK_FALSE(Scope::contains(mine, general));
    CHECK(Scope::strictly_contains(general, mine));
    CHECK_FALSE(Scope::strictly_contains(mine, mine));
    CHECK(Scope::same(mine, mine));
    CHECK(Scope::contains(sc("2026-01-01", nullptr), sc("2026-02-01", "2026-03-01")));
    CHECK_FALSE(Scope::contains(sc("2026-01-01", "2026-02-15"), sc("2026-02-01", "2026-03-01")));
    // note never takes part
    Scope a = mine, b = mine;
    a.note = "x"; b.note = "y";
    CHECK(Scope::same(a, b));
}

TEST_CASE("value: unit normalization and comparability") {
    auto mg = Value::point(400, "мг");
    CHECK(mg.unit == "kg");
    CHECK(mg.lo == doctest::Approx(0.0004));
    CHECK(mg.original_unit == "мг");
    CHECK(mg.original_lo == 400);
    CHECK(normalize_unit("$").unit == "USD");
    CHECK(normalize_unit(" KZT ").unit == "KZT");
    CHECK(normalize_unit("parsec").unit == "parsec");  // unknown stays, compares by string

    // "около 40 тысяч" vs "43 тысячи": overlapping intervals agree
    CHECK(Value::compatible(Value::number(38000, 42000, "USD"), Value::number(43000, 43000, "USD")) == false);
    CHECK(Value::compatible(Value::number(38000, 44000, "USD"), Value::point(43000, "USD")) == true);
    CHECK_FALSE(Value::compatible(Value::point(1, "USD"), Value::point(1, "KZT")).has_value());  // no exchange rate here
    CHECK(Value::compatible(Value::point(1, "kg"), Value::point(1000, "g")) == true);
    CHECK_FALSE(Value::compatible(Value::text("works"), Value::text("does not work")).has_value());
    CHECK(Value::compatible(Value::boolean(true), Value::boolean(false)) == false);
    CHECK(Value::compatible(Value::enum_member("BB-"), Value::enum_member("BB-")) == true);
    CHECK_FALSE(Value::compatible(Value::point(1), Value::text("1")).has_value());

    // round trip
    auto j = Value::number(1, 2, "см").to_json();
    auto back = Value::from_json(j);
    CHECK(back.type == ValueType::number);
    CHECK(back.unit == "m");
    CHECK(back.lo == doctest::Approx(0.01));
    CHECK(back.original_unit == "см");
}

TEST_CASE("store: assert fills valid_from from the earliest source; derive intersects premises") {
    auto db = Store::open(":memory:", "test");
    auto s1 = db.observe({.kind = SourceKind::document, .uri = "a", .content = "a", .observed_at = "2026-03-01"});
    auto s2 = db.observe({.kind = SourceKind::document, .uri = "b", .content = "b", .observed_at = "2026-01-15"});
    auto rule = db.define_rule({.name = "r"});

    auto a = db.assert_claim({.text = "A", .support = {s1, s2}, .credence = 0.8, .scope = {.note = "n"}});
    auto sa = Scope::from_json(db.get(a)->payload["scope"]);
    CHECK(sa.valid_from == "2026-01-15");
    CHECK_FALSE(sa.valid_to.has_value());

    auto b = db.assert_claim({.text = "B", .support = {s1}, .credence = 0.8, .scope = sc("2026-02-01", "2026-06-30", {{"patient", "me"}})});
    auto d = db.derive({.text = "D", .from = {a, b}, .rule = rule});  // no scope given: pure intersection
    auto sd = Scope::from_json(db.get(d)->payload["scope"]);
    CHECK(sd.valid_from == "2026-02-01");
    CHECK(sd.valid_to == "2026-06-30");
    CHECK(sd.conditions["patient"] == "me");

    // narrowing is fine
    auto d2 = db.derive({.text = "D2", .from = {a, b}, .rule = rule, .scope = sc("2026-03-01", "2026-04-01", {{"dose", "400mg"}})});
    auto sd2 = Scope::from_json(db.get(d2)->payload["scope"]);
    CHECK(sd2.valid_from == "2026-03-01");
    CHECK(sd2.conditions["patient"] == "me");
    CHECK(sd2.conditions["dose"] == "400mg");

    // widening is not
    try { db.derive({.text = "W", .from = {a, b}, .rule = rule, .scope = sc("2026-01-01", nullptr)}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::scope_widens); }
    try { db.derive({.text = "W", .from = {a, b}, .rule = rule, .scope = sc(nullptr, "2026-12-31")}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::scope_widens); }
    try { db.derive({.text = "W", .from = {a, b}, .rule = rule, .scope = sc(nullptr, nullptr, {{"patient", "wife"}})}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::scope_widens); }

    // premises never valid together
    auto c = db.assert_claim({.text = "C", .support = {s1}, .credence = 0.8, .scope = sc("2027-01-01", nullptr)});
    try { db.derive({.text = "E", .from = {b, c}, .rule = rule}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::empty_intersection); }
}

TEST_CASE("store: predicate registry keeps one type per predicate") {
    auto db = Store::open(":memory:", "test");
    auto s = db.observe({.kind = SourceKind::measurement, .uri = "scale", .content = "86", .observed_at = "2026-10-16"});
    // first use registers number/kg
    auto first = db.assert_claim({.text = "weight", .subject = "me", .predicate = "weight", .value = Value::point(86, "кг"), .support = {s}, .credence = 0.95, .method = CredenceMethod::measured});
    auto p = db.get_predicate("weight");
    REQUIRE(p.has_value());
    CHECK(p->value_type == ValueType::number);
    CHECK(p->unit == "kg");
    // same family, other spelling: fine
    CHECK_NOTHROW(db.assert_claim({.text = "weight", .subject = "me", .predicate = "weight", .value = Value::point(86000, "g"), .support = {s}, .credence = 0.95, .method = CredenceMethod::measured, .despite = {first}}));
    // other unit family or other type: rejected
    try { db.assert_claim({.text = "w", .predicate = "weight", .value = Value::point(1.8, "m"), .support = {s}, .credence = 0.8}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::type_mismatch); }
    try { db.assert_claim({.text = "w", .predicate = "weight", .value = Value::text("heavy"), .support = {s}, .credence = 0.8}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::type_mismatch); }
    // enumerations must be declared first, members checked
    try { db.assert_claim({.text = "r", .predicate = "rating", .value = Value::enum_member("BB-"), .support = {s}, .credence = 0.8}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::type_mismatch); }
    db.define_predicate({.name = "rating", .value_type = ValueType::enumeration, .enum_values = {"AAA", "BB-", "B"}});
    CHECK_NOTHROW(db.assert_claim({.text = "r", .predicate = "rating", .value = Value::enum_member("BB-"), .support = {s}, .credence = 0.8}));
    try { db.assert_claim({.text = "r", .predicate = "rating", .value = Value::enum_member("ZZ"), .support = {s}, .credence = 0.8}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::type_mismatch); }
    try { db.define_predicate({.name = "rating", .value_type = ValueType::text}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::duplicate); }
    try { db.define_predicate({.name = "mood", .value_type = ValueType::enumeration}); FAIL("x"); }
    catch (const Error& e) { CHECK(e.code == ErrorCode::bad_input); }
    CHECK(db.list_predicates().size() == 2);
    // untyped predicate name = no registry involvement; stored value is text
    auto id = db.assert_claim({.text = "free", .support = {s}, .credence = 0.5});
    CHECK(db.get(id)->payload["value"]["type"] == "text");
}
