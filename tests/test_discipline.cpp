// phase 5 (1/3): batches with quotas, hypotheses with confirmation and expiry.
#include <beliefdb/store.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace beliefdb;

namespace {
bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

struct World {
    Store db = Store::open(":memory:", "agent");
    std::string rule = db.define_rule({.name = "r"});
    std::string s1 = db.observe({.kind = SourceKind::document, .uri = "u1", .content = "one", .observed_at = "2026-01-01", .attribution = "x"});
    std::string s2 = db.observe({.kind = SourceKind::measurement, .uri = "u2", .content = "two", .observed_at = "2026-01-02", .attribution = "y"});
    std::string a = db.assert_claim({.text = "A", .support = {s1}, .credence = 0.8});
    std::string b = db.assert_claim({.text = "B", .support = {s2}, .credence = 0.65});
    ClaimInput claim(const char* text) { return {.text = text, .support = {s1}, .credence = 0.5}; }
};

template <class F>
ErrorCode code_of(F&& f) {
    try { f(); } catch (const Error& e) { return e.code; }
    return ErrorCode::not_found;  // nothing thrown: callers CHECK for a specific code anyway
}
}  // namespace

// ---- batches -----------------------------------------------------------------

TEST_CASE("batch: writes are tagged and counted, the entry lists what was created") {
    World w;
    CHECK_FALSE(w.db.current_batch());
    auto bid = w.db.begin_batch("going through the backlog");
    CHECK(bid.rfind("b_", 0) == 0);
    CHECK(w.db.current_batch() == bid);
    CHECK(w.db.counts().open_batches == 1);
    CHECK_THROWS_AS(w.db.begin_batch("second"), Error);  // one at a time

    auto s = w.db.observe({.kind = SourceKind::document, .uri = "u3", .content = "three", .observed_at = "2026-01-03"});
    auto c = w.db.assert_claim(w.claim("C"));
    auto d = w.db.derive({.text = "D", .from = {c}, .rule = w.rule});
    auto h = w.db.hypothesize({.text = "H", .from = {c}, .rule = w.rule});
    auto e = w.db.batch(bid);
    CHECK(e.actor == "agent");
    CHECK(e.purpose == "going through the backlog");
    CHECK(e.closed_at.empty());
    CHECK(e.n_sources == 1);
    CHECK(e.n_claims == 2);
    CHECK(e.n_hypotheses == 1);
    CHECK(e.nodes == std::vector<std::string>{s, c, d, h});

    w.db.end_batch();
    e = w.db.batch(bid);
    CHECK_FALSE(e.closed_at.empty());
    CHECK(e.closed_reason == "ended");
    CHECK(w.db.counts().open_batches == 0);
    CHECK(w.db.batches().empty());
    CHECK(w.db.batches(true).size() == 1);
    CHECK_FALSE(w.db.current_batch());
    CHECK_THROWS_AS(w.db.end_batch(), Error);     // no current batch
    CHECK_THROWS_AS(w.db.end_batch(bid), Error);  // already closed

    // an explicit end is a declaration: writing goes on unbatched (the policy allows it)
    auto e2 = w.db.assert_claim(w.claim("E"));
    auto b2 = w.db.begin_batch("next");
    auto f = w.db.assert_claim(w.claim("F"));
    CHECK(w.db.batch(b2).nodes == std::vector<std::string>{f});
    CHECK(w.db.batch(bid).nodes.size() == 4);  // untouched
    for (const auto& b : w.db.batches(true)) CHECK_FALSE(has(b.nodes, e2));
}

TEST_CASE("batch quota: the batch closes when full, the next write is refused with a reason") {
    World w;
    Policy p;
    p.batch_claims = 2;
    p.batch_hypotheses = 1;
    w.db.set_policy(p);
    auto bid = w.db.begin_batch("quota test");
    w.db.assert_claim(w.claim("C1"));
    w.db.assert_claim(w.claim("C2"));
    CHECK(code_of([&] { w.db.assert_claim(w.claim("C3")); }) == ErrorCode::quota_exceeded);
    auto e = w.db.batch(bid);
    CHECK(e.n_claims == 2);
    CHECK(e.closed_reason == "quota: 2 claims");
    CHECK(w.db.counts().claims == 4);  // A, B, C1, C2: the refused one was not written
    // an auto-close is sticky: the closed batch stays current and refuses everything until a new one begins
    CHECK(code_of([&] { w.db.assert_claim(w.claim("C4")); }) == ErrorCode::no_batch);
    CHECK(code_of([&] { w.db.observe({.kind = SourceKind::document, .uri = "u4", .content = "four"}); }) == ErrorCode::no_batch);
    CHECK(w.db.current_batch() == bid);

    auto b2 = w.db.begin_batch("hypotheses");
    w.db.hypothesize({.text = "H1", .from = {w.a}, .rule = w.rule});
    CHECK(code_of([&] { w.db.hypothesize({.text = "H2", .from = {w.a}, .rule = w.rule}); }) == ErrorCode::quota_exceeded);
    CHECK(w.db.batch(b2).closed_reason == "quota: 1 hypotheses");
}

TEST_CASE("daily quota: per (domain, actor), across batches; another domain or actor is not affected") {
    World w;
    Policy p;
    p.daily_claims = 3;  // A and B already count
    w.db.set_policy(p);
    w.db.begin_batch("1");
    w.db.assert_claim(w.claim("C"));
    CHECK(code_of([&] { w.db.assert_claim(w.claim("D")); }) == ErrorCode::quota_exceeded);
    w.db.end_batch();
    w.db.begin_batch("2");  // opening another batch does not help
    CHECK(code_of([&] { w.db.assert_claim(w.claim("D")); }) == ErrorCode::quota_exceeded);
    // the batch is not closed by a daily refusal: it is the actor's day that is over
    CHECK(w.db.batch(*w.db.current_batch()).closed_at.empty());

    auto other = w.claim("D");
    other.domain = "other";
    CHECK_NOTHROW(w.db.assert_claim(other));
    w.db.end_batch();
    w.db.set_actor("human");
    CHECK_NOTHROW(w.db.assert_claim(w.claim("D")));
}

TEST_CASE("require_batch: with the policy on, nothing is written outside a batch") {
    World w;
    Policy p;
    p.require_batch = true;
    w.db.set_policy(p);
    CHECK(code_of([&] { w.db.assert_claim(w.claim("C")); }) == ErrorCode::no_batch);
    CHECK(code_of([&] { w.db.observe({.kind = SourceKind::document, .uri = "u3", .content = "three"}); }) == ErrorCode::no_batch);
    CHECK(code_of([&] { w.db.hypothesize({.text = "H", .from = {w.a}, .rule = w.rule}); }) == ErrorCode::no_batch);
    w.db.begin_batch("ok");
    CHECK_NOTHROW(w.db.assert_claim(w.claim("C")));
}

TEST_CASE("idle batch closes itself on the next write") {
    World w;
    Policy p;
    p.batch_idle_seconds = 0.2;
    w.db.set_policy(p);
    auto bid = w.db.begin_batch("a slow minister");
    w.db.assert_claim(w.claim("C"));
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    CHECK(code_of([&] { w.db.assert_claim(w.claim("D")); }) == ErrorCode::no_batch);
    CHECK(w.db.batch(bid).closed_reason == "idle");
    CHECK(w.db.batch(bid).n_claims == 1);
}

TEST_CASE("resume_batch: another handle of the same actor continues it; others are refused") {
    const std::string path = (std::filesystem::temp_directory_path() /
                              ("beliefdb-batch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite")).string();
    std::string bid, s1;
    {
        auto db = Store::open(path, "agent");
        s1 = db.observe({.kind = SourceKind::document, .uri = "u1", .content = "one", .observed_at = "2026-01-01"});
        bid = db.begin_batch("via the CLI");
        db.assert_claim({.text = "A", .support = {s1}, .credence = 0.5});
    }
    {
        auto db = Store::open(path, "agent");
        CHECK_FALSE(db.current_batch());
        db.resume_batch(bid);
        db.assert_claim({.text = "B", .support = {s1}, .credence = 0.5});
        CHECK(db.batch(bid).n_claims == 2);
        CHECK(db.batch(bid).nodes.size() == 2);
    }
    {
        auto db = Store::open(path, "someone_else");
        CHECK(code_of([&] { db.resume_batch(bid); }) == ErrorCode::no_batch);
        db.set_actor("agent");
        db.resume_batch(bid);
        db.end_batch();
        CHECK(code_of([&] { db.resume_batch(bid); }) == ErrorCode::no_batch);  // closed
    }
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

TEST_CASE("reject_batch: everything created in it is retracted, dependents elsewhere go stale") {
    World w;
    auto bid = w.db.begin_batch("a bad run");
    auto s = w.db.observe({.kind = SourceKind::llm_output, .uri = "llm", .content = "guess", .observed_at = "2026-01-05"});
    auto c = w.db.assert_claim({.text = "C", .support = {s}, .credence = 0.5});
    auto d = w.db.derive({.text = "D", .from = {c}, .rule = w.rule});
    w.db.end_batch();
    auto outside = w.db.derive({.text = "outside", .from = {d, w.a}, .rule = w.rule});

    w.db.reject_batch(bid, "retelling itself");
    for (const auto& id : {s, c, d}) {
        auto n = w.db.get(id);
        CHECK(n->status == Status::retracted);
        CHECK(n->payload["retracted"]["reason"].get<std::string>().find("retelling") != std::string::npos);
    }
    CHECK(w.db.get(outside)->status == Status::stale);
    CHECK(w.db.get(w.a)->status == Status::active);
    CHECK(w.db.batch(bid).closed_reason == "rejected: retelling itself");
    CHECK_NOTHROW(w.db.reject_batch(bid, "once more"));  // idempotent on already retracted nodes
}

// ---- hypotheses --------------------------------------------------------------

TEST_CASE("hypothesis: justifies no claim, min credence as a ceiling, zero calories of its own, invisible to the detector") {
    World w;
    auto h = w.db.hypothesize({.text = "H", .subject = "x", .predicate = "p", .value = Value::point(1), .from = {w.a, w.b}, .rule = w.rule});
    CHECK(h.rfind("h_", 0) == 0);
    auto n = w.db.get(h);
    CHECK(n->type == NodeType::hypothesis);
    CHECK(n->status == Status::active);
    REQUIRE(n->credence);
    CHECK(n->credence->value == doctest::Approx(0.65));  // min(A 0.8, B 0.65)
    CHECK(n->credence->method == CredenceMethod::min);
    CHECK(n->credence->inputs["ceiling"] == true);
    CHECK(w.db.counts().hypotheses == 1);
    CHECK(w.db.counts().claims == 2);

    auto nut = w.db.nutrition(h);
    CHECK(nut.sources == std::vector<std::string>{std::min(w.s1, w.s2), std::max(w.s1, w.s2)});
    CHECK(nut.depth == 2);
    auto why = w.db.why(h);
    CHECK(why.children.size() == 2);
    CHECK(why.children[0].via == EdgeType::derives_from);

    // a hypothesis justifies no claim: not a support, not a premise of a derivation (another hypothesis may stand on it, see "higher order")
    CHECK(code_of([&] { w.db.assert_claim({.text = "C", .support = {h}, .credence = 0.5}); }) == ErrorCode::invalid_support);
    CHECK(code_of([&] { w.db.derive({.text = "C", .from = {h}, .rule = w.rule}); }) == ErrorCode::invalid_support);
    CHECK(code_of([&] { w.db.link(w.s1, h, EdgeType::supports); }) == ErrorCode::bad_input);
    // the rule is not "applied" until a claim comes out of it
    CHECK(w.db.get(w.rule)->payload["n_applied"] == 0);

    // the detector looks at claims only: an incompatible claim with the same subject/predicate is no conflict
    auto c = w.db.assert_claim({.text = "C", .subject = "x", .predicate = "p", .value = Value::point(2), .support = {w.s1}, .credence = 0.5});
    (void)c;
    CHECK(w.db.conflicts().empty());

    auto list = w.db.hypotheses();
    REQUIRE(list.size() == 1);
    CHECK(list[0].id == h);
    CHECK(list[0].from.size() == 2);
    CHECK(list[0].rule == w.rule);
    CHECK(list[0].outcome.empty());
    CHECK(list[0].days_left > 29.9);
    CHECK(list[0].days_left <= 30.0);
    CHECK(list[0].batch_id.empty());
}

TEST_CASE("confirm: evidence turns the hypothesis into a claim that keeps its derivation") {
    World w;
    auto h = w.db.hypothesize({.text = "H", .from = {w.a}, .rule = w.rule});
    CHECK(code_of([&] { w.db.confirm({.hypothesis = h, .support = {}}); }) == ErrorCode::no_support);
    CHECK(code_of([&] { w.db.confirm({.hypothesis = h, .support = {w.s2}, .credence = 0.7}); }) == ErrorCode::bad_credence);
    CHECK(code_of([&] { w.db.confirm({.hypothesis = h, .support = {w.s2}, .method = CredenceMethod::min}); }) == ErrorCode::bad_credence);
    CHECK(code_of([&] { w.db.confirm({.hypothesis = w.a, .support = {w.s2}}); }) == ErrorCode::bad_input);

    auto c = w.db.confirm({.hypothesis = h, .support = {w.s2}, .credence = 0.8, .method = CredenceMethod::measured});
    CHECK(c.rfind("c_", 0) == 0);
    auto n = w.db.get(c);
    CHECK(n->type == NodeType::claim);
    CHECK(n->payload["text"] == "H");
    CHECK(n->payload["confirmed_from"] == h);
    CHECK_FALSE(n->payload.contains("expires_at"));
    CHECK(n->credence->value == doctest::Approx(0.8));
    CHECK(n->credence->method == CredenceMethod::measured);
    CHECK(has(w.db.depends_on(c), w.a));
    CHECK(has(w.db.depends_on(c), w.s2));
    auto nut = w.db.nutrition(c);
    CHECK(nut.n_sources == 2);  // s1 through A, s2 directly
    CHECK(w.db.get(w.rule)->payload["n_applied"] == 1);

    auto hn = w.db.get(h);
    CHECK(hn->status == Status::superseded);
    CHECK(w.db.why(h).notice->find("confirmed") != std::string::npos);
    CHECK(w.db.hypotheses().empty());
    auto all = w.db.hypotheses("", true);
    REQUIRE(all.size() == 1);
    CHECK(all[0].outcome == "confirmed as " + c);
    CHECK(code_of([&] { w.db.confirm({.hypothesis = h, .support = {w.s2}}); }) == ErrorCode::bad_input);  // reviewed already
    // the confirmed claim is a claim like any other: usable as a premise
    CHECK_NOTHROW(w.db.derive({.text = "D", .from = {c}, .rule = w.rule}));
}

TEST_CASE("reject and expire: retract() is the rejection; unreviewed hypotheses expire lazily") {
    World w;
    auto h1 = w.db.hypothesize({.text = "H1", .from = {w.a}, .rule = w.rule});
    w.db.retract(h1, "nonsense");
    CHECK(w.db.hypothesis(h1).outcome == "rejected: nonsense");
    CHECK(w.db.hypotheses().empty());

    Policy p;
    p.hypothesis_ttl_days = 0.2 / 86400.0;  // 200 ms
    w.db.set_policy(p);
    auto h2 = w.db.hypothesize({.text = "H2", .from = {w.a}, .rule = w.rule});
    CHECK(w.db.hypotheses().size() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    CHECK(w.db.hypotheses().empty());  // the listing expired it
    auto e = w.db.hypothesis(h2);
    CHECK(e.status == Status::retracted);
    CHECK(e.outcome == "expired");
    CHECK(code_of([&] { w.db.confirm({.hypothesis = h2, .support = {w.s2}}); }) == ErrorCode::bad_input);
}

TEST_CASE("open-hypotheses quota: the queue backs up until somebody reviews it") {
    World w;
    Policy p;
    p.open_hypotheses = 2;
    w.db.set_policy(p);
    auto h1 = w.db.hypothesize({.text = "H1", .from = {w.a}, .rule = w.rule});
    w.db.hypothesize({.text = "H2", .from = {w.a}, .rule = w.rule});
    CHECK(code_of([&] { w.db.hypothesize({.text = "H3", .from = {w.a}, .rule = w.rule}); }) == ErrorCode::quota_exceeded);
    auto other = HypothesisInput{.text = "H3", .from = {w.a}, .rule = w.rule, .domain = "other"};
    CHECK_NOTHROW(w.db.hypothesize(other));  // the queue is per domain
    w.db.retract(h1, "reviewed");
    CHECK_NOTHROW(w.db.hypothesize({.text = "H3", .from = {w.a}, .rule = w.rule}));

    // a stale hypothesis still waits for review and still occupies the queue
    w.db.retract(w.s1, "gone");
    CHECK(w.db.hypotheses("general").size() == 2);
    CHECK(w.db.hypotheses("general")[0].status == Status::stale);
    CHECK(code_of([&] { w.db.hypothesize({.text = "H4", .from = {w.b}, .rule = w.rule}); }) == ErrorCode::quota_exceeded);
}

TEST_CASE("policy zeros: hypotheses never expire and the open queue has no limit") {
    World w;
    Policy p;
    p.hypothesis_ttl_days = 0;
    p.open_hypotheses = 0;
    p.daily_hypotheses = 100;
    w.db.set_policy(p);
    for (int i = 0; i < 25; ++i)  // past the default queue of 20
        CHECK_NOTHROW(w.db.hypothesize({.text = "H" + std::to_string(i), .from = {w.a}, .rule = w.rule}));
    auto list = w.db.hypotheses();
    REQUIRE(list.size() == 25);
    CHECK(list[0].expires_at.empty());
    CHECK(list[0].days_left == 0);
    CHECK(list[0].order == 1);
    CHECK_FALSE(w.db.get(list[0].id)->payload.contains("expires_at"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(w.db.hypotheses().size() == 25);  // nothing expired on the way through the listing
}

TEST_CASE("higher order: a hypothesis may stand on hypotheses; order, ceiling, calories and staleness follow the leaves") {
    World w;
    auto h1 = w.db.hypothesize({.text = "H1", .from = {w.a}, .rule = w.rule});
    auto h2 = w.db.hypothesize({.text = "H2", .from = {w.b}, .rule = w.rule});
    CHECK(w.db.hypothesis(h1).order == 1);
    // a claim never stands on a hypothesis, as before
    CHECK(code_of([&] { w.db.derive({.text = "D", .from = {h1}, .rule = w.rule}); }) == ErrorCode::invalid_support);
    CHECK(code_of([&] { w.db.assert_claim({.text = "C", .support = {h1}, .credence = 0.5}); }) == ErrorCode::invalid_support);

    auto h12 = w.db.hypothesize({.text = "H12", .from = {h1, h2}, .rule = w.rule});
    auto e = w.db.hypothesis(h12);
    CHECK(e.order == 2);
    CHECK(e.from.size() == 2);
    CHECK(e.credence->value == doctest::Approx(0.65));  // min over the premises: B's ceiling through H2
    auto nut = w.db.nutrition(h12);
    CHECK(nut.n_sources == 2);  // s1 through A/H1, s2 through B/H2: the union of the leaves, nothing of its own
    CHECK(has(w.db.depends_on(h12), w.s1));
    CHECK(has(w.db.depends_on(h12), w.s2));

    auto h3 = w.db.hypothesize({.text = "H3", .from = {h12, w.a}, .rule = w.rule});
    CHECK(w.db.hypothesis(h3).order == 3);
    CHECK(w.db.nutrition(h3).n_sources == 2);

    // confirming a higher-order hypothesis: the claim stands on the support and the claim premises; the hypotheses it was
    // built on are remembered, not derivation edges
    auto c = w.db.confirm({.hypothesis = h3, .support = {w.s2}, .credence = 0.65, .method = CredenceMethod::stated});
    auto n = w.db.get(c);
    CHECK(n->type == NodeType::claim);
    CHECK_FALSE(n->payload.contains("order"));
    REQUIRE(n->payload.contains("built_on"));
    CHECK(n->payload["built_on"] == nlohmann::json::array({h12}));
    CHECK(has(w.db.depends_on(c), w.a));
    CHECK(has(w.db.depends_on(c), w.s2));
    CHECK_FALSE(has(w.db.depends_on(c), h12));
    CHECK(w.db.hypothesis(h3).outcome == "confirmed as " + c);

    // a premise retracted: the tower above it goes stale, nothing is deleted; the confirmed claim stands on its own evidence
    auto h4 = w.db.hypothesize({.text = "H4", .from = {h12}, .rule = w.rule});
    w.db.retract(h1, "nonsense");
    CHECK(w.db.hypothesis(h12).status == Status::stale);
    CHECK(w.db.hypothesis(h4).status == Status::stale);
    CHECK(w.db.hypothesis(h2).status == Status::active);
    CHECK(w.db.get(c)->status == Status::active);
    CHECK_FALSE(w.db.get(c)->credence->stale);
}

TEST_CASE("the same document observed again is the same source: by content hash and by uri") {
    World w;
    CHECK(w.db.observe({.kind = SourceKind::document, .uri = "u1", .content = "one", .observed_at = "2026-01-05", .attribution = "x"}) == w.s1);   // same content
    CHECK(w.db.observe({.kind = SourceKind::human_statement, .uri = "u1", .content = "a different quote", .observed_at = "2026-01-06"}) == w.s1);  // same uri
    CHECK(w.db.observe({.kind = SourceKind::document, .uri = "", .content = "one more"}) != w.s1);  // no uri: only the hash can match
    CHECK(w.db.counts().sources == 3);
}
