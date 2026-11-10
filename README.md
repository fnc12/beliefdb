# beliefdb

An epistemic database: a store of claims that know where they came from.

SQLite holds rows. beliefdb holds **claims**, and for every claim it keeps the chain of justification down to primary **sources** (a document, a measurement, something a person said, an LLM output), a **credence** with an explicit method, what was **derived** from it, and what it **contradicts**. The main operation is `why(X)`: show me the tree of evidence under this claim.

It is a single C++20 library plus a command-line tool (`belief`) and an HTTP service (`beliefd`), all on top of one SQLite file. It was built as the memory of a system of LLM agents, where "the model said so" is not an acceptable source and where several agents paraphrasing the same post must not count as several pieces of evidence.

## Why not a vector database

A vector store answers "what have I seen that sounds like this?". It cannot tell you *why* a statement is believed, whether the three similar statements it found are three independent facts or one source retold three times, what breaks when a source turns out to be wrong, or where two stored statements contradict each other. beliefdb stores those relations explicitly and uses text search (full-text and, optionally, embeddings) only to *find* a claim; what you then get about it comes from the graph, not from similarity.

## Status

| Phase | What | State |
|---|---|---|
| 1 | core: nodes, edges, invariants, `observe` / `assert` / `derive` / `link` / `why` | done |
| 1.5 | structured scope (validity interval, conditions, intersection, nesting), typed values, predicate registry | done |
| 2 | evidence accounting: independent sources, weights, concentration, dilution | done |
| 3 | `retract` / `supersede`, cascading stale flags, lazy `recompute`, `what-if` | done |
| 4 | contradictions (explicit and detected on typed values), refinement candidates, questions | done |
| 5 | write gates: batches with quotas, hypotheses with confirmation and expiry, dedup, search, embeddings, the `beliefd` HTTP service and its read-only UI | done |
| 6 | calibration: predictions, Brier scores, orphan detection | planned |

Everything in phases 1–5 is covered by tests (`beliefdb_tests`, doctest). The design documents in `docs/` (specification, implementation decisions, open issues) are in English.

## Build

Requirements: CMake 3.24 or newer, a C++20 compiler (GCC 11, Clang 14 and Apple Clang 17 are tested), SQLite 3.35 or newer with FTS5 and JSON1 (the system library; `apt install libsqlite3-dev` on Debian/Ubuntu, included with Xcode on macOS).

```sh
cmake -S . -B build
cmake --build build
./build/beliefdb_tests
```

That is all. The other dependencies (nlohmann/json, sqlite_orm, doctest, cpp-httplib) are fetched by CMake with `FetchContent`, pinned to exact versions in `cmake/deps.cmake`. If you keep local checkouts of them under `~/deps` (one directory per dependency), CMake uses those instead of downloading; `-DBELIEFDB_DEPS_DIR=/path` points it elsewhere. This is only a cache for frequent clean builds, not a requirement.

The build produces three binaries in `build/`: `belief` (CLI), `beliefd` (HTTP service) and `beliefdb_tests`.

## A tour in ten commands

`belief` works like `sqlite3`: a database file and a command. The database is `--db PATH`, or `$BELIEF_DB`, or `./beliefdb.sqlite` in the current directory. Every write prints the id of what it created; you pass that id to the next command.

Start with two pieces of evidence about the same thing: a paper and what a doctor said.

```sh
$ belief --db demo.sqlite observe --kind document --uri "paper:doi/10.1000/xyz" \
    --attribution "Smith et al. 2024" --observed-at 2024-05-02 --domain sleep \
    --content "Participants who slept under 6 hours showed a 30% drop in working-memory scores"
s_01M71R953ZHVS1N0N4HPG317E6

$ belief --db demo.sqlite observe --kind human_statement --uri "chat:dr-lee/2026-11-01" \
    --attribution "Dr. Lee" --observed-at 2026-11-01 --domain sleep \
    --content "Less than six hours of sleep costs you a third of your working memory the next day"
s_01M71R955MDZE0KPMCPN7WXMSR
```

A source is always something from the world: `measurement`, `document`, `human_statement` or `llm_output`. There is no source kind "conclusion"; conclusions are claims, and they must stand on sources.

Now state a claim on those two sources. The claim carries a credence with its method (`stated`: taken from the sources as they put it), and a typed value so that it can be compared with other claims later.

```sh
$ belief --db demo.sqlite assert --text "Sleeping under 6 hours lowers working memory by about 30%" \
    --support s_01M71R953ZHVS1N0N4HPG317E6,s_01M71R955MDZE0KPMCPN7WXMSR \
    --credence 0.8 --method stated \
    --subject sleep_deprivation --predicate working_memory_drop --value-type number --value 30 --unit % \
    --domain sleep
c_01M71R9VWF233D3NWE71BRZCZD
```

Derive something from it. A derivation names its rule, and its credence follows from the premises (here the `min` method: a conclusion is no more certain than its weakest premise).

```sh
$ belief --db demo.sqlite rule --name exam_risk --description "a memory drop before an exam lowers the expected score"
r_01M71R9VZWPSF1EDGP0QJ1HCGP

$ belief --db demo.sqlite derive --text "Pulling an all-nighter before the exam will cost points" \
    --from c_01M71R9VWF233D3NWE71BRZCZD --rule exam_risk --domain sleep
c_01M71R9W34DJN4CNQCMCCJSTCD
```

Now the main question: **why** do we believe the derived claim?

```sh
$ belief --db demo.sqlite why c_01M71R9W34DJN4CNQCMCCJSTCD
nutrition: 2 sources, 2 heads, weight 1.30, concentration 0.62, depth 2, dilution 1.00, observed 2024-05-02..2026-11-01
c_01M71R9W34DJN4CNQCMCCJSTCD claim 0.8 min  Pulling an all-nighter before the exam will cost points
└─ derives_from [exam_risk] c_01M71R9VWF233D3NWE71BRZCZD claim 0.8 stated  Sleeping under 6 hours lowers working memory by about 30%
   ├─ supports s_01M71R953ZHVS1N0N4HPG317E6 source  document: paper:doi/10.1000/xyz
   └─ supports s_01M71R955MDZE0KPMCPN7WXMSR source  human_statement: chat:dr-lee/2026-11-01
```

The tree is the justification: every level shows the credence and how it was obtained, down to the sources. The header line is the **evidence summary** (the command is `nutrition`, after the metaphor the project started with: how much does this claim have to live on): two primary sources from two independent people ("heads"), a weight that sums the source kinds, a concentration of 0.62 (the largest source carries 62% of the weight), depth 2 and no dilution.

Finding a claim when you do not know its id is `search`. It is full-text (trigram FTS5, so inflected languages work) and, when an embedding server is configured, semantic as well; the output says which mode you got.

```sh
$ belief --db demo.sqlite search "all-nighter exam"
c_01M71R9W34DJN4CNQCMCCJSTCD (active) bm25 -3.33e-06  sleep  Pulling an all-nighter before the exam will cost points
mode: fts (embeddings not configured; full-text only)
```

Evidence disagrees. Record a replication that found a smaller effect, as a claim on the same subject and predicate with a different value:

```sh
$ belief --db demo.sqlite observe --kind document --uri "paper:doi/10.1000/abc" --attribution "Jones 2025" \
    --observed-at 2025-03-10 --domain sleep --content "A replication found a 10% drop, not 30%"
s_01M71R9WB16EQ4N03GP6CCTJKD

$ belief --db demo.sqlite assert --text "The working-memory drop after short sleep is closer to 10%" \
    --support s_01M71R9WB16EQ4N03GP6CCTJKD --credence 0.65 --method stated \
    --subject sleep_deprivation --predicate working_memory_drop --value-type number --value 10 --unit % --domain sleep
c_01M71R9WDFV0E4JDCYZRT3X0WC
warning: c_01M71R9WDFV0E4JDCYZRT3X0WC contradicts c_01M71R9VWF233D3NWE71BRZCZD [30 %] Sleeping under 6 hours lowers working memory by about 30%; see `belief conflicts`
```

Contradictions are not resolved for you; they surface, ranked by how much stands on them:

```sh
$ belief --db demo.sqlite conflicts
sleep_deprivation working_memory_drop: 1 dependents, detected, open 0d, seen 2x, overlap 2025-03-10..
    c_01M71R9VWF233D3NWE71BRZCZD 0.8 stated  [30 %] 2024-05-02..  Sleeping under 6 hours lowers working memory by about 30%
        2 sources, 1 dependents, domain sleep
    c_01M71R9WDFV0E4JDCYZRT3X0WC 0.65 stated  [10 %] 2025-03-10..  The working-memory drop after short sleep is closer to 10%
        1 sources, 0 dependents, domain sleep
```

Finally, sources go bad. Nothing is ever deleted; retracting a source flags everything that depends on it as stale, and `why` shows exactly which leg was knocked out:

```sh
$ belief --db demo.sqlite retract s_01M71R953ZHVS1N0N4HPG317E6 --reason "paper retracted by the journal"
ok

$ belief --db demo.sqlite why c_01M71R9W34DJN4CNQCMCCJSTCD
nutrition: 1 source, 1 head, weight 0.50, concentration 1.00, depth 2, dilution 2.00, observed 2026-11-01..2026-11-01
c_01M71R9W34DJN4CNQCMCCJSTCD claim (stale) 0.8 min STALE  Pulling an all-nighter before the exam will cost points
└─ derives_from [exam_risk] c_01M71R9VWF233D3NWE71BRZCZD claim (stale) 0.8 stated STALE  Sleeping under 6 hours lowers working memory by about 30%
   ├─ supports s_01M71R953ZHVS1N0N4HPG317E6 source (retracted) ✗  document: paper:doi/10.1000/xyz  [retracted: paper retracted by the journal (2026-11-10)]
   └─ supports s_01M71R955MDZE0KPMCPN7WXMSR source  human_statement: chat:dr-lee/2026-11-01
```

`belief stale` lists what needs a second look, shallowest first; `belief recompute ID` re-evaluates one claim on what is left; `belief supersede OLD …` records a correction and links it to what it replaces. `belief --help` lists every command.

## The model

**Sources** are evidence from the world: `measurement`, `document`, `human_statement`, `llm_output`. Someone else's conclusion whose grounds you cannot see ("our analysis shows X") is recorded as a `document` or `human_statement` with `--directness secondary`, and its weight is halved. A source has a `uri`, an `attribution` (who), an `observed_at` date and a `domain`; observing the same `uri` twice returns the same source.

**Claims** stand on sources (`assert`) or on other claims (`derive`, with a named rule). A claim has a credence from a fixed ladder (0.05, 0.2, 0.35, 0.5, 0.65, 0.8, 0.95) with a method: `stated`, `measured`, `llm_judgment`, `manual` for asserted claims; `min`, `rule`, `noisy_or`, `llm_judgment`, `manual` for derived ones. A claim cannot be written without support (`no_support`), and the graph cannot have cycles.

**Scope** is where a claim holds: a validity interval and conditions (`--valid-from`, `--valid-to`, `--cond key=value`). A derived claim's scope is the intersection of its premises' scopes; flags may only narrow it. **Values** are typed (`bool`, `number` as an interval with a unit, `enum`, `ref`, `text`), and a predicate keeps one type: a `weight` recorded in kilograms cannot later be recorded in metres (`type_mismatch`).

**Evidence** (`nutrition`) is the set of primary sources in the transitive closure under a claim. Three retellings of one document stay one source; a chain of derivations adds none. The summary reports the number of sources, the number of independent heads (distinct attributions), a weight summed by source kind, the concentration (share of the heaviest source) and the dilution (depth divided by number of sources). A hypothesis of order *n* has its weight multiplied by `0.95^(n−1)`: each further step of inference is one more place to be wrong (the factor is configurable; 5% is a guess until calibration).

**Nothing is deleted.** `retract ID --reason R` marks a source, claim or rule and flags every dependent as `STALE`; old values are still served, with the flag. `supersede OLD --reason R <observe|assert|derive options>` writes the correction, links it with a `replaces` edge and flags dependents the same way; in a `replaces` chain exactly one node is active. `recompute ID` re-evaluates one claim on demand; `what-if ID --credence X` shows what would change below, writing nothing.

**Contradictions** are found on typed values, never on text: two live claims with the same subject and predicate, overlapping scope and incompatible values. `conflicts` adds explicit `contradicts` edges, ranks pairs by the number of dependents and remembers the review state: `dismiss A B --reason` ("not a conflict") and `resolve A B --reason` take a pair out of the list until either side changes. If one scope is strictly inside the other, the pair goes to `refinements` instead, with a suggestion to link them `refines`.

**Questions** are first-class: `ask --text … --criterion …` needs a closing criterion, `progress` adds dated notes, `close --resolution` closes; conflicts show the questions opened about them.

**Hypotheses** are what machines are allowed to conclude. `hypothesize --text … --from C1,C2 --rule R` records an `h_` node on the same premises as a derivation, but a hypothesis justifies nothing: it cannot appear in `--support` or in `--from` of a `derive`, and its credence is a ceiling (the minimum over its premises), not an estimate. Hypotheses can stand on hypotheses (order `1 + max(order of premises)`). `confirm HID --support S --credence X --method M` turns one into a claim on real evidence; rejecting one is a normal `retract`. A hypothesis nobody reviewed within `hypothesis_ttl_days` (default 30) expires by itself, and a domain with `open_hypotheses` (default 20) unreviewed ones accepts no new ones until they are dealt with. Both set to 0 means "thoughts may live for years".

**Batches** are the unit of review for agents. `batch begin --purpose "…"` opens a batch for the actor with a declared purpose and prints its id; the following writes (`--batch ID` or `$BELIEF_BATCH`) count against it and are stamped with it in the revision log. Quotas: 20 claims and 5 hypotheses per batch, 50 claims and 10 hypotheses per domain and actor per day; a full or idle (30 minutes) batch closes itself. `batch show ID` lists what was written, `batch reject ID --reason R` retracts all of it. With `require_batch` in the policy, writes outside a batch are refused.

**Duplicates are refused, not merged silently.** A new claim with the same subject and predicate, overlapping scope and a compatible value as a live claim (or an unreviewed hypothesis) is answered with `duplicate_claim` and the id: add your evidence to the existing claim (`link SRC ID supports`), confirm the hypothesis, or pass `--despite ID` and link the new claim `refines` or `contradicts`. Text claims are never compared as text, but a text claim whose embedding is too close (cosine 0.92 by default) to a live claim in the same domain is refused the same way. `similar --text …` shows what an agent must look at before writing: structural matches first, then full-text, then nearest by embedding.

**Search and embeddings.** `search QUERY` runs over all claims and hypotheses: FTS5 with trigrams, plus cosine similarity when `$BELIEF_EMBEDDINGS` points at an OpenAI-style `/v1/embeddings` server (`$BELIEF_EMBEDDING_MODEL`, default `bge-m3`). Embeddings only generate candidates; whether two claims are "the same" is decided on structural fields. Without a server, or when it is down, everything works in full-text mode and says so.

## The service: `beliefd`

`beliefd --config beliefd.json` serves the same operations as JSON over HTTP, for agents that cannot run a binary. Routes mirror the CLI: `POST /observe /assert /derive /hypothesize /confirm /link /retract /supersede /recompute /rule /predicate /ask /progress /close /dismiss /resolve /similar /batch/begin /batch/end /batch/reject /reindex-embeddings`; `GET /health /whoami /node/ID /why/ID /nutrition/ID /what-if/ID?credence= /stale /conflicts /refinements /questions /question/ID /hypotheses /hypothesis/ID /search?q=`.

Actors are declared in the config and authenticated with `Authorization: Bearer <token>`; tokens are read from the environment as `BELIEFD_TOKEN_<NAME>` and never live in the file. An actor is limited to its domains; a node with `sensitivity: private` is invisible to actors of other domains (403 on `GET /node`, a `(private to …)` stub inside `why`, cut from search and lists). The batch travels in the `X-Belief-Batch` header.

```sh
export BELIEFD_TOKEN_RESEARCH=…
B=$(curl -s -X POST -H "Authorization: Bearer $BELIEFD_TOKEN_RESEARCH" localhost:7420/batch/begin \
      -d '{"purpose":"reading the sleep papers"}' | jq -r .id)
curl -s -X POST -H "Authorization: Bearer $BELIEFD_TOKEN_RESEARCH" -H "X-Belief-Batch: $B" localhost:7420/assert \
  -d '{"text":"Sleeping under 6 hours lowers working memory by about 30%","subject":"sleep_deprivation",
       "predicate":"working_memory_drop","value":{"type":"number","value":30,"unit":"%"},
       "support":["s_…"],"credence":0.8,"method":"stated","domain":"sleep"}'
# → {"id":"c_…","conflicts":[…],"similar":[…],"search_mode":"fts+embeddings"}
```

A read-only web page is built in: domains with counters, claims, hypotheses, sources and questions newest first, conflicts, word and meaning search, and "why" for any node. It is enabled by setting `BELIEFD_UI_PASSWORD` (HTTP Basic); without it `/ui` answers 404. An example config and a systemd user unit are in `deploy/`.

## The library

```cpp
#include <beliefdb/store.hpp>

auto db  = beliefdb::Store::open("beliefdb.sqlite", "agent:research");
auto src = db.observe({.kind = "document", .uri = "paper:doi/10.1000/xyz",
                       .content = "…", .observed_at = "2024-05-02"});
auto id  = db.assert_claim({.text = "…", .support = {src}, .credence = 0.8,
                            .method = beliefdb::CredenceMethod::stated, .scope = {.note = "…"}});
auto why = db.why(id);   // WhyNode: the tree down to the sources, credence and method at every level
```

Every error is a `beliefdb::Error` with a `code` (`no_support`, `cycle`, `bad_credence`, `empty_scope`, `duplicate_claim`, `type_mismatch`, …). The library is one target, `beliefdb`; link it and add `include/`.

## Documents

`docs/spec.md` is the original specification, `docs/design-notes.md` records implementation decisions and what was borrowed from truth-maintenance systems and Graphiti, `docs/open-issues.md` lists what the first production load showed. All in English, translated from the original Russian session notes.

## License

Dual-licensed, the same way as [sqlite_orm](https://github.com/fnc12/sqlite_orm): free under the [AGPL-3.0](LICENSE) for open-source projects, whatever their own license; a commercial license (MIT terms) for closed-source use, whatever its license. For a commercial license write to fnc12@me.com.
