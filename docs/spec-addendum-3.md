# Addendum 3 to the specification: the contradiction detector and the review state

Origin: the design session "A belief system with calibration", 2026-10-17, the answer to the question after phase 4 (a detector on read instead of an auto edge on write). The text is kept verbatim (translated from Russian). What was implemented is in [design-notes.md](design-notes.md).

---

=== ANSWER: A CONTRADICTION DETECTOR ON READ ===

The decision is accepted, no objections. Both motives are right: an auto edge
is indistinguishable from a decision, and it goes stale after a supersede. A
contradicts edge is only an explicit decision of a human or an agent.

BUT auto-detected pairs DO need to be stored, and the main reason is not
history.

THE REASON: WITHOUT PERSISTENCE A PAIR CANNOT BE DISMISSED.
A detector over overlapping scopes will find false pairs — it does not see the
conditions that distinguish the cases but are not expressed in conditions. A
human looks and says "this is not a conflict". If the state is not stored,
conflicts() will show the same pair next time, and so on forever. In a month
the output is 90% pairs that were already reviewed, and people stop using it.

So storage is needed not for history but for the REVIEW STATE. History is a
bonus.

This also removes your worry about staleness, but in the right way: it is the
DECISION about a pair that must go stale, not the storage that must be absent.

--- THE TABLE ---

conflict_triage(
  lo_id, hi_id,              -- canonical: min(id), max(id). PK.
  first_seen_at,
  last_seen_at,
  times_seen,
  state,                     -- open | dismissed | resolved | vanished | promoted
  fingerprint,               -- see below, the key field
  decided_by, decided_at,
  reason                     -- why dismissed, or how resolved
)

state:
  open      — found, not reviewed
  dismissed — a human said "not a conflict"
  resolved  — settled through refines / narrowing the scope / retract
  vanished  — stopped being found by itself (one side superseded)
  promoted  — became an explicit contradicts edge

--- FINGERPRINT: WHY IT IS MANDATORY ---

The dismissal was based on the content of the pair AT THAT MOMENT. If one of
the sides changed, the dismissal no longer holds.

fingerprint = a hash of (value, scope, status) of BOTH sides at decision time.

On a scan: if the pair's current fingerprint does not match the stored one, the
decision EXPIRES, the state returns to open, and the pair surfaces again. The
reason and the history are kept — the human will see "you dismissed this in
March for this reason, but the value has changed since".

That is exactly the same logic as stale for credence: a decision is not erased
silently, it is marked invalid.

--- WHAT NEEDS NO HANDLING ---

Resolution through supersede: superseded nodes do not enter the scan (it walks
active ones), the pair simply stops being found. No special logic is needed; it
is enough that a background pass sets state = vanished when the pair has gone
from the output. That is "the conflict resolved itself".

--- THE BOUNDARY BETWEEN why() AND conflicts() ---

Important not to mix them:
  - why() shows EXPLICIT contradicts edges, if any. That is part of the
    justification: the claim has a declared dispute.
  - why() does NOT show auto-detected unreviewed pairs. Otherwise why() becomes
    non-deterministic: it starts to depend on whatever else is in the database at
    read time, and the same node unfolds differently on different days.

why() is about provenance. conflicts() is about review. Do not glue them.

--- PERFORMANCE: DO NOT OPTIMIZE EARLY ---

The scan is not quadratic in practice: only claims with matching (subject,
predicate) are compared. An index on (domain, predicate, subject) keeps the
buckets small. At tens of thousands of nodes this is cheap.

If it ever becomes slow — materialize the scan's result into the same
conflict_triage with a background pass, and do NOT move to edges on write. The
architecture stays, only the moment of computation changes.

--- A METRIC FOR PHASE 6 ---

A side benefit of storage: the share of detected pairs that turned out to be
real conflicts (promoted + resolved) versus dismissed. That is the precision of
the detector itself. Without the log it cannot be measured, and it must be — if
more than half is dismissed, the scope model is too poor and more keys in
conditions are needed, not a smarter detector.

And a separate signal: a pair in state = open for longer than three months.
Usually that means not "one of the sides is wrong" but "our model of scope does
not distinguish two cases that must be distinguished". Such pairs are worth
showing as a separate list.
