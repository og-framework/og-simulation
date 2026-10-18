<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverStepHooks.h` — guards

Every prohibition that governs a line of `TickDriverStepHooks.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `TickDriverStepHooks.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `TickDriverStepHooks-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

The concept conformance is a `static_assert` and needs no entry; so are the owner-thread check of an
authority `beforeTick`, its `authorityTick` presence check, and every hook's step-context check (they
are `OG_CHECK`s, rationale §5). The order of the `afterTick` calls is pinned by the call-order tests
and is ordinary rationale (§4), not a guard: a reorder moves the tagged line with it, so no tag could
sit where that edit is typed.

**Provenance of the ids.** These three guards are the successors, with fresh ids, of the three guards
of the engine-hosted step hooks this header was transcribed from (rationale §8): G-01 here succeeds
that class's G-01 (one tick of release), G-02 here succeeds its G-02 (the offset is the tick minus
the physics step), and G-03 here succeeds its G-03 (the step-start time is taken in
`beforeSimulate`). The task that deletes the old class retires the old ids and names these.

---

## G-01 — The authority `beforeTick` releases the inputs of `*upcoming.authorityTick`, and of no other tick

**Tag site:** `TickDriverStepHooks.h`, in `TickDriverStepHooks::beforeTick`, immediately above
`m_doors.game.releaseInputsForAuthorityTick(*upcoming.authorityTick);`.

**The prohibition.** The authority hook hands the game exactly `*upcoming.authorityTick`. Do not
substitute another tick for it (`*upcoming.authorityTick - 1`, the server clock's current tick, the
frame's first tick), and do not delete the release.

**The consequence.** `authorityTick` is the tick the step driver simulates next: it builds the
authority's `UpcomingTick` with `serverTick + 1`, and after the integrate it `OG_CHECK`s that the step
simulated exactly that tick. The game releases the inputs that are due for the tick it is given, and
the step that follows consumes the inputs of `authorityTick`; any other tick releases inputs this step
does not consume and withholds the ones it does, and without the release the step consumes no
delayed input at all. `TickDriverStepHooks.Authority.ReleasesExactlyServerTickPlusOneOncePerStep` and
`TickDriverStepHooks.CallOrder.AnAuthorityTickReleasesBeforeTheCaptureAndPublishesInOrder` fail with
`*upcoming.authorityTick - 1`.

The release takes one tick and no count (`TickDriverGame` requires
`releaseInputsForAuthorityTick(SimTick)`), so "one tick per call" is the concept's signature, not this
guard. That the hook releases once and calls none of the game's per-frame hooks is rationale §4, and
the tests above pin it.

**What breaks if it moves.** It must stay on the authority path of `beforeTick`, after the two
checks: `beforeTick` is the first hook of a step, before the integrate that consumes the inputs, and
the release is inside the step's `t1 - t0` (rationale §5).

## G-02 — The published offset is `outcome.tick - outcome.physicsStep`

**Tag site:** `TickDriverStepHooks.h`, in `TickDriverStepHooks::afterTick`, immediately above the
`m_doors.driver.publishTickOffset_Step(...)` call.

**The prohibition.** The offset is the outcome's integrated tick minus the outcome's physics step,
both from `outcome`. Do not add or subtract 1, and do not derive it from a clock (the prediction
clock or the server clock) instead of the outcome.

**The consequence.** The offset's reader adds it to a physics step count to name the tick that step
simulates: `outcome.tick` is the tick this step integrated, so the next physics step plus the offset
is the tick the next step simulates, as long as each step advances the tick by one. A `± 1` shifts
every tick named that way. A clock moves during a client's replay, and on a Stall, a Skip or a
HardResync it does not advance one tick per physics step, while the outcome's pair belongs to exactly
one step. `TickDriverStepHooks.TickOffset.IsTheTickMinusThePhysicsStepAcrossStallSkipAndHardResync`
fails with `+ 1` (it checks the offset of every step through a Stall, a Skip and a HardResync).

**What breaks if it moves.** It must stay in `afterTick`, the only hook that has the outcome, and it is
published once per step, never during a replay (the step driver calls no hook then).

## G-03 — The step-start time `t1` is taken in `beforeSimulate`, on both roles

**Tag site:** `TickDriverStepHooks.h`, in `TickDriverStepHooks::beforeSimulate`, immediately above
`m_stepStartSeconds = m_doors.engine.nowSeconds();`.

**The prohibition.** Do not take the step-start time in `beforeTick` or in `beforePhysics`.
`beforeSimulate` runs after any replay and immediately before the normal step consumes input, which
is the instant the game's `onStepStarted` is given.

**The consequence.** `beforeTick` runs before a client's replay, so the time would be early by the
whole replay, and the step cost would count the replay in the step (`t2 - t1`) instead of before it
(`t1 - t0`). `beforePhysics` runs after the integrate, so the time would be late by the input
consumption and the integrate. Either way `onStepStarted` receives a time that is not the start of the
normal step. `TickDriverStepHooks.CallOrder.AClientTickWithAResimRunsEachHookOnceAroundTheReplay` and
`TickDriverStepHooks.CallOrder.AnAuthorityTickReleasesBeforeTheCaptureAndPublishesInOrder` fail with the
time taken in `beforeTick`.

**What breaks if it moves.** Inside `beforeSimulate` it must follow the correction capture, so the
capture's cost is in `t1 - t0` with the replay it follows (rationale §5).
