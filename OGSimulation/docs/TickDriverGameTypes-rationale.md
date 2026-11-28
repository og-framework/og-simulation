<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverGameTypes.h` — rationale

The narrative behind `TickRateDoor` and `IdleWake`, the two types a tick driver's game hooks receive or return. The header
keeps its licence, its docs pointer and its code; everything that explains it lives here. It states no prohibition, so it
has no guards document.

**If this file and `TickDriverGameTypes.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: the STL and `SimulationScheduler.h` only. Everything below is true to a reader with no game engine and no
other repository.

---

## §1 What it is for

`TickRateDoor` is the argument of a game's `beforePump` hook, and `IdleWake` is what a game's `idleUntil` hook returns
(`TickDriverConcepts-rationale.md` §3 and §8). Code that hands a game the door or answers the idle hook needs the two types
but not the concepts, so they are declared here, in a header of their own, and `TickDriverConcepts.h` includes it. A clock
controller that adjusts the step rate through the door includes this header, and so does an idle hook over a network
endpoint, without the concepts' requirements, self-checks and their includes.

## §2 `TickRateDoor`

`TickRateDoor` is the only part of the scheduler a game reaches. `setRateScale`, `rateScale`,
`lostTime`, `physicsStepCount` and `nextDeadline` forward to the same-named `SimulationScheduler`
members, and `currentInterval` to `SimulationScheduler::currentStepInterval`, the interval of the
scheduler's newest deadline segment: the spacing from `nextDeadline()` to the deadline after it, and
exactly `dtSeconds / rateScale()` (a rate change applies from the next deadline on,
`SimulationScheduler-rationale.md` §5). It holds a pointer to the scheduler, which must outlive it. The
scheduler has no synchronization, so the door is used only on the scheduler's owning thread, which is
where `beforePump` runs.

The constructor takes three more arguments, and the door's owner passes all of them; none has a
default:
- `tickOffset`, returned by `tickOffset()`: the offset from a physics step to the sim tick that step
  integrates, as the owner last saw it. The door copies the value when it is built, so it stays the
  same for the door's lifetime. A tick driver passes its published offset (`SimulationTickDriver-rationale.md` §5, J3);
- `refusesRateChange`: when true, `setRateScale` changes nothing. The scheduler's rate, interval and
  deadlines stay as they were, and the call only adds one to the refusal counter. Every call counts:
  a value equal to the current scale, or a NaN, is refused and counted too. A tick driver passes its
  role, so an authority's door refuses and a client's applies (`SimulationTickDriver-guards.md` G-11);
- `refusedRateChanges`, a counter the door holds by pointer and reads back in `refusedRateChanges()`.
  It belongs to the owner and must outlive the door. A driver builds a new door for each pump, and the
  count carries over from one door to the next, so it covers the whole session. A door that applies
  its calls never writes the counter.

## §3 `IdleWake`

The three ways an idle hook can end: `Deadline` (the wait reached its deadline), `Activity` (it ended early because work
arrived and was handled) and `Unavailable` (it did not wait). What each value obliges the hook to have done, and what the
caller does after each, is the hook's contract: `TickDriverConcepts-rationale.md` §8.

## §4 Provenance

`TickRateDoor` and `IdleWake` were written for the og-tickdriver migration in `TickDriverConcepts.h` (`IdleWake` on
2026-11-12; `TickRateDoor` reached its present form on 2026-11-20, `TickDriverConcepts-rationale.md` §12). On 2026-11-24
og-session-runtime task 18 moved both, unchanged, to this header, so that og-simulation's clock controller and og-sim-net's
idle hook could name them without including the concepts header. Nothing in this file depends on any document outside this
submodule.
