# Addendum 2 to the specification: the source kind `derived`

Origin: the design session "A belief system with calibration", 2026-10-16, the answer to the question after phase 2 (why `derived` has no weight and when it is created at all). The text is kept verbatim (translated from Russian). What was implemented is in [design-notes.md](design-notes.md).

---

=== ANSWER: THE SOURCE KIND derived — REMOVE IT FROM THE ENUMERATION ===

First, an admission: derived got into the list of kinds in my previous answer
by carelessness. It is a category error. A source is primary evidence from the
world. A conclusion is not evidence, it is a claim with derives_from edges.
Keeping derived among the kinds means allowing reasoning to be packed into the
wrapper of evidence, and after that any weights are a fight with the symptom.

Your reasoning about 0.1 is right in its motive: you correctly felt that a high
weight would let an agent feed a claim with its own conclusion. But 0.1 leaves
the hole open, just narrow. We close it structurally.

1. A WEIGHT FOR derived — NO, because there is no derived kind.
Remove it from the enumeration. The valid set of kinds:

  measurement | document | human_statement | llm_output

kind answers "who or what produced this", not "how close to the world it is".

2. BOTH OF YOUR CASES ARE SOLVED WITHOUT A NEW KIND.

Case A — importing a conclusion without its chain of justification.
Someone claims "our analysis shows X", and we cannot see what showed it. For US
this is not a conclusion, for us it is evidence: we were told. It is modelled as
a document or a human_statement with attribution = that author. The nourishment
of such a node is one source, one attribution. And that is right: we really do
just take someone's word.

But the difference between "a table of measurements" and "someone's conclusion
from measurements we cannot see" is real and must be stored. It is NOT a source
kind, it is an orthogonal property. Add a field on source:

  directness: primary | secondary

  primary   — a primary observation, data, a direct statement about a fact
  secondary — an account of someone else's conclusion, a retelling, a summary of
              someone else's analysis

secondary is a 0.5 multiplier on the kind's weight. Then:

  measurement      primary   1.0
  document         primary   0.8
  document         secondary 0.4
  human_statement  primary   0.5
  human_statement  secondary 0.25
  llm_output       —         0.1   (always; directness does not apply)

One field instead of a kind, works the same for people and for documents, and
no way remains to pack reasoning into a source.

Case B — a bridge between domains.
No source is needed here at all, and creating one is FORBIDDEN. The derives_from
edge simply CROSSES the domain boundary and points at the node in domain B
directly. That was the decision in the first brief: no correspondence between
ministries, the graph is read directly, provenance is intact.

A derived source at a domain boundary would break exactly what everything is
built for: why() would hit a fake source instead of continuing the walk into
domain B. That is precisely the loss of provenance at a hop that we forbade.

Case C, which you did not name, but it will come — bootstrapping.
Moving accumulated knowledge into the system (from memory.md, from
correspondence, from the author's head). The temptation to mark it derived is
strong. Correctly: human_statement, attribution = the author, directness per
situation. That is a real attribution, not a stopgap.

3. INHERITANCE OF NOURISHMENT — the question is withdrawn, but the answer
matters, because it generalizes.

Nutrition is NOT a scalar copied somewhere. It is a union over the transitive
closure. Hence both cases solve themselves:

  - A cross-domain edge: the closure continues into domain B, the union is
    computed correctly and automatically. There is nothing to inherit — it just
    works. That, by the way, is the strongest argument for a direct edge over a
    bridge node.

  - An import without a chain: the chain really does break. Nourishment = one
    source, one attribution. And it MUST be lost, because we really cannot check
    the conclusion. That is not a bug; the system is honestly reporting that we
    trust someone's word.

So the answer to "neither lose nor gain nourishment" is: do not lose it where
the chain is intact (a cross-domain edge), and lose it where there is none (an
import). The difference between these cases is exactly what we want to see.

THE DOUBLE-COUNTING TRAP, must be handled.
An imported conclusion and its own sources are NOT independent. If a report
claims X on the basis of s1 and s2, and later we obtain s1 and s2 separately,
they cannot be counted as three independent pieces of evidence. Dedup by
attribution catches this only partially.

The rule: when the own sources of an imported conclusion become known, the
import is SUPERSEDED (replaces) rather than kept as a second independent
supports. Otherwise nourishment grows out of nothing, merely because we learned
what someone else's conclusion stood on.

=== WHAT CHANGES IN THE CODE ===
  - remove derived from the kind enumeration, migrate existing nodes (if any:
    into document/human_statement by meaning);
  - add sources.directness: primary | secondary, default primary;
  - a 0.5 multiplier for secondary, in the config next to the kind weights;
  - llm_output ignores directness;
  - forbid creating a source at a domain boundary: a cross-domain reference is
    only a derives_from edge onto the other domain's node;
  - supersede instead of a second supports when an import's sources appear.
