# The detective benchmark

Does a memory of justified claims help an LLM name the culprit earlier than the story reveals it?

A mystery is fed scene by scene. After every scene the detective records the scene as a source,
asks the model to extract facts and writes them into beliefdb as claims supported by that scene
(a fact already in memory gets the scene linked as further support instead of a duplicate), then
asks the model who did it, giving it **only what beliefdb returns about each suspect**, never the
transcript. The answer is recorded as a prediction on each suspect's hypothesis (`belief predict`),
and at the end every prediction is settled against the known culprit, so `belief calibration`
shows how the confidence fared scene by scene.

The baseline is the same model with the whole transcript so far in its context and no memory.

**Score:** the first scene from which the true culprit is ranked first and stays first, relative
to the reveal scene. The reveal scene itself is the worst honest result; earlier is better.

```sh
cmake -S . -B build && cmake --build build
examples/detective/detective.py --story examples/detective/stories/lighthouse.json \
    --belief ./build/belief --llm openai --llm-url http://127.0.0.1:8080/v1 --model qwen3
examples/detective/detective.py --story examples/detective/stories/lighthouse.json --baseline \
    --llm openai --llm-url http://127.0.0.1:8080/v1 --model qwen3
```

`--llm claude-cli` uses Claude Code's `claude -p` instead of an HTTP endpoint. Python 3.8+, standard library only.

## Stories

`stories/*.json`: `scenes` in order, `suspects`, `culprit`, `reveal_scene`. The two stories here are original
and short. Famous mysteries are useless for this: a model that knows the ending from training is not
detecting anything. Write your own, or generate one with a model that is then not the one being tested,
and check that the judge model does not recognise the plot.

## What to expect

On a short story the transcript fits in the context and the baseline will often do as well or better:
the memory only pays off when the story is longer than what the model can hold and weigh at once, or when
the clue is a contradiction between two distant scenes, which is exactly what `belief conflicts` surfaces.
The point of the benchmark is to measure that, honestly, with a baseline, not to assume it.

## First results (2026-11-10, Claude via `claude -p`)

| story | scenes | reveal | transcript baseline: culprit first from | beliefdb memory: culprit first from |
|---|---|---|---|---|
| lighthouse | 12 | 12 | scene 2 | scene 2 |
| conservatory | 20 | 20 | scene 10 | scene 13 |

Honest reading: on these lengths the transcript fits in the context and the baseline wins or ties. The memory
line judged from `search` hits per suspect name only, so a clue that is a contradiction between two scenes
(an alibi of "ten minutes away" against a library sign-out time) reached it as two separate claims without the link.
What should change before the memory can win: retrieve by question (means, motive, opportunity, lies) rather than by
name, give the judge `why` and `conflicts`, and run stories long enough to overflow the context. The calibration
table that the run prints is the other half of the result: the detective's 0.35 forecasts never came true (n 9),
its 0.65 ones always did (n 4), which is what calibration is for.

## Results on a real screenplay (2026-11-11, Qwen3 on a local llama-server, ~8k-token context per request)

Text: the official *Knives Out* shooting script (not redistributed here; `load_text.py --screenplay`, 196 scenes in 25 chunks of 8,
reveal in chunk 21). `recognition_check.py` first: the model did not know the culprit by title and did not recognise the opening,
so the text is usable. (*The Mysterious Affair at Styles* was rejected: the model named Agatha Christie, the title and the murderer
from the first two chapters.)

| line | culprit (Ransom) ranked first and kept there from | notes |
|---|---|---|
| transcript baseline, last 6000 words in context | never stable; first at chunk 20 (one before the reveal), lost it again | fell for the story's trap: Marta at 0.94 in chunks 12–14 |
| beliefdb memory, judged from `search` by suspect name | chunk 24, three after the reveal | fixated on a minor character (Fran at 0.65) whom many stored facts mention; 499 claims written, none merged |

Neither line detects anything on this text; the memory line is worse, and its calibration says so (forecasts at 0.65 came
true 0 times out of 6). The reasons are visible in the run and are the next work: retrieval by suspect *name* rewards the
character most talked about, not the one with means and motive; the judge never sees `why` or `conflicts`; text-valued
facts never merge, so the memory is a pile, not a graph. The benchmark itself held up: the text is clean, the baseline is
honest about its context, the scores are reproducible.

## Detective v2 on real texts (2026-11-11, same Qwen3)

v2 asks the memory six generic investigative questions through semantic search, shows every fact with the scene it stands on,
hands the judge the contradictions the store finds, and tells it that being mentioned often is not evidence (see `HONESTY.md`:
designed before the run, nothing tuned to a text).

| text | reveal | transcript baseline (last 6000 words) | memory v1 (by name) | memory v2 (by question) |
|---|---|---|---|---|
| Knives Out (25 chunks) | 21 | first at 20, not kept | 24 | never: Marta at 0.75–0.85 before and after the reveal |
| The Murder on the Links (28 chapters) | 28 | 28 (at the reveal only) | — | never: fixated on Georges Conneau |

Neither line detects on either text, and v2 is not better than v1. The judge is the bottleneck: a quantized Qwen3 with an
8k-token window follows the story's own misdirection (a confession inside the story, a character the narrator points at) even
with the right facts in front of it, and the baseline, which sees the text itself, does no better. The calibration tables say
the same thing from the other side: v2's 0.8 forecasts on Knives Out came true 0 times out of 6.

What this establishes: the benchmark separates nothing until the judge can reason over the facts it is given. The next
experiment is the same controller with a stronger judge model, and the comparison that matters is then baseline against
memory under that judge, on texts the judge does not recognise.

## Detective v3: the memory thinks (designed 2026-11-11, before the run)

v2 gave the judge a pile of facts and asked «who did it»; the judge answered by resemblance. v3 makes the memory do what the
detective of a real agent system does with it: after every scene a **walk** takes the scene's new facts, each with its nearest
neighbours already in memory (semantic search), and writes up to five first-order *thoughts* (`hypothesize`, rule `walk`): a lie,
access to the means, a motive, an unexplained movement, an object out of place; each thought stands on the items it was drawn from.
The same pass reviews the live thoughts against the new facts: refuted ones are retracted, confirmed ones become claims. Then a
**second-order pass** reads the live thoughts with what each stands on and writes up to three thoughts of a higher order where two
thoughts converge on one person or one event (rule `order2`; a hypothesis may stand on hypotheses, its order is 1 + the deepest premise).
The **judge sees only the tower**: the live thoughts, highest order first, each with its premises, plus the store's contradictions.
No facts by themselves, no transcript. Per-suspect predictions and calibration are unchanged.

Generic by construction (`HONESTY.md`): the prompts name no story, the same prompts run on every text, nothing is tuned after a result.
The judge model is the same quantized Qwen3, now with a 128k-token window on one slot, so the transcript baseline is rerun with the
**whole** text in context (`--baseline-window-words 0`): a fairer and stronger opponent than the 6000-word tail of the earlier lines.

### v3 results (2026-11-11, same Qwen3, one 128k slot)

| text | reveal | transcript baseline, whole text in context | memory v2 (facts by question) | memory v3 (tower of thoughts) |
|---|---|---|---|---|
| Knives Out (25 chunks) | 21 | **19** (two before the reveal; Ransom at 0.85 from 19 on) | never | never: Marta 0.43–0.45 first from chunk 15, Ransom a steady second at 0.33–0.35 |
| The Murder on the Links (28 chapters) | 28 | 28 (at the reveal only; Jack, then Madame Daubreuil, then Bella before that) | never | 28 (at the reveal only; Georges Conneau at 0.71 from chapter 20) |

What the runs say:

- The baseline with the **whole** text in context is the best line so far: on *Knives Out* it names Ransom two chunks before the
  reveal and keeps him. The 6000-word tail of the earlier lines was the weak baseline, not the model. On *Links* nobody detects.
- v3 does think: 70 live thoughts on *Knives Out* (21 of order 2 to 5), 81 on *Links* (12 of order 2+); the walk refutes and confirms,
  and the order-2 pass does converge on people. The thought that names the real mechanism («Ransom admitted he switched the vials ...
  combined with his facilitation of the tox report's destruction») arrives at chunk 24, three after the reveal, as an order-4 thought.
- Two defects are visible in the tower, both generic:
  1. **Statements become facts.** The extractor records «Marta administered a lethal dose of morphine» as a claim at 0.8, and the judge
     cites it as her «direct admission» in every verdict from chunk 15 on. The memory holds what she *believes*, not what happened, and
     nothing in the store distinguishes the two: a claim stated by a character should carry the character as its attribution and a
     lower credence than what the narrator shows, and the judge should be told which is which.
  2. **Every higher-order thought is stale.** Each review that confirms or retracts a premise flags the thoughts above it; the tower the
     judge reads is almost entirely `stale`, and the judge is not told. Recompute (or re-derive) after the review, or show the flag.
- Calibration keeps saying the honest thing: v3's 0.5 forecasts on *Knives Out* came true 0 times out of 15; on *Links* its 0.65 ones 1 in 9.

The next step stands as written in `docs/next.md`, with the two defects above ahead of the stronger judge: they are cheaper and they are
what the runs show.

## Detective v4: imagination (designed 2026-11-12, before the run)

v3's thoughts were restatements of contradictions, and its prompt forbade imagining anything not in the list: a detective with the
imagination switched off. v4 switches it on. After every scene the **imagination** writes many *scenarios* (up to 15) of what may have
happened; each names its **assumptions** — links the text has not shown yet, the missing piece — and its **expectations** — facts later
scenes should show if it is true. In beliefdb an assumption is a hypothesis (rule `assumption`) standing on the facts that prompted it,
a scenario is a hypothesis standing on facts and its assumptions (so a bolder scenario is of a higher order), and an expectation is a
`predict` on the scenario. Every new scene is a **check**: its facts settle the open expectations (`settle yes|no`), a scenario whose
expectations were refuted and none met is retracted, and hypotheses flagged stale by a review are recomputed (`recompute` now takes a
hypothesis) or retracted when nothing live is left under them. The **judge** sees the live scenarios ranked by met minus refuted minus
half an assumption each, with their assumptions, grounds and settled expectations, and is told that an untested scenario is a guess.
A character's statement is now extracted as «X says: …» with credence 0.5, the narrator's showing at 0.8, and every prompt says which is which.
Method only, no story in the prompts (see `HONESTY.md`). Same judge model, same texts, same whole-text baseline as v3.
