# Open issues of beliefdb (after the first production load, 2026-10-18)

Three "oracle" agents (digests of Telegram channels in the fnc-hq agent system) turned ~1200 Telegram posts into ~1700 text claims. What works and what does not, by the facts in the database.

## Does not work, must come back to

1. **Conflicts for text claims.** The contradiction detector compares typed values
   (subject + predicate + value). Text claims have none of that, so `contradicts` never appears:
   0 edges over 1700 claims, although the author certainly changed positions over 700 posts.
   Options: a hypothesis "the author changed position" at high embedding similarity and opposite
   polarity (needs an LLM judge on the pair), or require an explicit `contradicts` from the digest
   when a position changes (the instruction says so now, the agents do not use it).
2. **Hypotheses are not born.** The layer of generalizations ("systematically does not believe in
   X") stays empty: 0 hypotheses, the agents never called `hypothesize`, even after the rule began
   to be created by name. A separate step or role "generalize the domain" over the accumulated
   claims is needed, not a request at the end of a batch.
3. **No unit of review.** The `hq` actor is human and writes outside batches, so a digest batch
   cannot be rejected with one action. Options: agent actors per ministry with `require_batch`, or
   a batch the controller opens itself for every agent turn.
4. **Sources without claims.** ~3% of sources were recorded and no claim came out of them (the
   agent changed its mind). Harmless, but `orphans()` does not catch them: they are not stale,
   they are empty. A cleanup, or a ban on `observe` without a following `assert` in the same turn.
5. **Search without a cutoff.** `/search` always returns the 20 best, even when the best cosine is
   0.45 and the topic is foreign. The oracle sorts it out itself, but a threshold or a "nothing
   close" mark would be more honest.

## Already fixed by the same load

- The duplicate gate for text by embedding (`policy.text_duplicate_cosine`, 0.92).
- A daily quota per actor (`daily_claims`/`daily_hypotheses` in beliefd.json).
- `observe` of the same uri returns the same source.
- The rule for `hypothesize`/`derive` is created by name (on the controller side).
