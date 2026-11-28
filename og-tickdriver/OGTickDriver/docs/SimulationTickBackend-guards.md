<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationTickBackend.h` — guards

Every prohibition that governs a line of `SimulationTickBackend.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `SimulationTickBackend.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `SimulationTickBackend-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

**One live entry, G-01.** Every ordering rule of the header is held by the compiler or a run-time check, so none
needs a tag (rationale §3, §4): the member order (render source < J9 < presenter < driver) is a `static_assert` in the
constructor; the presenter built before the driver, a door reached before `start`, a registration whose owner holds
no slot, a full J9, a `FreeRunning` member called off the runner and a refused viz door are `OG_CHECK`s; the world
host's members and `build` are the two concepts. The rest of the order (BL1, the stop before teardown, the release's
occupancy push, the restore note to the driver) is pinned by the tests in rationale §6, each seen failing, and the two
halves an adapter types around the calls (its tick functions stopped before `stop()`, its world host outliving the
backend) are typed in the adapter, where its own guards cover them. No check can see a `FreeRunning` registration that
reaches the presenter on the runner, so that one is G-01.

---

## G-01 — in `FreeRunning`, a registration reaches the presenter only through J9

**Tag site:** `SimulationTickBackend.h`, in `toPresenter`, immediately above `if (freeRunning())`.

**The prohibition.** ⛔ Do not delete this branch, weaken its condition, or call `applyToPresenter` (or the
presenter's `onRegistered` or `onUnregistered`) before it. In `FreeRunning` the registration is pushed onto J9 and the
function returns; only `present()`'s drain applies it.

**The consequence.** `toPresenter` runs on the runner in `FreeRunning`. The presenter's target map is the frame
thread's: `present()` iterates it while the runner would insert into it or erase from it. That is a data race on a
hash map, with no check anywhere to catch it, and it corrupts the map or crashes the frame thread at the next present.

**What breaks if it moves.** `SimulationTickBackend.AuthorityLoop.PresenterRegistrationsCrossThroughTheRingToPresent`
fails: with the condition replaced by `false`, the presenter shows the target before the drain and drops a
forgotten one before it (measured 2026-11-13).

---

## Retired

None.
