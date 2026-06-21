# Code review guide

The standard for reviewing changes to Villen. It is written for **two
audiences**: human reviewers, and the CI reviewer (the `claude-review` workflow).
Both apply the same rules below.

The process half adapts Google's
[*How to do a code review*](https://google.github.io/eng-practices/review/) — the
well-known engineering-practices guide. The Villen-specific half (architectural
invariants and style) is what makes a change fit *this* codebase. When the two
disagree, the Villen rules win here.

## The standard

> Approve a change once it definitely improves the overall health of the
> codebase, even if it is not perfect.

There is no perfect change. Don't block a healthy improvement over personal
preference, and don't let "could be slightly better" hold up a fix. Conversely,
*do* block a change that makes the codebase worse — degraded health compounds.

Reviewers move fast: a prompt review keeps authors unblocked. It is fine to
approve with small comments the author is trusted to fold in.

## What to look for

A condensed, Villen-tuned version of Google's checklist. Roughly in priority
order — a problem high in the list outranks a nit lower down.

- **Design.** Does the change belong here at all? Does it respect the two-layer
  model (Villen runs engines; engines run games) and the seams below? A feature
  bolted onto the wrong layer is the most expensive thing to miss.
- **Functionality.** Does it do what it claims, including edge cases —
  disconnects, reconnects, malformed input, an empty roster, a full table?
- **Complexity.** Is anything more complex than it needs to be? Speculative
  generality ("we might need this later") is complexity; flag it.
- **Tests.** Changed behavior needs a test that fails without the change. Network
  / membership / authority behavior belongs in the host integration suite
  (`tests/integration_tests.cpp`); pure rules belong in `tests/engine_tests.cpp`.
- **Naming & comments.** Names say what a thing is; comments say *why*, not *what*
  the code already says. Match the surrounding comment density.
- **Style & consistency.** The rules below, and consistency with nearby code.
- **Security.** Anything reading off the player WebSocket is untrusted input —
  see the invariants below.

## Villen invariants (reviewers must enforce)

These are load-bearing. A change that breaks one is blocking, however small.

- **Engine purity.** `engine/` is pure rules — no graphics, socket, device, or
  I/O code, and no game-host concepts. All network concerns live on the player
  WebSocket edge (DESIGN §9).
- **Villen never constructs a gameplay message.** The host owns the envelope (who,
  which seat, the join/leave/reconnect lifecycle); the engine owns the payload
  (moves, state) and emits its own bytes. Host code that parses or builds a
  game-specific message is in the wrong place (DESIGN-game-framework §4).
- **One thread, one loop.** Host code is driven from the single poll loop with no
  locks and no worker threads (DESIGN §5). A new thread, mutex, or background task
  in a host path needs an explicit, called-out justification.
- **Untrusted wire input never throws.** JSON off the WebSocket is read
  exception-free: `nlohmann::json` is parsed with `allow_exceptions=false` and
  every field is type-checked (`is_string()` etc.) before use — never `value()` /
  `get<>()` on a key whose type a hostile client controls, which would throw and
  kill the single loop (a DoS). See `host/src/envelope.cpp` and `strField` in
  `host/src/engines/chess/chess_engine.cpp` for the pattern.
- **No local infrastructure details in commits** — personal IPs, hostnames,
  account names, keys. Generic Steam Deck / SteamOS facts are fine.

## Style rules

- **Always brace the body of `if` / `else` / `for` / `while` / `do`, even when it
  is a single statement, and put that body on its own line** — no unbraced guard
  clauses or early returns, and no packing the statement onto the condition line
  (`if (x) { stmt; }`). The one exception is the standard `else if` chain: write
  `else if (cond) { ... }`, not `else { if (cond) { ... } }` — no extra outer
  brace layer. This prevents the `goto fail;`-class bug (a second statement
  silently falling outside the conditional), keeps later diffs minimal and safe
  when a branch grows, and makes every control-flow body visually uniform.

  ```cpp
  // Bad — unbraced single statements
  if (fd_ < 0) return false;
  for (auto& c : conns_) c.close();
  if (ok)
      send(msg);
  else
      reject();

  // Bad — braced but packed onto the condition line
  if (fd_ < 0) { return false; }

  // Good
  if (fd_ < 0) {
      return false;
  }
  for (auto& c : conns_) {
      c.close();
  }
  if (ok) {
      send(msg);
  } else {
      reject();
  }

  // Good — else if chains stay flat, no extra outer brace
  if (a) {
      foo();
  } else if (b) {
      bar();
  } else {
      baz();
  }
  ```

  This is the rule for all new and modified code. Existing unbraced statements
  predate it; update them when you are already touching that code.

  The checked-in [`.clang-format`](../.clang-format) enforces this mechanically
  (`InsertBraces` + `AllowShort*: Never`), alongside the rest of the house style
  (4-space indent, 100-col, attach braces, left-aligned pointers). Format files
  you add or substantially rewrite, but **do not bulk-reformat a pre-existing
  file** — that buries your change under unrelated churn and trips the rule above.
  The pinned dev binary is the [`clang-format`](https://pypi.org/project/clang-format/)
  PyPI wheel (needs ≥ 16 for `InsertBraces`); install in a venv and run:

  ```bash
  pip install clang-format            # in a venv (PEP 668)
  clang-format -i path/to/new_file.cpp
  clang-format --dry-run -Werror path/to/file.cpp   # CI-style check, no edits
  # only the lines you touched in an otherwise-legacy file:
  clang-format -i --lines=START:END path/to/legacy.cpp
  ```

- **C++17, "C with destructors."** Flat, allocation-visible, RAII for handles.
  Prefer plain structs and free functions over class hierarchies; reach for an
  abstraction only once a second concrete user exists.

- **Match the surrounding idiom** — naming, comment density, error handling — so a
  diff reads like the file it lands in (the brace rule above is the one place new
  code may diverge from unbraced legacy neighbors).

## Writing review comments

Prefix each comment so its weight is unambiguous (the CI reviewer uses these too):

- **`blocking:`** must be resolved before merge (a broken invariant, a real bug, a
  missing test for changed behavior).
- **`nit:`** a minor suggestion; the author may merge without addressing it.
- **`question:`** asking to understand, not (yet) requesting a change.
- **`praise:`** call out something done well — reviews aren't only for faults.

Be specific and kind: point at the line, say what and why, and prefer suggesting a
concrete improvement over only naming the problem.
