# Villen — self-hotfix: can the appliance heal its own minor errors? (design exploration)

> **Status:** exploration / forward-looking. Not scheduled, not an engine — a
> *control plane* that would sit on top of the [`chat`](DESIGN-chat.md) engine's
> tool-calling (its §17). This note exists so that whoever builds tool-calling knows
> what it could grow into, and so the one genuinely-missing safety primitive is on
> record before anyone wires a model to a compiler.
> **Audience:** the agent who picks up [`DESIGN-chat.md`](DESIGN-chat.md) §17.

**The thesis, stated bluntly:** Villen could plausibly hotfix its own *minor,
well-localized, test-covered* software errors — and the surprising part is that the
substrate for it already exists, assembled for unrelated reasons. The gap is **not
the compiler**; it is **governance** — a safe restart/rollback path and hard scope
guards. "Self-healing appliance" decomposes into "`chat` + tools + a
build/deploy/rollback control plane," and only the last clause is unbuilt.

---

## 1. The honest answer, in three tiers

| Tier | What it fixes | Recompile? | Status |
|---|---|---|---|
| **0** | client bugs (JS/CSS/HTML) | **none** | mechanically possible **today** |
| **1** | host/engine bugs | yes, on a **paired build host** | plausible with a few hundred lines of glue |
| **2** | the trust/safety envelope around 1 | — | the part to be permanently humble about |

"Minor software error" is doing real work in that sentence: this is realistic for a
null-deref, an off-by-one, a wrong status string, a mis-rendered field. It is **not**
realistic — and must not be trusted — for subtle concurrency, the socket lifecycle,
the protocol edge, or anything security-sensitive, where a self-applied patch is
worse than the bug.

---

## 2. The substrate Villen already has (the non-obvious part)

Choices made for *other* reasons happen to be exactly what a self-healing system
needs:

- **Serve-the-client-from-disk** ([`ws_server.cpp`](../host/src/ws_server.cpp)
  `serveHttp`): the host re-reads `client/` on every HTTP request, so client fixes
  need **no build and no restart** (CLAUDE.md: *"Client-only changes are cheap"*).
- **A real verification gate**: a `ctest`/doctest suite
  ([`tests/engine_tests.cpp`](../tests/engine_tests.cpp)), a **pure engine**
  (DESIGN §9.1), and for `filter` a **CPU-reference oracle** the accelerated path
  must match. Self-modification is only ever safe behind a verifier, and Villen has
  one with teeth.
- **The cross-build glibc discipline is scriptable** (steamdeck-debugging.md §2):
  the scariest deploy failure — a binary that crash-quits *before `main()`* — is
  caught by `objdump -T new_binary | grep GLIBC_` ≤ the Deck's. A mystery for a human
  is a one-line precondition for a pipeline.
- **A restart seam**: the deploy wrapper `run-villen.sh` already launches the binary;
  making it a supervisor (relaunch loop) is a few lines.
- **Serializable, self-contained state** (DESIGN §9.2): a restart can dump and reload
  live sessions, so a hotfix needn't kick players off.
- **Containment**: the `IGame` slot (DESIGN-filter §2) means a fix scoped to one
  engine can't corrupt the others.
- **A local brain**: the [`chat`](DESIGN-chat.md) engine + §17 tool-calling, running
  *on-device* — the appliance fixes itself without phoning home, consistent with the
  no-egress ethos.

---

## 3. Tier 0 — client fixes, no recompile (already mechanical)

Because the host serves `client/` from disk per request, an agent with file-write
access patches a heatmap glitch or a promotion-dialog bug by editing the file; the
next hard-refresh has the fix. No binary involved. The whole browser-facing surface
is *already* hotfixable — all that's missing is the agent and a guardrail.

---

## 4. Tier 1 — host/engine fixes via build-test-deploy

The hard constraint is non-negotiable: **the Deck has no toolchain and a read-only
rootfs — you cross-build on a PC and copy** (steamdeck-debugging.md §1, DESIGN
§11.1). So "Villen recompiles itself" can never mean *on the Deck*; it means the
appliance drives a **paired build host** on the LAN (a PC, or a glibc-pinned
container). Granting that, the loop is assembled from existing seams:

```
localize  → the model has the source + logs; pin the fault to a file/function
patch     → propose a MINIMAL diff, scoped to an allowlisted path
build     → ship the diff to the build host; cmake --build
verify    → ctest  AND  objdump -T ≤ Deck glibc  AND  (filter) GPU==CPU-ref
            └─ any red → discard the patch, report, stop
deploy    → atomic-swap the binary into the bundle
restart   → supervisor wrapper relaunches; serialized state reloads (§9.2)
confirm   → new binary stays up N seconds and passes a smoke ping
```

Nothing there is new architecture; it is *wiring existing seams into a control
plane*. The recompile happens off-Deck; the Deck only ever receives a
verified-green binary.

---

## 5. Tier 2 — the hard parts (the real ceiling)

1. **A/B rollback is the one missing primitive that matters.** The glibc trap makes
   "deploy a binary that won't boot" a live failure mode, and a bricked appliance
   can't fix itself — the bootstrap trap. The fix is what SteamOS itself does for OS
   updates: keep the last-known-good binary, and have the supervisor revert-and-
   relaunch if the new one exits non-zero within N seconds of start. Without this,
   self-update is one bad patch from a paperweight. **This is the piece not yet in
   the repo, and it is the highest-value thing to build first.**
2. **The verifier is only as good as the tests.** Green `ctest` proves the fix didn't
   break what's covered; it says nothing about the untested surface (the WS framing,
   socket lifecycle, the admin UI). Scope `self-hotfix` to test-adjacent code.
3. **Model reliability.** A 7–8B local model (what 16 GB holds, DESIGN-chat §5) is a
   capable junior, not a senior engineer. Constrain it to small, well-framed,
   test-gated fixes; never let it free-hand the network edge.
4. **Scope guards are a feature.** A hard allowlist of touchable paths + a diff-size
   cap + "engine-internal only" keeps a minor fix from becoming a rewrite.

---

## 6. What's actually missing (the build list)

1. **Supervised A/B restart** — wrapper keeps `villen.last-good`, boots `villen.new`,
   reverts on early non-zero exit / failed smoke ping. *(Build this first; it's also
   useful for plain manual deploys.)*
2. **A build-driver tool** the model calls — patch → remote build → run the gate →
   return pass/fail + logs. A thin RPC to the paired build host.
3. **A scope allowlist + diff-size cap** enforced *outside* the model.
4. **The agent loop** — `chat` §17 tool-calling with `read_file` / `propose_patch` /
   `build` / `deploy`, plus a human-in-the-loop approval for anything past Tier 0
   until trust is earned.

---

## 7. Rejected / risky directions

- **Build *on* the Deck.** Impossible — no toolchain, read-only rootfs. Off-Deck
  build host is mandatory, not a preference (steamdeck-debugging.md §1).
- **Overwrite the running binary in place.** Don't — build elsewhere, atomic-swap the
  file, restart through the supervisor. A process editing the executable it is
  running is a race with no upside here.
- **Trust the gate for security fixes.** No. Tests prove function, not safety; route
  anything security-touching to a human.
- **Unbounded self-modification.** No. The value is in the *bounded* class; an agent
  that may rewrite the protocol is a liability, not a feature.
- **Cloud model in the loop.** Against the no-egress ethos and ships your source off
  the LAN; the local `chat` backend is the point (DESIGN-chat §16).

---

## 8. Open questions

- **Trust ramp:** human-approval-required → human-notified → autonomous, gated on a
  track record per scope. How is the track record measured?
- **Smoke-test depth:** how much must a freshly-deployed binary prove before the
  supervisor commits it (ping? a scripted self-game? a synthetic client)?
- **Provenance:** every self-applied patch as a real git commit on a branch, with the
  model's reasoning + the gate logs in the message — so a human can audit/revert.
- **The reference-oracle pattern as a self-test generator:** could the model *add* a
  failing test that reproduces the bug before fixing it, making the gate prove the
  fix rather than just "didn't break things"?
- **Relationship to tool-calling-drives-the-engines** (DESIGN-chat §17): self-hotfix
  is the most dangerous tool in that set and should ship last, behind the others.
