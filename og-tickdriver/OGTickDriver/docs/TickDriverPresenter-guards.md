<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverPresenter.h` — guards

Every prohibition that governs a line of `TickDriverPresenter.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `TickDriverPresenter.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `TickDriverPresenter-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

Neither id below succeeds an older guard: both prohibitions were stated only in prose before this
header existed (rationale §13).

---

## G-01 — The render time is the clock's host time minus the delay in step intervals. Do not take it from the engine's wall clock or from sim ticks.

**Tag site:** `TickDriverPresenter.h`, in `buildRenderPoses` of `BasicTickDriverPresenter`, immediately above the
statement that assigns `params.renderTimeSeconds`.

**The prohibition.** Do not replace `clock.hostNowSeconds()` with the engine's `nowSeconds()` (or any other
clock), do not replace `clock.stepIntervalSeconds()` with a fixed step or the configuration's `stepSeconds`,
and do not compute the render time from sim ticks.

**The consequence.** Every snapshot carries the step deadline it was published with, and those deadlines
are in the tick driver's host time base: the time `hostNowSeconds()` reads. The interpolation brackets the
render time between two of those deadlines, so a render time in any other base selects the wrong pair or
none: an engine wall clock that is offset from the host time puts every frame at the newest snapshot or
beyond the oldest one. Sim ticks stop on a Stall step and jump on a Skip step, while each of those steps
still publishes a snapshot at its own deadline. The step interval is the scheduler's current one, which
the rate scale changes. `TickDriverPresenter.Differential.InterpolationAt100FpsAgainst60HzSteps` and every
other differential case fail with the engine's wall clock in place of the host time: the rig's wall clock
is 1,000 s ahead of the host time, so the first frame's trace line reads render=999.976667 where the
transcribed path reads the host-time render time.

**What breaks if it moves.** The statement must stay after the delay is clamped (it uses the clamped
`delaySteps`) and before `interpolateRenderPoses`, which reads `params`.

## G-02 — The held check's tolerance stays 1e-8 m (1e-6 cm). Do not raise it to the applied check's tolerance.

**Tag site:** `TickDriverPresenter.h`, in namespace `tickDriverPresenter`, immediately above the declaration
of `kHeldTolerance`.

**The prohibition.** Do not raise `kHeldTolerance` to `kAppliedTolerance` (about 1e-6 m, 1e-4 cm) or to any other larger
value, and do not make the held check compare with `kAppliedTolerance`.

**The consequence.** The two checks ask different questions. The applied check (after the apply) compares
the anchor with the pose that was requested, and must tolerate an engine that ignores a move smaller than
its own no-move threshold. The held check (before the next apply) compares the anchor with the position the
apply *returned*, which already includes that threshold; any drift larger than one rounding is something
else moving the anchor between two applies, and the anchor is what the game reads in between (for the
brawler, the aim plane point). Raised to about 1e-6 m (1e-4 cm), moves below that per frame go unreported.
`TickDriverPresenter.Differential.ChecksMovedMismatchAndShadow` nudges an anchor by 5e-7 m (5e-5 cm) between two
frames; with the tolerance at about 1e-6 m its `[SimHost.RenderApply] MOVED` Warning is missing and the case fails.

**What breaks if it moves.** Only `checkRenderTargetsHeld` reads it. The value is an og-sim length.
