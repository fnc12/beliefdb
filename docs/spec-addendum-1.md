# Addendum 1 to the specification: answers to the questions after phase 1

Origin: the design session "A belief system with calibration", 2026-10-16. The text is kept verbatim (translated from Russian). What of it is implemented, and in which phase, is in [design-notes.md](design-notes.md).

---

1. SCOPE — the structure is accepted, with three refinements.

We take what was proposed: valid_from / valid_to (event time), conditions as
key-value pairs, note for the rest.

Refinement A. Do not confuse the two time axes. valid_from/valid_to is when the
fact was true in the world. When WE learned about it is created_at /
superseded_at on the node itself, which already exist. Graphiti separates exactly
this way, and the separation is mandatory: without it you cannot answer "what did
I consider true in March".
valid_to = NULL means "still in force", not "unknown".
If valid_from is not given, it defaults to the source's observed_at.

Refinement B. Intersection = the intervals overlap AND the conditions agree on the
shared keys. A missing key = wildcard. That is right, but it gives an asymmetry
that must be handled: {patient: me} and {} intersect, although the second is the
more general claim. So the detector must distinguish two cases:

  - the scope of one STRICTLY CONTAINS the scope of the other -> a candidate for
    refines, NOT for contradicts. Suggest refines.
  - the scopes intersect but neither contains the other -> contradicts.

Otherwise every refinement of a general rule will surface as a conflict, and
conflicts() will be clogged with noise within the first month.

Refinement C (the sub-question about inheritance). This matters more than it
seems. The scope of a derived claim is COMPUTED as the intersection of the
premises' scopes. The caller may NARROW it but not widen it. Widening = a write
error.

Invariant: scope(derived) ⊆ ∩ scope(premises).

Rationale: a conclusion cannot hold in more situations than its premises. The
check is cheap and catches a whole class of agent errors, where it happily
generalizes a special case to everything. An empty intersection -> error: you are
deriving from premises that are never true at the same time.

2. TYPED VALUES — a minimal set of five types.

  bool
  number   — ALWAYS an interval [lo, hi]; a point is lo == hi. Plus a unit.
  enum     — a member of a named set of values (the set is declared separately)
  ref      — a node id (for claims of the form "X is caused by Y")
  text     — the fallback, EXPLICITLY INCOMPARABLE

The key decision: comparability is a property of the type, not best effort.
If value_type = text, the automatic contradiction detector does NOT TRY to
compare at all. Such a contradiction can only be set by hand.
This keeps the spirit of the specification: no LLM in the deterministic path.

Why number is always an interval: "about 40 thousand" versus "43 thousand" is
not a contradiction but agreement. Intervals settle this without subtleties of
tolerances. Incompatibility = the intervals do not overlap.

Normalize units to a base unit on write, keep the original. Otherwise "$40k" and
"40000 USD" look like a conflict.

AN ADDITION that was not in the specification but follows from this question:
a light PREDICATE REGISTRY is needed. predicates(name, value_type, unit, note),
created on first use. Otherwise the same predicate gets number in one claim and
text in another, and there is nothing to compare. A type mismatch on reuse of a
predicate = a write error.

3. SOURCE RELIABILITY — none of the three options, but their combination.

Subtypes with weights (stated/expert) are NOT taken: they require deciding in
advance who is an expert, which is exactly what we wanted to measure, not
postulate.

We do this:
  - kind stays coarse and STRUCTURAL: measurement | document | human_statement |
    llm_output | derived. It is about the nature of the evidence.
  - a separate ATTRIBUTION field is added: who exactly said it ("Dr. Ivanov",
    "reddit user X", "the official Panic blog"). It is not a weight, it is an
    identity.
  - reliability is NOT assigned by hand. It is a track record per attribution,
    computed in phase 6 through predictions.
  - until there is data, the default is by kind.
  - a manual trust_hint is allowed for bootstrapping, but EXPLICITLY marked as
    manual, to tell it later from measured reliability.

And here is the most important part of this answer: it refines the mechanics of
nutrition. Three posts quoting one doctor: three sources, but ONE attribution.
The independence of evidence is determined by attribution, not by source.

So nutrition() must return BOTH numbers:
  n_distinct_sources       — how many documents/measurements
  n_distinct_attributions  — how many independent heads

The second is the more honest measure of nourishment. If they are five and one,
you have one source retold five times, and that must be visible.

4. WHAT A "RUN" IS — not a time span and not an LLM call, but an explicit batch.

  begin_batch(actor, purpose) -> batch_id
  ... writes ...
  end_batch(batch_id)

Quotas are tied to the batch_id. The agent declares the boundaries itself and
declares the PURPOSE itself ("processing the Playdate subreddit backlog").

Why this beats any definition of a session:
  - the agent must state what it is doing before writing;
  - a natural unit of review appears: a human looks at one batch and accepts or
    rejects it whole;
  - revisions.actor finally means something.

Auto-closing: a batch closes itself after N minutes of idleness or N writes. An
unclosed batch of a long-lived minister must not grow forever.

TWO LEVELS OF QUOTAS, mandatory:
  - a quota of writes and hypotheses per batch;
  - a separate daily limit per (domain, actor) pair.
Without the second, an agent that hit the batch quota will simply open the next
one.

5. NUTRITION WEIGHTS — the proposed ones are fine, but the spread at the bottom
is sharper.

  measured      1.0
  document      0.8   (primary, dated, citable)
  stated        0.5
  llm_judgment  0.1

llm_judgment is close to zero not out of modesty: it is not evidence, it is a
retelling. 0.2 gives it too much of a voice in the union.

Show, in this order: the raw cardinality of the set, the number of attributions,
the weighted number. Plus ONE ADDITIONAL METRIC — concentration: the share of
the weight contributed by the largest single source. If one source provides 80%
of the nourishment, the claim is fragile, and that must be visible next to the
number.

The numbers go into the config. And let nobody treat them as found truth — they
are meaningful only after calibration in phase 6.

6. CREDENCE OFF THE LADDER -> ERROR. No objections, the decision is right.
Silent rounding would hide exactly what we want to see.
One addition: name the two nearest steps in the error text, so that the caller
chooses consciously rather than at random.

7. ACYCLICITY — agreed, but the wording must be widened, their version is still
leaky.

Correctly: ALL evidential edge types form ONE common DAG. supports,
derives_from, assumes — check acyclicity over the UNION, not per type. Otherwise
a cycle alternating types passes: A supports B, B derives_from A. Each type on
its own is acyclic, yet why() still runs forever, because it walks the union.

Non-evidential edges — contradicts, refines, answers, replaces — are excluded
from the check.

8. REPLACES: src = new, dst = old. CONFIRMED.
It reads as "new replaces old", matching the verb.
Plus: in a replaces chain only one node may be active — check it.
And why() on a superseded node must say so in the first line, not somewhere in
the details.

9. CONTRADICTS: one row — agreed, but CANONICALIZATION is needed, otherwise it is
a bug. Without it both (A,B) and (B,A) can be inserted as two different rows, and
the PK will not catch it. Always store as (min(id), max(id)). Then the primary
key itself forbids the duplicate.
Document that for contradicts src/dst carry NO direction semantics, unlike all
other edges.

10. EMBEDDINGS — they fit; "no external network" meant "no third-party cloud",
not "no loopback and LAN". bge-m3 on one's own machine is what was meant.

Requirements:
  - base_url only from the config, with no default pointing anywhere outside;
  - FAIL CLOSED: the provider is unreachable -> dedup degrades to FTS-only and
    SAYS SO in the response, not silently;
  - model_id and the dimension are written next to every vector. A model change
    invalidates the vectors; do not mix models in one index;
  - FTS-only is a fully supported mode with tests, not an emergency one.

AND A TRAP to avoid at once. Cosine similarity over claim text gives false merges
where claims differ by negation or by scope: "X works" and "X does not work" embed
almost identically. So in the dedup gate embeddings are ONLY a candidate
generator. The final decision "the same or different" is made by the structural
fields: subject, predicate, value_type, scope intersection. Where there is no
structure (value_type = text), do not merge automatically at all — show a human.

=== IN TOTAL, WHAT CHANGES IN THE SPECIFICATION ===
New things that were not in the brief and must be added:
  - a predicate registry with a declared value_type (from question 2);
  - attribution on a source, separate from kind, and n_distinct_attributions
    in nutrition (from question 3);
  - a batch instead of a session, with two levels of quotas (from question 4);
  - the concentration metric in nutrition (from question 5);
  - the invariant scope(derived) ⊆ ∩ scope(premises) (from question 1);
  - refines-versus-contradicts with nested scopes (from question 1);
  - one DAG over all evidential edges (from question 7).
