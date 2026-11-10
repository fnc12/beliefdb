# Contributing

Issues and pull requests are welcome. A few things that keep the project coherent:

- Read `docs/spec.md` first: it says what beliefdb is and, more importantly, what it is not (not a Bayesian network, not a retrieval layer, nothing is ever deleted). Changes that go against it need a discussion before code.
- Every behaviour change comes with a test in the test file of the matching phase (`tests/`), and the suite stays green on Linux and macOS (CI runs both).
- Decisions go into `docs/design-notes.md` as a row: question, decision, why. The "why" is the part future readers need.
- Code style follows the surrounding code: C++20, sqlite_orm for every query, one transaction per API call, errors as `beliefdb::Error` with a code.
- Commit messages in English, one logical change per commit.

By contributing you agree that your contribution is licensed under the AGPL-3.0 and may also be distributed under the project's commercial license (see README, "License").
