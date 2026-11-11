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
