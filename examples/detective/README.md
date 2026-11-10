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
