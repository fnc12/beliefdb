# Where the work stopped (2026-11-11)

A note to come back to. Everything below is in the repository; this page is the thread.

## Done

- Phases 1–6 of the specification, including calibration (`predict` / `settle` / `calibration` / `orphans`).
- Surfaces: CLI `belief`, HTTP `beliefd` with the read-only `/ui`, MCP server `belief-mcp` (16 tools), AGENTS.md.
- Public repository, CI on Linux and macOS, release v0.1.0 with binaries, English docs, dual license.
- `examples/detective/`: the benchmark (scene-by-scene whodunits, memory-only judging, predictions settled against the
  culprit, a transcript baseline), `load_text.py` for Gutenberg novels and screenplays, `recognition_check.py` against
  contaminated texts, `HONESTY.md` with the rule against tuning to a text.

## The open question

Does a memory of justified claims help a model find the culprit earlier than reading the transcript does?

Results so far, judge = a quantized Qwen3 with an 8k-token window (README of the example has the tables):

- on three short LLM-written stories: ties or the baseline wins (the transcript fits in the context);
- on *Knives Out* (25 chunks, reveal at 21): baseline first at 20 and lost it; memory v1 (by name) at 24; memory v2 (by
  investigative question) never;
- on *The Murder on the Links* (28 chapters): baseline at the reveal only; memory v2 never.

Reading: neither line detects; the judge follows the story's own misdirection even with the right facts in front of it.
The benchmark cannot separate "memory helps" from "judge cannot reason" until the judge is stronger.

## v3 (2026-11-11): the memory thinks

The oracle detective's mechanism moved into the benchmark: a walk over each scene's facts with their neighbours writes first-order
hypotheses and reviews the live ones, a second-order pass writes hypotheses on hypotheses, the judge reads only the tower. Results
(README of the example): still no detection; the baseline with the whole text in a 128k window now finds the *Knives Out* culprit
two chunks before the reveal, the memory does not. What the tower shows: characters' statements are stored as facts at full credence
(«Marta administered a lethal dose») and quoted by the judge as admissions; every higher-order thought is `stale` after the reviews
and the judge is not told. Both generic, both before the stronger judge.

## Next step, decided

Run the same controller (v2, unchanged) with a stronger judge, on texts that judge does not recognise, baseline against
memory. Two ways to get the judge:

0. first the two defects v3 exposed: attribution and credence of a character's statement versus the narrator's showing (the judge
   must see which is which), and recomputation of stale higher-order thoughts after a review;
1. a bigger local model (Qwen3-32B at 4-bit fits a 32 GB GPU); the real texts probably stay usable — check with
   `recognition_check.py` first;
2. a frontier model, which almost certainly knows *Knives Out* and every Christie: then only obscure or anonymised texts,
   each passed through the recognition check, and never through anyone's personal account.

Then, if the memory still does not win: the facts never merge (text values are incomparable by design), so the store is a
pile rather than a graph; the extractor must be made to produce typed values (times, places, possessions) so `conflicts`
has something to find. That is a change to extraction, generic, and it goes through `HONESTY.md`.

## Smaller items

- `recognition_check.py` treats "author recognised, title wrong" as usable; decide whether that is too lenient.
- The duplicate gate rejects the per-suspect hypotheses as near-duplicates of each other; the controller passes
  `--despite`. A better rule: text duplicates only when subject and predicate also match.
- Reliability from calibration does not yet feed back into nutrition weights (design-notes, phase 6, "not done").
