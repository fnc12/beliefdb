# AGENTS.md

Instructions for coding agents (Claude Code, Codex, Cursor, Gemini CLI, …) working in this repository or using beliefdb as memory. Humans: the README is for you; this file is the condensed, operational version.

## What this is

beliefdb is an epistemic database: a store of claims that keep the chain of evidence down to primary sources, a credence with an explicit method, derivations, hypotheses and contradictions. One SQLite file. C++20 library `beliefdb`, CLI `belief`, HTTP service `beliefd`, MCP server `belief-mcp`. The main operation is `why(id)`.

## Build, test, run

```sh
cmake -S . -B build          # CMake ≥ 3.24, C++20 compiler, system SQLite ≥ 3.35 with FTS5 and JSON1
cmake --build build          # other deps come via FetchContent (nlohmann/json, sqlite_orm, doctest, cpp-httplib)
./build/beliefdb_tests       # doctest, in-memory databases, must stay green
./build/belief --help        # CLI
./build/belief-mcp --db memory.sqlite   # MCP server over stdio
./build/beliefd --config deploy/beliefd.example.json   # HTTP service
```

A local dependency cache under `~/deps` is optional (`cmake/deps.cmake`); never required.

## Layout

- `include/beliefdb/` — public headers: `types.hpp` (inputs, entries, enums, JSON), `store.hpp` (the API).
- `src/` — the store (`store.cpp`), typed values and units (`value.cpp`), embeddings client (`embed.cpp`), schema (`schema.hpp`, sqlite_orm).
- `cli/belief.cpp` — the CLI; `daemon/` — HTTP server, read-only web page (`ui.hpp`), shared JSON parsing (`json_io.hpp`); `mcp/belief_mcp.cpp` — the MCP server.
- `tests/` — one file per phase; `docs/` — the specification (`spec.md`), its three addenda, `design-notes.md` (every decision with its reason), `open-issues.md`.
- `deploy/` — example config and systemd unit for `beliefd`.

## Rules of the code

1. All SQLite access goes through sqlite_orm (`src/schema.hpp`), never raw SQL strings in new code unless sqlite_orm cannot express it (FTS5 and recursive queries are the known exceptions, kept in one place).
2. Nothing is ever deleted from the graph. Changes are `retract`, `supersede`, `replaces`; dependents are flagged stale, not recomputed.
3. Every write is one transaction; invariants (support required, acyclic justification, credence on the ladder, scope not widened) are checked on write and throw `beliefdb::Error` with a `code`.
4. Credence values are the fixed steps 0.05 0.2 0.35 0.5 0.65 0.8 0.95; a method is mandatory.
5. The library must work with no embedding server (full-text only) and say so in results (`mode`, `notice`).
6. Secrets only from the environment (`BELIEFD_TOKEN_<NAME>`, `BELIEFD_UI_PASSWORD`); config files hold none.
7. Tests for every behaviour change; the suite runs in a few seconds. Add to the test file of the matching phase.
8. Document decisions in `docs/design-notes.md` as a row: question, decision, why.

Commit messages in English, one logical change per commit.

## Using beliefdb as your memory (MCP)

Add the server to your client, pointing at a database file:

```sh
claude mcp add beliefdb -- /path/to/belief-mcp --db ~/beliefdb.sqlite        # Claude Code
```

For Claude Desktop, Cursor or any other MCP client add `{"command": "/path/to/belief-mcp", "args": ["--db", "/path/to/beliefdb.sqlite"]}` to its MCP configuration. Semantic search needs an OpenAI-style embedding server: set `BELIEF_EMBEDDINGS=http://host:port` (and `BELIEF_EMBEDDING_MODEL`, default bge-m3) in the server's environment; without it search is full-text.

The server's `initialize` reply carries the discipline; in short:

1. `search` (and `similar` for structured claims) before writing. An existing claim gets your evidence via `link` (supports), not a duplicate.
2. Evidence first: `observe` the source (what the user said, a document, a measurement), then `assert` the claim with that id in `support`. Opinions of the model are not claims; they are `hypothesize`, built on claim ids.
3. A changed fact: `retract` with a reason, or `assert` the new one and `link … replaces`.
4. `why` to explain any claim; `conflicts` to find contradictions and resolve them with the user.

Tools: `observe`, `assert`, `derive`, `hypothesize`, `search`, `similar`, `why`, `conflicts`, `retract`, `link`, `recent`, `stats`. The same operations exist in the CLI (`belief --help`) and over HTTP (`beliefd`, see README).
