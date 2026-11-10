# Implementation decisions

A companion to [spec.md](spec.md): what was taken from the literature and which decisions were made where the specification leaves room. Updated phase by phase.

## From Truth Maintenance Systems (Doyle 1979, de Kleer 1986)

**What we take.**
- The separation "premise / conclusion with a justification" and the rule "a node is believed while it has at least one valid justification". Here it is invariant 1 (a claim without supports/derives_from does not exist) and the status `active` only with a living justification.
- The justification as an explicit record, not a side effect. A JTMS keeps a support list per node; here these are the `supports` / `derives_from` / `assumes` edges plus `credence.inputs`.
- Retraction does not delete consequences, it reviews their status. In a JTMS that is IN/OUT relabelling over the network; here it is a lazy `stale` flag (phase 3) with explicit recomputation, because our statuses are not boolean.
- From the ATMS: the idea that a node carries the **set** of premises it stands on (label = set of environments). Nutrition (phase 2) is an ATMS label reduced to the set of primary sources: not "under which assumptions it holds" but "on which evidence it stands". The union of sets on derivation repeats the union of environments one to one.
- Also from the ATMS: a contradiction (nogood) is not resolved automatically; it is recorded and excludes contexts. Here: `contradicts` + `conflicts()` (phase 4), show, do not decide.

**What we do not take, and why.**
- Full relabelling on every change (JTMS) and exponential label growth (ATMS). A known dead end: the cost of computing labels grows with the number of assumptions. Our sets are bounded by primary sources (their number grows linearly with observations, not with combinations), and recomputation is lazy.
- Dependency-directed backtracking. That is a search over the assumption space to restore consistency automatically; the specification explicitly forbids automatic resolution of contradictions.
- JTMS "odd loops" (a node justified through its own negation) are impossible by construction: the justification graph is acyclic, negation is not a node, a contradiction is a separate edge.
- Non-boolean statuses: a TMS works with IN/OUT, here credence comes in steps. Hence MIN instead of logical AND: the minimum confidence along the chain is the conservative analogue of "a conclusion is no more reliable than its weakest premise".

## From Graphiti / Zep (the bitemporal model)

We take the four time dimensions but lay them out differently:
- Event time (`t_valid` / `t_invalid` in Graphiti) is `scope.valid_from` / `scope.valid_to` on a claim here. It is the claim's area of validity, not storage metadata.
- Transaction time (`t_created` / `t_expired`) is the node's `created_at` and the moment of a `status` change (written to `revisions`). Nothing is deleted: invalidation = `superseded` + a `replaces` edge (phase 3), exactly as Graphiti sets `t_invalid` on the old edge instead of deleting it.
- What we do not take: Graphiti detects contradictions by LLM comparison and **always gives priority to the new fact**. Unacceptable for us: a new post must not silently displace a measurement. Contradictions surface and are ranked by the weight of consequences (phase 4); priority is decided by a human or by nutrition.
- The three-level hierarchy (episodes → entities → communities) is not reproduced: episodes are our `source`, entities are the claim's `subject`, there are no communities. Per the specification, no retrieval layer is built.

## Phase 1 decisions

| Question | Decision | Why |
|---|---|---|
| Identifiers | ULID with a type prefix: `s_`, `c_`, `h_`, `q_`, `r_` | sort by time; the type is visible in `why` output and in agent logs |
| Time | ISO-8601 UTC strings | readable in `sqlite3`, sorts lexicographically; no date parser on the C++ side |
| JSON fields | `payload`, `inputs`, `scope` stored as TEXT, searched with `json_extract` | no indexes on them yet; they will appear as queries demand (e.g. by `content_hash`) |
| Credence steps | a value off the ladder is an error, not a rounding | silent rounding would hide that the caller invented precision |
| Methods per call | `assert`: stated / measured / llm_judgment / manual; `derive`: min / rule / noisy_or / llm_judgment / manual | the method must match the way the number was obtained; `min` makes no sense for an observation |
| `derive` without credence | allowed only with method `min`, the value = minimum over the premises | the only method computable without external data |
| Scope of a derived claim | mandatory, as for any claim | a derived claim is still a claim; scope inheritance through interval intersection was deferred to phase 1.5, where the scope format appeared |
| Edge direction | as in the spec: `supports` evidence→claim, `derives_from` / `assumes` claim→premise | one "dependency" computed in one place (`Impl::closure`) |
| Acyclicity | one DAG over the union of `supports`, `derives_from`, `assumes` (`Impl::closure` walks all three at once) | a cycle alternating types would otherwise pass; confirmed in addendum 1, item 7 |
| Graph traversal | BFS by levels with `IN (...)`, not a recursive CTE | the dependency mixes edge directions by type, which in a CTE is a union subquery; switching to sqlite_orm's `with_recursive` when profiling says so |
| `contradicts` | one row in canonical form (min(id), max(id)); src/dst carry no direction | the primary key itself rejects the mirrored duplicate (addendum 1, item 9) |
| Source dedup | by SHA-256 of the content, the first record wins | a repeated observation does not create a node and does not change the first one's `uri` |
| Source kind | enum `measurement / document / human_statement / llm_output`; separately `attribution` (who said it) and an optional `trust_hint` marked as manual | the nature of the evidence and the identity are separated; reliability is a track record per attribution in phase 6 (addendum 1, item 3). There is no `derived` kind: a conclusion is a claim with `derives_from`, not evidence (addendum 2) |
| Source directness | `directness: primary / secondary`, default primary; stored in the payload | "a table of measurements" and "someone's conclusion from measurements we cannot see" differ, but that is a property, not a kind (addendum 2, case A) |
| A source bridging domains | `observe` refuses a source whose `uri` or `content` contains an id of a node of this store (`bridge_source`) | a cross-domain premise is a `derives_from`/`supports` edge onto the foreign node; otherwise `why()` would hit a fake source and provenance would be lost (addendum 2, case B) |
| Credence off the ladder | the error names the two nearest steps | a conscious choice, not a guess (addendum 1, item 6) |
| Transactions | every API call is one transaction (`transaction_guard`) | a partially written claim without edges would violate invariant 1 |
| Database file | WAL + `synchronous=NORMAL`, `open_forever` | few fsyncs, a quiet disk; with WAL, NORMAL keeps integrity across a power loss |
| Tests | doctest, in-memory database | fast, no artifacts |

## Phase 1.5 decisions

| Question | Decision | Why |
|---|---|---|
| Comparing scope dates | ISO-8601 strings, lexicographically; an unset `valid_from` = −∞, `valid_to` = +∞ ("still in force") | no date parser; one precision per axis within a domain is the writer's responsibility |
| Scope intersection | later start, earlier end, union of conditions; a shared key with different values = empty | a missing key = wildcard (addendum 1, item 1B) |
| `valid_from` on assert | by default the earliest `observed_at` among the supporting sources; with claim premises without an interval the scope must be given explicitly | addendum 1, item 1A |
| Scope on derive | = the intersection of the premises; the caller's flags may only narrow (axis by axis); a conflicting condition or a widened interval = `scope_widens`, an empty intersection of premises = `empty_intersection` | the invariant scope(derived) ⊆ ∩ scope(premises) (item 1C) |
| Value types | bool, number [lo, hi] + unit, enum, ref, text; `Value::compatible` returns nullopt for text, different types and different units | comparability is a property of the type (item 2) |
| Units | an alias table (mass→kg, length→m, time→s, %, currency codes); the original is kept alongside | currencies are not converted, USD and KZT are simply incomparable; a full unit system is not needed |
| Predicate registry | the `predicates` table; bool/number/ref/text are registered on first use, enum only through `define_predicate` with its members | the member set cannot be inferred from one value |
| Empty predicate name | the registry is not involved, the value is stored as is | claims without structure are allowed, but the detector does not compare them |

## Phase 2 decisions

| Question | Decision | Why |
|---|---|---|
| Storing the set | the `evidence(node_id, source_set BLOB, n_sources, depth, computed_at)` table; the blob = sorted ids joined by `\n` | ids are short, sets are hundreds of elements; roaring is not needed, and the blob is readable in `sqlite3` |
| When it is computed | at `assert`/`derive` immediately; on `link` with an evidential edge the node and all dependents are recomputed to a fixed point | dependents form a DAG, no more passes than the depth; laziness is not needed here, the union is cheap |
| The attribution set | not stored, computed on read from the sources in the set | kind and dates are needed too, the sources are loaded anyway |
| A source without attribution | counts as its own "head" and goes into `n_unattributed` | otherwise five anonymous posts would collapse into zero heads |
| Weights | `NutritionWeights` on the Store: measurement 1.0, document 0.8, human_statement 0.5, llm_output 0.1; `secondary_factor` 0.5 multiplies the kind's weight for secondary, llm_output ignores it; `n_secondary` reported separately | values from addendum 1, item 5 and addendum 2 |
| Weighted sum and concentration | not persisted, computed on read | the weights are configuration and may change without a migration |
| Depth | a source is 0, a claim on sources is 1, then 1 + max over the premises | `dilution = depth / n_sources` as in the spec |
| Hypotheses | go through the same `compute_evidence` (phase 5) | by construction they add zero sources |

Deferred: FTS5 (needed in phase 5 for dedup), the `predictions` table (phase 6); `sensitivity` is only stored for now, access checks arrive with the HTTP API.

## Phase 3 decisions

| Question | Decision | Why |
|---|---|---|
| What stale is | the `credence.stale` flag plus the node status `stale`; the cause in the `stale_marks(node_id, cause, reason, at)` table, one row per root cause | the spec requires serving the old value with a flag; the cause is needed so an agent can say "stands on data changed then, because of that" |
| The root cause | `cause` is always the retracted/superseded node or rule, never the nearest neighbour | in a long chain there are many neighbours and one question: what exactly changed |
| The cascade | over `dependents` (the reverse closure of supports/derives_from/assumes); for a rule, all its applications through `rule_id` and their dependents | spec item 3 on rules; one walk, nothing recomputed |
| Evidence on retraction | the source sets are recomputed at once, dead premises are excluded; credence stays lazy | otherwise nutrition would show a source that is gone; cheap (set operations), and the "moment of change" is kept by the flag, not the set |
| A retracted premise | the edge stays, the node is excluded from the "living" premises; `why` shows it with `✗` and the reason | the spec: "why shows the knocked-out leg"; the history must remain readable |
| supersede | = writing the new node the usual way (observe/assert/derive) + `link(replaces)`; a manual `link(replaces)` does the same: the old one becomes `superseded`, the cascade runs | one code path for both; the invariant "one active node per chain" cannot be bypassed with a manual edge |
| The replaces chain | the replaced node must be active or stale; `superseded` → `already_replaced` naming the active head; `retracted` → an error, the new node is written on its own | "correcting a retracted node" would mix two different events |
| A replacement cannot depend on what it replaces | `closure(new)` must not contain old | otherwise the new node would stand on the displaced one |
| One node replacing several | allowed | merging two claims into one is a normal correction |
| recompute follows replaces | the edge onto a superseded premise is removed and re-created onto the active head of the chain; recorded in `credence.inputs.followed` and in revisions | that is exactly what "new replaces old" means; done only on an explicit command, not in the cascade |
| recompute bottom-up | a premise with a stale flag → `stale_premise`; `stale()` sorts by depth, shallow first | a value from a stale premise would be the same garbage |
| recompute without living premises | error `no_support`, the flag stays | a node without support cannot be silently "repaired"; the decision (retract or a new premise) is the human's |
| recompute for non-min | requires `credence` from the caller, the method is kept | the store cannot judge; "recompute" a stated claim means re-reading the source |
| A changed value | dependents are flagged stale with the reason "recomputed a -> b" | a credence change is a premise change like any other |
| A new node on a stale premise | inherits the premises' marks (same cause/reason/at) | otherwise a stale value could be "laundered" with one derive |
| what_if | only for a claim with credence; a projection by `min` steps, through any other method "it will just go stale"; nothing is written | the store cannot recompute other methods, and it is more honest to say "will go stale" than to invent a number |
| Nested transactions | `Txn`: a public call from inside another (supersede → assert → link) joins the outer transaction | SQLite does not nest BEGIN; savepoints are unnecessary here |
| A retracted rule | `derive` with it → `invalid_support`; in `why` the rule name is marked `(retracted)` | a rule is a premise like any other |

## Phase 4 decisions

| Question | Decision | Why |
|---|---|---|
| Where the detector lives | on read: `conflicts()` scans living claims (active and stale), groups by (subject, predicate) and compares pairs; it writes only the review state into `conflict_triage` (addendum 3) | a contradiction is an observation over the graph, not a fact in it; writing an "auto edge" would mean guessing later who put it there. An explicit `contradicts` edge is a human/agent decision, and it is stored |
| What is compared | only `Value::compatible`: bool, enum, ref, number intervals in one unit. `text` and different units are "incomparable", not a conflict | addendum 1, item 2: comparability is a property of the type, no LLM in the deterministic path |
| Nested scopes | values incompatible and one scope strictly contains the other → a `refines` candidate, in `refinements()`, not in `conflicts()`; disappears after a `refines` or `contradicts` edge between them | addendum 1, item 1B: otherwise every refinement of a general rule would surface as a conflict |
| Equal scopes | an ordinary contradiction | nothing to refine |
| Disjoint scopes | nothing | both are right under different conditions, which is the norm |
| Ranking | the union of both sides' dependents, descending; then id | the spec: first what props up the largest part of the graph; a union so that shared descendants are not counted twice |
| What a record holds | both sides with value, scope, credence, number of sources and dependents; the scope intersection; explicit/detected flags; open questions about the pair | everything needed to choose an action from the spec: narrow the scope, retract, open a question |
| Cross-domain pairs | comparison over the whole store; a domain filter keeps pairs with at least one side in the domain | the same fact about the world may be recorded in two domains |
| Warning on write | after `assert`/`derive` the CLI prints `warning: … contradicts …` to stderr; the library does not refuse the write | the spec: contradictions surface, they do not block; the agent learns at once |
| Question | a `q_` node; payload: text, why_open, acceptance_criterion (mandatory), about (claim ids), progress (a list of dates/actors/notes); status `active` = open, `closed` = closed with a resolution | the spec: a first-class object with a closing criterion and progress |
| Closing | explicit only, `close_question(id, resolution)`; the store does not check the criterion | the criterion is text for a human; there may be an answer (`answers`), or the question is dropped as moot |
| A question as a premise | not allowed (`invalid_support`), as before | a question is not evidence |
| `Status::closed` | added to the enum, used only by questions | an explicit status is indexed and visible in `show`; a payload flag would have to be searched for |

## Phase 5 decisions, part 1: batches and hypotheses

| Question | Decision | Why |
|---|---|---|
| What a batch is | a `batches(id b_…, actor, purpose, started/last_write/closed_at, closed_reason, counters)` row; the handle's actor opens it with a purpose and it becomes current; `revisions.batch_id` marks everything written inside | addendum 1, item 4: the agent must state a purpose before writing; a batch is the unit of review |
| What is counted | claims (`assert`, `derive`, `confirm`) and hypotheses separately; sources are only marked and counted in `n_sources`, no quota on them | the danger is retelling oneself, i.e. claims; evidence is deduplicated by hash and does no harm |
| Batch quota | `Policy::batch_claims` 20, `batch_hypotheses` 5; a full batch closes itself (`quota: 20 claims`) on the N+1-th write attempt, the write is refused | the spec: "a batch closes itself after N writes" |
| Daily quota per actor | `ActorConfig::daily_claims/daily_hypotheses` in beliefd.json override `Policy` for one actor | digesting a channel's archive (700 posts in a day through one actor) hit 50 claims per domain and 90% of the extract ended up in profile.md as "waiting to be written" (2026-10-18); the quota guards against retelling oneself, not against a one-off load |
| Text dedup | `gate_text_duplicate`: a text claim (no subject/predicate) with cosine ≥ `Policy::text_duplicate_cosine` (0.92) to a living claim of the same domain is refused as `duplicate_claim` with the id and text; bypass with `despite`; without an embedding (provider down, quiet hours) the gate is open | the first 1089 claims of the digest agents (2026-10-18) were all text, the structural gate did not see them: 8 pairs with cosine > 0.9 and not one "repeated position → supports" link; spec item 2 demands a search before writing, the agents did not do it |
| Source by uri | `observe` with the same `uri` returns the existing source, like a content-hash match | re-digesting the same post (a different quote) would spawn a second node and split its support |
| Idle | `batch_idle_seconds` 1800: on the next write the batch is closed with reason `idle` | an unclosed batch of a long-lived minister must not grow forever |
| Stickiness | an auto-closed batch stays current and refuses all writes (`no_batch`) until the actor opens a new one; an explicit `end_batch` clears the current one, and with `require_batch = false` writes go without a batch | choosing to batch is the actor's decision, and the next batch needs a new purpose; silently letting a write "past the batch" would bypass the quota |
| Closing and rollback | `settle_batch` closes the batch in its own transaction before the write's transaction | otherwise the rollback of a refused write would also roll back the closing (as it did in the first version) |
| Daily limit | `daily_claims` 50, `daily_hypotheses` 10 per (domain, actor) per UTC day, counted over `revisions` (actor, at, reason) filtered by the node's domain; a refusal does not close the batch | addendum 1, item 4: without the second level an agent would just open the next batch |
| `require_batch` | off by default (a human through the CLI writes without a batch, the daily limit still applies); the HTTP API turns it on for agents | the gates must be in the store, not in the wrapper, but manual work must not require ceremony |
| `resume_batch` | an open batch of the same actor can be continued from another handle (CLI: `--batch` / `$BELIEF_BATCH`); someone else's or a closed one → `no_batch` | the CLI is one process per command |
| Whose batch may be closed | `end_batch`/`reject_batch` by anyone, the revision keeps the actor | closing and rejecting an agent's batch is the human's right |
| `reject_batch` | retracts everything created in the batch and still in force, last to first; the batch closes with `rejected: …` | the spec: "a human looks at one batch and accepts or rejects it whole" |
| Hypothesis | an `h_` node with a payload like a derived claim plus `rule`, `order` and (with a non-zero TTL) `expires_at`; `derives_from` edges onto claims and hypotheses; credence = min over the premises, method `min`, `inputs.ceiling = true` | the spec: a separate type, not a claim; the number is a ceiling, not an estimate |
| Order (2026-10-22, "depth 1" repealed) | `check_support(…, hypotheses_too)` only from `hypothesize`: a hypothesis may stand on a hypothesis, `order = 1 + max(order of premises)`; `derive`/`assert`/`link supports` still refuse hypotheses; on `confirm` the hypothesis premises go into the payload as `built_on`, `derives_from` edges only onto claims | the owner: a hypothesis is a thought, not a draft under review; a second-order thought (the intersection of two regularities) is what the detective is for; no self-reinforcement because the tower's nutrition = the union of the leaves, and stale from a retracted leg reaches the top along the edges |
| Detector | `live_claims` takes only `claim`; hypotheses do not enter conflicts | a hypothesis asserts nothing yet; its dispute with a claim is a reason to confirm or reject it, not a graph conflict |
| `confirm` | a new `c_` with the same payload (`confirmed_from`), `supports` from the evidence, `derives_from` as in the hypothesis, credence/method as in `assert`; then `link(c, h, replaces, "confirmed")`, the hypothesis becomes superseded; the rule's `n_applied` grows here, not at `hypothesize` | the spec: "becomes a claim only when confirmed by a source or a human"; replaces gives navigation and the NOTICE in `why(h)` |
| Rejection | `retract(h, reason)`, no separate API; shown as `rejected: …` | one way to remove a node |
| Expiry | `hypothesis_ttl_days` 30; lazily: `hypothesize`, `confirm` and `hypotheses()` first retract the expired ones (`expired`); 0 = never, `expires_at` is not written and is empty in output | no background process; "reviewed" = confirm or retract; 0 is the mode of the agent system since 2026-10-22: an open question is a normal state of a thought, not an overdue item |
| Queue | `open_hypotheses` 20 per domain (active and stale); beyond that `quota_exceeded`; 0 = no limit | the spec: "the queue must jam, not grow" — while hypotheses waited for a human; without a reviewer the queue would jam the detective |
| `stale()` and dead nodes | `mark_stale` skips retracted/superseded | nothing to recompute for them; earlier a retracted dependent showed up in `stale` |

## Phase 5 decisions, part 2: dedup, search, embeddings

| Question | Decision | Why |
|---|---|---|
| What a duplicate is | a living claim or an unreviewed hypothesis with the same subject and predicate (both non-empty), overlapping scope and `Value::compatible == true` | the spec, gate 2: "a close claim already exists"; addendum 1, item 10: decided by structure, not text |
| What the gate does | `duplicate_claim` with the id, value and text of the match and three ways out: `supports` onto it, `confirm` of the hypothesis, `despite` + `refines`/`contradicts`; `despite` is written to the payload | "a duplicate cannot be created silently", but it cannot be forbidden either: a refinement and a dispute are legitimate writes, and they must be explicit |
| Incompatible value | passes: that is a contradiction, the phase 4 detector finds it and the CLI warns | the gate must not duplicate the detector |
| Text (`text`) and other incomparables | not refused and not merged; `similar()` shows them as structural with `compatible = null` | addendum 1, item 10: "where there is no structure, do not merge automatically, show a human" |
| No subject/predicate | no structural match, the gate is silent; FTS and embeddings remain as a hint | nothing to compare |
| `supersede` | the node being corrected is excluded from the gate (`gate_exclude`); other matches require `despite` as usual | a correction is by definition "the same thing" |
| FTS | the virtual table `claims_fts(node_id unindexed, text, subject, predicate)`, FTS5, the `trigram` tokenizer; a query = tokens of three or more characters joined by OR, bm25 | trigrams give substrings without a stemmer, which inflected languages need; SQLite ≥ 3.34 on both machines |
| What is indexed | claims and hypotheses on write; no deletion, the status is filtered on read: `similar()` sees only the living, `search()` everything | nothing is deleted here either |
| Embeddings | the `embeddings(node_id, model_id, dim, vec float32, computed_at)` table; the provider is an OpenAI-compatible `/v1/embeddings` (llama-server with bge-m3, 1024), `EmbeddingConfig{base_url, model, timeout, backoff}` with no default URL | addendum 1, item 10; cpp-httplib as the client (and as the server in part 3), header-only, no TLS |
| Vector search | a scan over all vectors of the current model and dimension, cosine, top-N | the store is small; an index (sqlite-vec) when measurements say so |
| Fail closed | a provider error is written to `embed_error`, the following calls are skipped for `backoff_seconds` (60), the write goes through without a vector; `similar`/`search` return `mode = "fts"` and a `notice` with the reason | a write must not wait for a dead server; no silent degradation |
| Model change | `model_id` next to the vector; vectors of another model or dimension do not take part; `reindex_embeddings()` fills in the missing ones for the current model | do not mix models in one index |
| A confirmed hypothesis | the vector is copied under the claim's id, no second call | the same text |
| Output order | structural, then cosine descending, then bm25; `limit` | structural candidates always outrank textual closeness |
| GPU | the embedding server lives on the GPU machine; one call takes milliseconds, but quiet hours belong in `beliefd` (part 3), not in the library | the "no GPU noise at night" rule |

## Phase 5 decisions, part 3: the HTTP service

| Question | Decision | Why |
|---|---|---|
| Library | cpp-httplib v0.61.0, header-only, no TLS or compression (options off in `deps.cmake`) | loopback and WireGuard; TLS is done by nginx at the edge; one dependency for both the embedding client and the server |
| Actors | a list in the config: `name`, `domains`, `human`; the token only from `BELIEFD_TOKEN_<NAME>`; a token in the file → a warning and ignored | secrets only in the environment |
| One `Store` per process | a mutex per request; before handling: `set_actor`, `set_policy` (require_batch for agents only), `leave_batch` and `resume_batch` from the header for POSTs outside `/batch/*` | SQLite has one writer; a batch is a property of the request, not of the process |
| Domains | an agent writes (`observe/assert/derive/hypothesize/ask/rule`) only into its own; changes (`retract/supersede/recompute/confirm/progress/close`) only nodes of its domains; `link`/`dismiss`/`resolve` need at least one end of its own | the spec: ministries read each other but do not edit |
| Sensitivity | `private` is visible only to the node's domain actors and to the human; `domain` and `public` are visible to all | the spec: the column from day one, the check in the API; the `domain`/`public` distinction is kept for a future external exposure |
| How it is cut | `GET /node` 403; `why` — the subtree becomes a stub with `truncated`; lists (`stale`, `questions`, `hypotheses`, `search`, `similar`, `what-if`) are filtered per node; the sides of conflicts and refinements are redacted (text, value, scope removed, id and domain stay) | a conflict with someone's private claim must be visible as a fact, not by content |
| Premises from another domain | allowed if the node is visible; an invisible node in `support`/`from`/`about` → 403 | addendum 2: a cross-domain justification is an edge |
| Batches | `GET /batch`, `/batches` — one's own (the human sees all); `end` — one's own or the human; `reject` and `reindex-embeddings` — the human only | review is the human's right |
| Write response | `{id, conflicts: [...], similar: [...], search_mode}` | what the CLI prints to stderr, an agent needs as data |
| Codes | 400 input, 401 token, 403 domain/privacy, 404, 409 `no_batch`/`duplicate_claim`/`already_replaced`/`duplicate`, 429 quotas | the client distinguishes "fix the request" from "wait / open a batch" |
| Quiet hours | `embeddings.quiet_hours` in local time; on every request the provider is switched on/off through `set_embeddings`, not a single call inside the window; `reindex` inside the window → 409 | the "no GPU noise at night" rule: the embedding server lives on the GPU machine |
| Deployment | `deploy/beliefd.example.json`, `deploy/beliefd.service` (a user unit, `EnvironmentFile` with tokens, 0600); the database on an SSD | the quiet-disk rule |
| Not done | an MCP wrapper; rate limiting by IP; TLS | MCP is a separate layer over HTTP; the rest is nginx's job |

## Phase 6 decisions: calibration

| Question | Decision | Why |
|---|---|---|
| What a prediction is | a `predictions` row (`p_` id): node, p, the node's method, rule name and attributions snapshotted at forecast time, `made_at`, optional `resolve_by`, later `resolved_at`, `outcome`, `brier` | the spec: a forecast recorded BEFORE the outcome; the snapshot is what calibration groups by, and the node may change later |
| What may be predicted | a live claim or hypothesis; p defaults to its credence | a hypothesis's credence is a ceiling, so a forecast on it is the honest number to test |
| Resolution | `resolve(id, outcome)`: brier = (p − outcome)²; a resolved prediction cannot be resolved again | one forecast, one answer |
| The rule's track record | on resolution the rule node gets `n_resolved`, `n_correct`, `brier_sum`, `brier` (mean); `n_applied` keeps counting at `derive`/`confirm` | the spec's rule fields, now fed by outcomes rather than by applications |
| `calibration()` | over resolved predictions: overall Brier and buckets by credence step (nearest ladder step to p), by method, by rule, by attribution (a prediction counts in every attribution under its node) | "which method is worth what" is a question per group; attribution reliability is the track record the spec promised instead of hand-set weights |
| `orphans(months)` | active claims whose newest source is older than the horizon and that got no `supports` edge within it | the hygiene pass from the spec; a fresh edge onto an old source counts as reconfirmation |
| Where it shows | CLI `predict / resolve / predictions / calibration / orphans`, HTTP `POST /predict /resolve`, `GET /predictions /calibration /orphans`, MCP tools of the same names | every surface, as for the other phases |
| Not done | automatic resolution (nothing in the store knows what happened in the world); reliability feeding back into nutrition weights | the first needs an agent or a human; the second waits for enough resolved predictions to mean anything |

## Addendum 3: the review state of contradictions

The answer to the question after phase 4 is in [spec-addendum-3.md](spec-addendum-3.md). What was done:

| Item | Decision | Status |
|---|---|---|
| The `conflict_triage` table | (lo_id, hi_id) PK, first/last_seen_at, times_seen, state, fingerprint, decided_by/at, reason; an `expired_at` field added | done |
| When it is synchronized | every `conflicts()` (now a writing call, one transaction): new pairs become open, decisions with a mismatched fingerprint expire (state open, `expired_at`, the reason stays), open pairs the scan no longer finds become vanished, found again → open; an explicit edge → promoted | done; no background pass, the scan is the pass |
| The fingerprint | sha256 of (value, scope, status) of both sides | done; the status is included, so one side going stale also expires the decision |
| Decisions | `dismiss_conflict(a, b, reason)`, `resolve_conflict(a, b, reason)`; only for a pair the detector finds now (`not_a_conflict`), and not for promoted (the edge is already the decision) | done |
| What `conflicts()` shows | by default open and promoted; `include_decided` adds dismissed/resolved if the pair is still found; vanished and resolved on vanished pairs are visible only in `triage_stats()` | done |
| `why()` and auto pairs | `why()` shows only explicit edges, never auto-detected pairs | and never did |
| A metric for phase 6 | `triage_stats()`: counters by state and precision = (promoted + resolved) / (promoted + resolved + dismissed); in `belief stats` | done |
| Long-open pairs | `Triage.open_days`, in the CLI `conflicts --older-than DAYS` | done |
| An index on (domain, predicate, subject) | no | deferred until measurements; subject/predicate live in the JSON payload, an index would need expressions or columns |

## Addendum 1: where each item landed

The answers to the questions after phase 1 are in [spec-addendum-1.md](spec-addendum-1.md). Spread over the phases:

| Addendum item | Where it is implemented | Status |
|---|---|---|
| 6 credence: an error naming the two nearest steps | phase 1 | done |
| 7 one DAG over the evidential edges | phase 1 (`closure` already walked the union; a test for alternating types added) | done |
| 9 `contradicts` in canonical form | phase 1 | done |
| 3 `kind` as an enum, `attribution`, `trust_hint` (manual); `n_distinct_attributions` | phase 1 fields, phase 2 counter | done |
| 8 `replaces`: src new, dst old; one active node per chain; `why()` on a superseded node says so in the first line | phase 3 (retract/supersede) | done |
| 1 scope: `valid_from` defaults to the source's `observed_at`; the invariant scope(derived) ⊆ ∩ scope(premises), an empty intersection = error | phase 1.5 | done |
| 2 typed value: bool / number[lo,hi]+unit / enum / ref / text; the `predicates(name, value_type, unit, note)` registry, a type mismatch = error | phase 1.5 | done |
| 1 refines versus contradicts with nested scopes | phase 4 (detector) | done: `refinements()` |
| 5 nutrition weights 1.0 / 0.8 / 0.5 / 0.1 in the config; show the raw cardinality, the attribution count, the weighted sum, the concentration | phase 2 | done |
| 4 batches: `begin_batch(actor, purpose)` / `end_batch`, auto-closing, per-batch and daily per-(domain, actor) quotas | phase 5 (1/3) | done |
| 10 embeddings: base_url only from the config, fail closed to FTS-only with an explicit message, model_id and dimension next to the vector, candidates by embedding + the decision by structure | phase 5 (2/3) | done |

Phase 1.5 was split out because structured scope and typed values change the shape of `assert`/`derive` and are needed by every later phase (phase 2 inherits scope, phase 4 compares values).

## Addendum 2: the source kind `derived`

The answer to the question after phase 2 is in [spec-addendum-2.md](spec-addendum-2.md). What was done:

| Item | Phase | Status |
|---|---|---|
| `derived` removed from `SourceKind`; the string `derived` is no longer parsed | phase 2 | done; no migration needed, no database had such nodes |
| `directness: primary / secondary` on a source, default primary; the 0.5 multiplier in `NutritionWeights::secondary_factor`; llm_output ignores directness; `n_secondary` in nutrition | phase 2 | done |
| A source cannot wrap a node of this store: a node id in `uri`/`content` = error `bridge_source`; a cross-domain premise is only an edge | phase 2 | done; checked by the id shape (prefix + ULID) and the node's existence |
| Bootstrapping from notes/correspondence = `human_statement`, attribution = the person who knew it, directness per situation | a rule for agents | in the README |
| When the own sources of an imported conclusion become known, the import is superseded (`replaces`) rather than kept as a second `supports` | phase 3 (supersede) | done; acceptance test: after superseding the import and recomputing the consequence, nutrition = two measurements, two heads, not three |

## Nested scopes and the default valid_from (after phase 4)

Observation: two claims without an explicit scope get `valid_from` from their sources' dates (addendum 1, item 1), and if the dates differ, their scopes nest. The detector would treat such a pair as a refinement, and the contradiction would silently slide into `refinements()`.

| Question | Decision | Why |
|---|---|---|
| When nesting is a refinement | only when the inner scope is really narrower: it has a `valid_to` the outer lacks or an earlier one, or conditions the outer lacks. A difference only in the start date with both ends open is a contradiction | both claims are in force now; it is either a dispute or a change over time, and then a `supersede` or a `valid_to` on the old one is needed, not a silent "refinement" |
| Transparency | `assert` writes `scope_valid_from_inferred: true` into the payload when the date was taken from the source rather than given | the output shows that nobody asserted the interval |

## Neighbours and recent items (2026-10-21, for the detective agent)

- `neighbours(id, k, min_cosine, domain, since)` finds a node's kin by its ALREADY stored vector: no provider call, so it works in quiet hours too. The `min_cosine` threshold (0.6 by default) is below the duplicate gate (0.92): similar is wanted, not the same. The own domain by default, `"*"` for all; `since` cuts off the old. A node without a vector (the provider was down when it was written) is searched by its own words through FTS, with a note in `notice`.
- `recent(domain, since, limit)`: living claims and hypotheses created no earlier than `since`, oldest first: what a periodic reader has not seen yet. `created_at` added to `SimilarEntry`.
- Routes `GET /neighbours/ID`, `GET /recent`; sensitivity visibility as in search.
