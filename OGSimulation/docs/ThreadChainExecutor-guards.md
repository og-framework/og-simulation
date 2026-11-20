<!-- SPDX-License-Identifier: MPL-2.0 -->
# `ThreadChainExecutor.h` — guards

Every prohibition that governs a line of `ThreadChainExecutor.h`. Each entry has an **opaque, stable id**. In
the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `ThreadChainExecutor.h` disagree, the header is authoritative and this file is stale.** Fix
this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved to a
"Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `ThreadChainExecutor-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is true and
actionable to a reader with no game engine and no other repository.

---

The other rules this header carries are checks, not guards: a second worker, a handle from another executor,
a dispatch after stop, a wait on the worker for a task not yet run and a stop on the worker are `OG_CHECK`s;
the host's thread-factory shape, the composed engine and the executor's identity are self-check
`static_assert`s (rationale §9).

## G-01 — `deferUntil` never holds the continuation

**Tag site:** `ThreadChainExecutor.h`, the line above `return false;` in `ThreadChainExecutor::deferUntil`.

**The prohibition.** Do not make `deferUntil` return true, and do not keep the continuation it is given to run
later, on the worker or anywhere else.

**The consequence.** The continuation is the tick driver's post-physics pass, which must run on the frame
thread: it calls the game's `onPostPhysics`, writes the driver's window and drains the step-cost ring, which
the frame thread consumes. This executor has no frame-thread event to run it from. Run on the worker, it races
the frame thread's next `startFrame`; held and never run, the frame's post-physics pass is lost. With false,
`SimulationTickDriver::endFrame` waits for the batch with `waitFor` and runs the pass on the frame thread.

**What breaks if it moves.** `ThreadChainExecutor.Deferral.DeferUntilNeverHoldsTheContinuation` fails, and the
conformance case's trace, wait count and held-deferral count differ from the fake executor's.

## Retired

No id has been retired. The next guard this header needs is G-02.
