<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationDisplaySnapshot.h` — guards

Every prohibition that governs a line of `SimulationDisplaySnapshot.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `SimulationDisplaySnapshot.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `SimulationDisplaySnapshot-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

---

## G-01 — the snapshot's `vizState` is copied from `getAllState()`, never from `getVizState()`

**Tag site:** `SimulationDisplaySnapshot.h`, in `fillSimulationDisplaySnapshot`, inside the per-simulatable
lambda, immediately above `entry.vizState = simulatable.getAllState();`.

**The prohibition.** Do not change `getAllState()` to `getVizState()` (or to any other accessor of the
simulatable's visualization copy). The two return the same type, so the compiler accepts the change.

**The consequence.** The fill runs on the thread that steps the simulation. The simulatable's
visualization copy is not that thread's: the host refreshes it on the drawing thread
(`updateVisualizationAll`, `SimulationObjectStorage.h`, which calls each simulatable's
`updateVizState`) and `BasicTickDriverPresenter::updateVisualization_GameThread` (`TickDriverPresenter.h`) then overwrites its body
positions with the interpolated render poses on that same thread. Reading it from the step thread is
a new unsynchronized read of a drawing-thread write, which is the crossing this snapshot exists to
remove, and it publishes a state one refresh old with render poses mixed into it, which is not the
state the step produced.

**What breaks if it moves.** Nothing orders it. The line is the snapshot's only source of the
simulation state; every slice a display reads from `vizState` comes through it.
