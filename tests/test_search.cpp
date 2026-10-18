// Ф5 (2/3): full-text search, the structural dedup gate, embeddings as a candidate generator.
#include <beliefdb/store.hpp>
#include <doctest/doctest.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <httplib.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <atomic>
#include <thread>

using namespace beliefdb;
using nlohmann::json;

namespace {
template <class F>
ErrorCode code_of(F&& f) {
    try { f(); } catch (const Error& e) { return e.code; }
    return ErrorCode::not_found;
}
template <class F>
std::string message_of(F&& f) {
    try { f(); } catch (const Error& e) { return e.what(); }
    return "";
}
bool has_id(const SimilarResult& r, const std::string& id) {
    return std::any_of(r.entries.begin(), r.entries.end(), [&](const SimilarEntry& e) { return e.id == id; });
}
const SimilarEntry* entry(const SimilarResult& r, const std::string& id) {
    for (const auto& e : r.entries) if (e.id == id) return &e;
    return nullptr;
}

struct World {
    Store db = Store::open(":memory:", "test");
    std::string rule = db.define_rule({.name = "r"});
    std::string s1 = db.observe({.kind = SourceKind::measurement, .uri = "scale", .content = "86", .observed_at = "2026-10-01", .attribution = "scale"});
    std::string s2 = db.observe({.kind = SourceKind::document, .uri = "doc", .content = "x", .observed_at = "2026-10-05", .attribution = "doc"});
    Scope sc(const char* from, const char* to = nullptr) { Scope s; s.valid_from = from; if (to) s.valid_to = to; return s; }
};

/// A stand-in for the bge-m3 server: OpenAI-shaped /v1/embeddings with a
/// deterministic 8-dim vector (byte histogram), so identical texts coincide and
/// unrelated texts do not.
struct FakeEmbedder {
    httplib::Server srv;
    int port = 0;
    std::thread thread;
    std::atomic<int> calls{0};
    std::string model_seen;
    FakeEmbedder() {
        srv.Post("/v1/embeddings", [this](const httplib::Request& req, httplib::Response& res) {
            ++calls;
            const auto body = json::parse(req.body);
            model_seen = body.value("model", "");
            const std::string text = body["input"].get<std::string>();
            std::vector<double> v(8, 0.0);
            for (unsigned char ch : text) v[ch % 8] += 1.0;
            res.set_content(json{{"model", model_seen}, {"object", "list"}, {"data", json::array({json{{"embedding", v}, {"index", 0}}})}}.dump(),
                            "application/json");
        });
        port = srv.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
    }
    ~FakeEmbedder() { srv.stop(); if (thread.joinable()) thread.join(); }
    EmbeddingConfig config(std::string model = "fake-8") const {
        EmbeddingConfig c;
        c.base_url = "http://127.0.0.1:" + std::to_string(port);
        c.model = std::move(model);
        c.timeout_ms = 2000;
        c.backoff_seconds = 1;
        return c;
    }
};
}  // namespace

TEST_CASE("search: trigram full text over claims and hypotheses, every status, ranked") {
    World w;
    auto a = w.db.assert_claim({.text = "вес 86 кг по весам", .subject = "me", .predicate = "weight", .value = Value::point(86, "kg"), .support = {w.s1}, .credence = 0.95, .method = CredenceMethod::measured});
    auto b = w.db.assert_claim({.text = "ибупрофен 400 мг", .subject = "me", .predicate = "ibuprofen", .value = Value::point(400, "mg"), .support = {w.s2}, .credence = 0.8});
    auto h = w.db.hypothesize({.text = "вес снизится", .from = {a}, .rule = w.rule});
    auto r = w.db.search("вес");
    CHECK(r.mode == "fts");
    CHECK(r.notice.find("not configured") != std::string::npos);
    CHECK(has_id(r, a));
    CHECK(has_id(r, h));
    CHECK_FALSE(has_id(r, b));
    CHECK(entry(r, a)->fts_rank.has_value());
    CHECK(entry(r, h)->type == NodeType::hypothesis);
    // substrings match ("весам" contains "веса"), tokens under three characters are ignored
    CHECK(has_id(w.db.search("веса"), a));
    CHECK(w.db.search("мг").entries.empty());
    CHECK(has_id(w.db.search("400 мг ибупрофен"), b));
    // search sees everything; similar() sees only what is live
    w.db.retract(b, "ошибка");
    CHECK(entry(w.db.search("ибупрофен"), b)->status == Status::retracted);
    CHECK_FALSE(has_id(w.db.similar({.text = "ибупрофен"}), b));
    // domain filter
    CHECK(w.db.search("вес", "other").entries.empty());
}

TEST_CASE("dedup gate: a structural duplicate is refused, with the way out in the message") {
    World w;
    auto a = w.db.assert_claim({.text = "вес 86 кг", .subject = "me", .predicate = "weight", .value = Value::point(86, "kg"),
                                .support = {w.s1}, .credence = 0.95, .method = CredenceMethod::measured, .scope = w.sc("2026-10-01")});
    // same thing, compatible value, overlapping scope: refused
    ClaimInput dup{.text = "вес около 86", .subject = "me", .predicate = "weight", .value = Value::number(85, 87, "kg"),
                   .support = {w.s2}, .credence = 0.5, .scope = w.sc("2026-10-05")};
    CHECK(code_of([&] { w.db.assert_claim(dup); }) == ErrorCode::duplicate_claim);
    const auto msg = message_of([&] { w.db.assert_claim(dup); });
    CHECK(msg.find(a) != std::string::npos);
    CHECK(msg.find("supports") != std::string::npos);
    CHECK(msg.find("despite") != std::string::npos);
    CHECK(w.db.counts().claims == 1);
    // the structural candidate is what similar() lists first, with the comparison
    auto sim = w.db.similar({.text = dup.text, .subject = "me", .predicate = "weight", .scope = dup.scope});
    REQUIRE(!sim.entries.empty());
    CHECK(sim.entries[0].id == a);
    CHECK(sim.entries[0].structural);
    // incompatible value: not a duplicate, a contradiction, which the detector surfaces
    ClaimInput other = dup;
    other.value = Value::point(80, "kg");
    auto b = w.db.assert_claim(other);
    CHECK(w.db.conflicts().size() == 1);
    // disjoint scope: a different fact
    ClaimInput past = dup;
    past.scope = w.sc("2020-01-01", "2020-12-31");
    CHECK_NOTHROW(w.db.assert_claim(past));
    // different subject or predicate: no structure to compare
    ClaimInput sub = dup;
    sub.subject = "you";
    CHECK_NOTHROW(w.db.assert_claim(sub));
    // text values are incomparable: never merged automatically, only listed for a human
    auto t1 = w.db.assert_claim({.text = "X работает", .subject = "X", .predicate = "works", .value = Value::text("да"), .support = {w.s1}, .credence = 0.5, .scope = w.sc("2026-01-01")});
    CHECK_NOTHROW(w.db.assert_claim({.text = "X работает нормально", .subject = "X", .predicate = "works", .value = Value::text("да"), .support = {w.s2}, .credence = 0.5, .scope = w.sc("2026-01-01")}));
    auto tsim = w.db.similar({.text = "X работает", .subject = "X", .predicate = "works", .scope = w.sc("2026-01-01")});
    REQUIRE(entry(tsim, t1));
    CHECK(entry(tsim, t1)->structural);
    CHECK_FALSE(entry(tsim, t1)->compatible.has_value());

    // despite: written, recorded, and the caller is expected to link it
    dup.despite = {a};
    auto d = w.db.assert_claim(dup);
    CHECK(w.db.get(d)->payload["despite"] == json::array({a}));
    (void)b;
    // a correction is not a duplicate of what it corrects (d, compatible and acknowledged, is excluded explicitly)
    CHECK_NOTHROW(w.db.supersede(a, ClaimInput{.text = "вес 86.5 кг", .subject = "me", .predicate = "weight", .value = Value::point(86.5, "kg"), .support = {w.s1}, .credence = 0.95, .method = CredenceMethod::measured, .scope = w.sc("2026-10-01"), .despite = {d}}, "точнее"));
}

TEST_CASE("dedup gate: derive and hypothesize go through it; a pending hypothesis counts and asks to be confirmed") {
    World w;
    auto a = w.db.assert_claim({.text = "A", .subject = "x", .predicate = "p", .value = Value::point(1), .support = {w.s1}, .credence = 0.8, .scope = w.sc("2026-01-01")});
    auto h = w.db.hypothesize({.text = "H", .subject = "x", .predicate = "q", .value = Value::point(2), .from = {a}, .rule = w.rule});
    // the same as the hypothesis: confirm it instead
    const auto msg = message_of([&] { w.db.assert_claim({.text = "H again", .subject = "x", .predicate = "q", .value = Value::point(2), .support = {w.s2}, .credence = 0.5, .scope = w.sc("2026-01-01")}); });
    CHECK(msg.find(h) != std::string::npos);
    CHECK(msg.find("confirm") != std::string::npos);
    // derive / hypothesize duplicates of a claim
    CHECK(code_of([&] { w.db.derive({.text = "A2", .subject = "x", .predicate = "p", .value = Value::point(1), .from = {a}, .rule = w.rule}); }) == ErrorCode::duplicate_claim);
    CHECK(code_of([&] { w.db.hypothesize({.text = "A3", .subject = "x", .predicate = "p", .value = Value::point(1), .from = {a}, .rule = w.rule}); }) == ErrorCode::duplicate_claim);
    // confirming the hypothesis is not a duplicate of itself
    CHECK_NOTHROW(w.db.confirm({.hypothesis = h, .support = {w.s2}}));
    // untyped claims without subject/predicate have no structure: nothing to refuse on
    CHECK_NOTHROW(w.db.assert_claim({.text = "free text", .support = {w.s1}, .credence = 0.5}));
    CHECK_NOTHROW(w.db.assert_claim({.text = "free text", .support = {w.s2}, .credence = 0.5}));
}

TEST_CASE("embeddings: vectors are stored with the model, used as a candidate generator, and fail closed to FTS") {
    FakeEmbedder fake;
    World w;
    auto st = w.db.embedding_status();
    CHECK_FALSE(st.configured);
    w.db.set_embeddings(fake.config());

    auto a = w.db.assert_claim({.text = "вес 86 кг", .subject = "me", .predicate = "weight", .value = Value::point(86, "kg"), .support = {w.s1}, .credence = 0.95, .method = CredenceMethod::measured});
    auto b = w.db.assert_claim({.text = "zzzz qqqq", .support = {w.s2}, .credence = 0.5});
    CHECK(fake.calls == 2);
    CHECK(fake.model_seen == "fake-8");
    st = w.db.embedding_status();
    CHECK(st.configured);
    CHECK(st.available);
    CHECK(st.model == "fake-8");
    CHECK(st.dim == 8);
    CHECK(st.n_vectors == 2);
    CHECK(st.n_missing == 0);

    auto r = w.db.similar({.text = "вес 86 кг"});
    CHECK(r.mode == "fts+embeddings");
    CHECK(r.notice.empty());
    REQUIRE(r.entries.size() == 2);
    CHECK(r.entries[0].id == a);
    CHECK(*r.entries[0].cosine == doctest::Approx(1.0));
    CHECK(r.entries[1].id == b);
    CHECK(*r.entries[1].cosine < 0.9);
    CHECK(r.entries[0].fts_rank.has_value());  // found by both
    CHECK_FALSE(r.entries[1].fts_rank.has_value());

    // a confirmed hypothesis passes its vector on to the claim (same text, no second call)
    auto h = w.db.hypothesize({.text = "вес упадёт", .from = {a}, .rule = w.rule});
    const int calls_after_h = fake.calls;
    auto c = w.db.confirm({.hypothesis = h, .support = {w.s2}});
    CHECK(fake.calls == calls_after_h);
    CHECK(w.db.embedding_status().n_vectors == 4);
    CHECK(has_id(w.db.similar({.text = "вес упадёт"}), c));

    // provider down: writes go on, search says so, and the next tries wait for the backoff
    fake.srv.stop();
    fake.thread.join();
    auto d = w.db.assert_claim({.text = "без вектора", .support = {w.s1}, .credence = 0.5});
    st = w.db.embedding_status();
    CHECK_FALSE(st.available);
    CHECK(st.n_missing == 1);
    r = w.db.search("без вектора");
    CHECK(r.mode == "fts");
    CHECK(r.notice.find("unavailable") != std::string::npos);
    CHECK(has_id(r, d));
    const int calls_before = fake.calls;
    w.db.assert_claim({.text = "ещё без вектора", .support = {w.s1}, .credence = 0.5});
    CHECK(fake.calls == calls_before);  // backed off, no stall
    CHECK(code_of([&] { w.db.set_embeddings(EmbeddingConfig{}); }) == ErrorCode::bad_input);  // no default URL

    // provider back (new instance, new port): reindex fills the gaps
    FakeEmbedder fake2;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // backoff of the old failure
    w.db.set_embeddings(fake2.config());
    CHECK(w.db.reindex_embeddings() == 2);
    CHECK(w.db.embedding_status().n_missing == 0);
    // a model change invalidates the stored vectors: nothing mixes, reindex recomputes
    w.db.set_embeddings(fake2.config("fake-8-v2"));
    st = w.db.embedding_status();
    CHECK(st.n_vectors == 0);
    CHECK(st.n_missing == 5);  // a, b, c, d, e; the superseded hypothesis is not live
    CHECK(w.db.similar({.text = "вес 86 кг"}).entries[0].cosine.has_value() == false);
    CHECK(w.db.reindex_embeddings() == 5);
    CHECK(*w.db.similar({.text = "вес 86 кг"}).entries[0].cosine == doctest::Approx(1.0));
}

TEST_CASE("text dedup gate: a restated position of the same domain is refused with the id, despite lets a refinement through, other domains are untouched") {
    FakeEmbedder fake;
    World w;
    w.db.set_embeddings(fake.config());
    auto a = w.db.assert_claim({.text = "aaaa aaaa", .support = {w.s1}, .credence = 0.65, .domain = "d1"});
    // the same words again (cosine 1.0): the gate names the existing claim
    CHECK_THROWS_WITH_AS(w.db.assert_claim({.text = "aaaa aaaa", .support = {w.s2}, .credence = 0.65, .domain = "d1"}), doctest::Contains(a.c_str()), Error);
    // nearly the same bytes (cosine ~0.98): still the same claim
    CHECK_THROWS_WITH_AS(w.db.assert_claim({.text = "aaaa aaab", .support = {w.s2}, .credence = 0.65, .domain = "d1"}), doctest::Contains("cosine"), Error);
    // a different statement passes
    auto b = w.db.assert_claim({.text = "bbbb bbbb", .support = {w.s2}, .credence = 0.65, .domain = "d1"});
    CHECK(b != a);
    // another domain is not a duplicate (the oracles do not share positions)
    auto c = w.db.assert_claim({.text = "aaaa aaaa", .support = {w.s2}, .credence = 0.65, .domain = "d2"});
    CHECK(c != a);
    // despite: a refinement of the same position (new numbers, new date)
    auto d = w.db.assert_claim({.text = "aaaa aaab", .support = {w.s2}, .credence = 0.65, .domain = "d1", .despite = {a}});
    CHECK(d != a);
    // the gate is a policy: 0 disables it
    Policy p; p.text_duplicate_cosine = 0;
    w.db.set_policy(p);
    CHECK(w.db.assert_claim({.text = "aaaa aaaa", .support = {w.s1}, .credence = 0.65, .domain = "d1"}) != a);
}
