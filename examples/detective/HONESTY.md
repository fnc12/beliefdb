# The rule of the benchmark

The detective controller is changed only in ways that are generic to any whodunit: how facts are
extracted, which investigative questions the memory is asked, what the judge is shown. Nothing in
the controller may refer to a particular story, character, clue or outcome, and no setting is
adjusted after looking at a story's result to make that story's number better.

Concretely:

1. Every change to `detective.py` is designed and written down before it is run on a benchmark text.
2. A change is kept only if it is argued from how detection works in general (means, motive,
   opportunity, contradictions, provenance), not from what a specific run got wrong.
3. Results are reported for every text the model does not recognise, including the ones where
   the memory line loses. A text on which the controller was debugged is reported as such.
4. New, unseen texts are the real test: a controller that works only on the text it was tuned on
   is a fitted model, not a detective.

Results so far are in `README.md`; the texts under copyright are not in the repository.

Addendum (2026-11-12, v4). The v4 prompts were written after reading a screenplay's own account of its detective's method
("I have eliminated no suspects"; "a case with a hole in the middle"; imagine what fills it, then let the facts fall where they
fall). Only the method went into the prompts; no character, clue or outcome did, and the same prompts run on every text.
