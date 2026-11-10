# beliefdb — specification of the component (binary `belief`, library `libbeliefdb`)

_The CLI was renamed from `bdb` to `belief` on 2026-10-16: `bdb` is taken by a Python standard-library module and by Berkeley DB packages on crates.io/npm._

Origin: a design session titled "A belief system with calibration", transferred by the author on 2026-10-16. The brief is kept verbatim, translated from Russian.

---

A pluggable store of a graph of justified claims. A separate component: as SQLite is a
relational database, beliefdb is an epistemic one. Domain-independent. It is the shared
knowledge layer for every "ministry" (agent) of the system it was built for.

WHY. Replace a flat memory.md with a structure where every claim knows: where it came
from (the chain down to primary data), how reliable it is (with an explicit method for the
estimate), what follows from it (so that consequences are reviewed when a premise changes),
what it contradicts (contradictions surface instead of being collapsed).
The main operation of the whole component is why(X): unfold the justification down to the
sources. Everything else serves it.

WHAT IT IS NOT. Not a Bayesian network (correct inference over a hand-built graph yields
confidently wrong numbers). Not an ontology / RDF. Not a vector memory. Not a replacement
for SQLite — SQLite is the transport here.

## MODEL

Node types:

```
source     — primary evidence: a post, a measurement, a document, a person's statement,
             the output of a run. IMMUTABLE: a new version = a new node + a `replaces`
             edge. Fields: kind, uri, content_hash, observed_at, retrieved_at.
claim      — a verifiable statement. Fields: text, subject, predicate, value, credence,
             credence_method, status, scope.
             scope = where it holds (domain, time interval, conditions).
             A claim without a scope is almost always false somewhere else.
hypothesis — a machine conclusion not yet confirmed. A separate type, NOT a claim.
question   — an open question. Fields: text, why_open, acceptance_criterion (what exactly
             closes it), progress. A first-class object, not the absence of an answer.
rule       — a named way of inference + its track record: n_applied, n_correct, brier.
```

Edges:

```
supports       (source|claim) -> claim
contradicts    claim -> claim   (symmetric)
derives_from   claim -> claim   (through a rule)
assumes        claim -> claim   (a tacit premise)
answers        claim -> question
refines        claim -> claim   (narrows the scope)
replaces       any -> any       (supersession; the old node is not deleted)
```

INVARIANTS — checked on write; a violation is an error, not a warning:
1. No claim without justification. Every claim has at least one incoming `supports` or
   `derives_from`. You cannot just "write down what I think".
2. The justification graph is acyclic over `derives_from` and `assumes`. A cycle is a
   write error.
3. Sources are immutable. A correction = a new node + `replaces`.
4. Nothing is deleted. Only the status changes: active | stale | retracted | superseded.
   Physical deletion is done only by a human, by an explicit command.
5. credence_method is mandatory. A number without the origin of the number is refused.

## NUTRITION (the key mechanism, the most important part of this brief)

The author's idea; it solves the problem of correlated evidence structurally, without
probabilistic arithmetic. The analogy: the volume of food is not its nutritional value.

Every claim carries, besides its credence, the SET of source identifiers in the transitive
closure of its support. Not a counter — a set.

```
  claim A  <- source s1, s2
  claim B  <- source s2, s3
  claim C  derives_from A, B
           nutrition(C) = {s1, s2, s3}   -> three sources, NOT four
```

Why this is critical: three posts retelling one original give one source automatically when
the sets are united. Nothing needs to be computed — it is a union.

Nutrition = the cardinality of the set, weighted by source kind:
measured > stated > llm_judgment.

The main consequence: a hypothesis adds NOT A SINGLE calorie — its set is the union of its
parents', there is nowhere else to get any. So a chain of five hypotheses numerically carries
exactly as much as its foundation, and this is visible rather than postulated.

Implementation: a sorted blob or a roaring bitmap per node; union on derive; incremental
recomputation. A dilution metric comes for free: inference depth against the size of the
source set. A claim at the ninth level with two sources is degenerate, however many
justifications are written under it.

## PROBABILITIES: where the truth is and where the lies are

Probabilities MUST NOT be multiplied along the graph. A naive product gives 0.97 where the
honest value is 0.6. That is the main way to get a machine that lies confidently with three
decimal places.

- credence is stored in COARSE STEPS: 0.05 / 0.2 / 0.35 / 0.5 / 0.65 / 0.8 / 0.95.
  The illusion of precision is more harmful than the loss of resolution.
- credence_method is mandatory and enumerable: stated | measured | llm_judgment | rule |
  min | noisy_or | manual. noisy_or only when the independence of the evidence is JUSTIFIED,
  not assumed.
- inputs (JSON) keeps what the number was computed from, so that the computation can be
  checked.
- For derive the default is MIN, not the product. Conservative, stable, does not accumulate
  false confidence.
- LLM estimates are not calibrated. A method earns trust only through predictions: a
  forecast recorded BEFORE the outcome, then Brier. A method without a track record is marked
  uncalibrated wherever it is shown.

The value of the component is in structure, provenance and staleness, NOT in the precision
of the numbers.

## PROPAGATION OF CHANGES (lazy)

When a node is changed or retracted:
1. Walk the graph down along `derives_from` and `assumes`.
2. Every reachable node gets credence.stale = 1. DO NOT RECOMPUTE.
3. If a rule is affected, mark all of its applications.

Recomputation happens on an explicit command or by a background worker. Reading a stale node
returns both the old value and the flag.

Why this way: eager recomputation is expensive and — worse — hides from the human exactly
the moment everything is built for. The right behaviour of an agent is "this conclusion
stands on data that changed a month ago, recompute?", not silently swapping the number.

Retraction does NOT delete consequences: a claim may also stand on other grounds. The
consequence stays, gets flagged, and why() shows the leg that was knocked out.

## CONTRADICTIONS

Detection: an explicit `contradicts` edge, or automatically — two active claims with the same
(subject, predicate) and incompatible values in OVERLAPPING scopes.

Never resolved automatically. conflicts() returns them sorted by the number of dependent
consequences: first what props up the largest part of the graph. Resolution is a human
operation or an explicit investigation: narrow the scope of both (most often both are right,
under different conditions), retract one, or open a question with a closing criterion.

## SELF-REINFORCEMENT: the main danger, kills the project most quietly

The active part produces claims from claims, they become the input of the next run, and in
three weeks the graph is 90% machine speculation referring to itself, each piece looking
justified. why() will honestly unfold a chain made of air.

Hard rules:
1. Machine conclusions are of type hypothesis, a separate namespace.
2. A hypothesis CANNOT be an input for a claim (support, derive). It can be an input for
   another hypothesis (since 2026-10-22): that is a hypothesis of order 1 + max(order of the
   premises). Still no self-reinforcement — the nutrition of the tower is the union of the
   leaves, the chain adds no sources, and retracting a leg makes the whole tower stale.
3. A hypothesis becomes a claim only when confirmed by a source or by a human.
4. A hard quota: N hypotheses per run; no new ones are generated until the old ones are
   reviewed. The queue must JAM, not grow. (This applies while hypotheses wait for a human
   reviewer; when the detective itself reviews them against new data, the queue limit is
   lifted: `open_hypotheses = 0`.)
5. Hypotheses expire: not reviewed within 30 days — automatic retraction. (Cancelled in the
   same mode: `hypothesis_ttl_days = 0`, a hypothesis lives until confirmed or refuted; an
   open question is a normal state.)

## WRITE DISCIPLINE: where the project will really die

The store is the easy part. The hard part is not letting an agent trash the graph. One
careless run devalues a month of careful work. Mandatory gates:

1. assert without support is refused. Always.
2. Before a write — a search over FTS and embeddings. If a close claim already exists, the
   agent must either add `supports` to it, or explicitly create `contradicts` or `refines`.
   Silently creating a duplicate is not allowed.
3. An empty scope is refused.
4. The agent's opinions are not written into the graph. What goes into the graph: what a
   human said, what was measured, what was extracted from a source, what was derived by a
   named rule. Everything else goes into the answer to the user, not into the database.
5. A write quota per session. An agent that wrote forty claims in one run has almost
   certainly been retelling itself.

Hygiene: orphans() once a month — claims standing on sources older than N months without
reconfirmation. Knowledge goes stale silently; that must be visible.

## API (the same in every wrapper)

Writes:
```
  observe(kind, uri, content, observed_at) -> source_id     (dedup by content_hash)
  assert(text, subject, predicate, value, support=[ids], credence, method,
         scope, domain) -> claim_id                          (refused if support is empty)
  derive(text, from=[ids], rule, credence, method) -> claim_id
  hypothesize(text, from=[ids], rule) -> hypothesis_id
  link(src, dst, type, rule=None)
  ask(text, why_open, acceptance_criterion, domain) -> question_id
  retract(id, reason)         — cascades stale flags to the consequences
  supersede(old_id, new)      — the same + a replaces edge
  confirm(hypothesis_id, support=[ids])  — turns it into a claim
```

Reads:
```
  why(id, depth=all)      -> justification tree down to the sources, with credence and
                             method at every level
  nutrition(id)           -> the set of sources, breakdown by kind, oldest/newest dates,
                             inference depth
  what_if(id, new_value)  -> what changes below in the graph, without writing
  stale()                 -> nodes whose premises changed after the last computation
  conflicts(domain)       -> pairs of contradictions, by the weight of consequences
  open_questions(domain)  -> questions with status and progress
  orphans()               -> what has gone stale
  calibration(domain)     -> the track record of methods and rules
```

## SCHEMA (SQLite)

```
nodes(id TEXT PK, type TEXT, domain TEXT, created_at, updated_at,
      status TEXT, sensitivity TEXT, payload JSON)
edges(src TEXT, dst TEXT, type TEXT, rule_id TEXT, weight REAL, created_at,
      PRIMARY KEY(src, dst, type))
credence(node_id TEXT PK, value REAL, method TEXT, inputs JSON, computed_at,
         stale INTEGER DEFAULT 0)
evidence(node_id TEXT PK, source_set BLOB, n_distinct INT, depth INT,
         computed_at)
revisions(id, node_id, at, actor, before JSON, after JSON, reason)
predictions(id, node_id, made_at, p, ci_low, ci_high, resolved_at, outcome, brier)
```

Indexes: (domain, type, status); edges(dst, type) for reverse traversal; FTS5 over claim
text. Graph traversal is a recursive CTE, not a dump into memory.

## TECHNICALLY

- C++20. CMake + FetchContent (NOT vcpkg, not conan).
- SQLite through sqlite_orm (the author's library — all database work goes through it).
  FTS5. sqlite-vec for similar-claim search.
- nlohmann/json for payloads.
- Catch2 or doctest.
- From one codebase: library + CLI + HTTP server (REST). MCP is an optional wrapper ON TOP,
  not the primary interface. The author works through Claude Code and needs an API, not
  MCP-first.
- Domains are isolated by a column, not by separate files.
- Everything local, no external network.
- Secrets only in environment variables; .gitignore from the first commit.

## WHAT NOT TO BUILD (important, saves months)

Do not invent a retrieval layer. Chunking, embeddings, hybrid search, validity intervals of
facts — all of that has been done many times and better than a first attempt will manage. We
checked by searching: Mem0, Letta, Cognee, LangMem, Zep/Graphiti, SuperMemory — all of them
are about retrieval, and none of them does credence with a method or propagates invalidation
along the inference chain. That is, nobody has beliefdb's main operation, and everybody has
retrieval.

Therefore: READ the design of Graphiti (the bitemporal model: when a fact was true versus
when we learned about it; supersession instead of deletion) and implement the same ideas on
our own stack. Their code is not needed — the schema is.

Also, before implementation, read about Truth Maintenance Systems (Doyle 1979, de Kleer's
ATMS 1986). It is exactly this problem, it is forty years old, and the known dead ends are
described there. Cheaper to read than to walk into them.

## HOW THE MINISTRIES USE IT

One database, domains separated by the `domain` column. Rules of interaction:

1. Route incoming material as a COPY, not exclusively. One letter may go to three domains;
   that is cheap, while the loss of information on a misrouting is not, and it happens
   silently.
2. NO correspondence between agents. Ministers asking each other questions is combinatorial
   chatter, and provenance is lost at every hop. Instead: the graph of one domain is read by
   another DIRECTLY, read-only. When domain A derives from a claim of domain B, the edge
   simply crosses the domain boundary, and why() unfolds through it. Provenance intact, no
   messages.
3. A `sensitivity` column on nodes from day one. Personal mail, medical results, finances
   are not given to cross-domain reads by default. Cheaper to do at once than to separate
   later.

## PHASES (stop and show after each)

Phase 1. Core. Nodes, edges, invariants, observe/assert/link/why.
   Acceptance: build a graph of 20 claims by hand; why unfolds the chain to the sources;
   assert without support is refused.

Phase 2. Nutrition. evidence, set union on derive, nutrition(), the dilution metric.
   Acceptance: three claims from one source give n_distinct = 1; a chain of hypotheses does
   not increase nutrition.

Phase 3. Changes. retract, supersede, cascading stale flags, lazy recomputation, what_if.
   Acceptance: I change a source at the foundation — all consequences are flagged, nothing
   is deleted, why shows the knocked-out leg.

Phase 4. Contradictions and questions. conflicts, auto-detection by (subject, predicate,
   scope), ask/open_questions.
   Acceptance: I write two incompatible claims — the system finds them itself and ranks them
   by the weight of consequences.

Phase 5. API and write discipline. HTTP server, write gates, dedup via search, quotas, the
   hypothesis lifecycle with expiry.
   Acceptance: I hold an ordinary conversation through an agent — the graph grows
   meaningfully, without duplicates and without the agent's opinions.

Phase 6. Calibration. predictions, Brier by methods and rules, calibration(), orphans().
   Acceptance: after a month of use it is visible what each estimation method is worth.

## WHAT TO EXPECT, HONESTLY

The value appears at hundreds of claims, not dozens. The first weeks are a warehouse. The
numbers will lie until a track record accumulates: until then trust the structure and the
provenance, not the credence. The main risk is not technical but disciplinary — the write
gates matter more than any optimization.
