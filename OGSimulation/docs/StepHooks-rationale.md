<!-- SPDX-License-Identifier: MPL-2.0 -->
# `StepHooks.h` — rationale

The narrative behind the `StepHooks` concept and the two values it passes, `UpcomingTick` and
`TickOutcome`. The header keeps its licence, its code and its self-check `static_assert`s; everything
that explains it lives here. It has no guards: every rule it carries is a `static_assert`.

**If this file and `StepHooks.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction. Everything below is true to a reader with no game engine and no other repository.

---

## §1 What the hooks are for

`SimulationStepDriver` runs one tick per physics step. Four moments in that tick belong to the
host, not the core, and the hooks are those four moments:

| hook | when | what a host does there |
|---|---|---|
| `beforeTick(const UpcomingTick&)` | first, before any resim | drain inbound messages; apply lifecycle commands (and report occupancy to the driver); on the authority, release the delayed inputs for `*authorityTick` |
| `beforeSimulate(const UpcomingTick&)` | after any replay, immediately before the normal step starts consuming input; on a client before the scratch save | take the step-start time: the instant the tick's inputs are consumed, which latency stamps posted later in the tick use |
| `beforePhysics(SimTick)` | after the integrate, before the physics world steps | a hot-path client sends its input for the tick, before the step (the GGPO order) |
| `afterTick(const TickOutcome&)` | last | encode and publish outbound data, render and visualisation snapshots |

They are called on the driver's thread, in that order, exactly once per `runTick`. A replay calls
none of them: nothing a replay produces is sent or published (`SimulationStepDriver-rationale.md`
§3).

**Why `beforeSimulate` is a moment of its own.** Neither neighbour is that instant. `beforeTick`
runs before the replay, so a time taken there is early by the whole replay; `beforePhysics` runs
after the integrate, so a time taken there is late by the integrate and everything before it. On
the authority there is no replay, and `beforeSimulate` follows `beforeTick` directly; it is still a
separate call, so a host takes its step-start time in one place on both roles.

`beforeSimulate` receives the same `UpcomingTick` as `beforeTick` in the same `runTick`, the
authority's `authorityTick` included. On a client it runs before the scratch save, so a host action
there that changes the world (for example an occupancy change through `noteOccupancy`) is inside
the world a Skip backfills (`SimulationStepDriver-rationale.md` §2).

## §2 The values

`UpcomingTick`:
- `authority`: the role of the driver calling the hook;
- `authorityTick`: on the authority, the tick this `runTick` will simulate (the server clock plus
  one; `SimulationStepDriver-rationale.md` §7). Empty on a client, whose tick is not decided until
  `advancePrediction` runs inside the integrate;
- `physicsStep`: the scheduler's step number.

`TickOutcome`: the tick and `StepKind` the manager integrated, whether a HardResync fired, the number
of ticks replayed, whether a resim request was refused (its anchor was not held), and the physics
step.

## §3 What the self-check pins

The concept requires each hook to be callable with those arguments and to return `void`, and the
header asserts:
- the reference shape satisfies it;
- a type with no `beforeSimulate` does not, so a host cannot silently skip the step-start moment;
- a type with no `beforePhysics` does not, so a host cannot silently skip the pre-step moment;
- a hook returning a value does not: a hook cannot veto, retry or reorder the tick;
- an `afterTick` taking a mutable `TickOutcome&` does not: the outcome is the driver's report, read
  only.

Every negative shape declares every member except the one property it names, with the reference
signatures. A negative shape that also lacked some other member would fail the concept for that
reason as well, and its assertion would stay true even if the property it names stopped being
enforced. That is why every shape, negative ones included, declares `beforeSimulate`, and why
`beforeSimulate` has a negative shape of its own.

## §4 Threading

The hooks run on the driver's thread. A host that needs data from another thread reaches it through
a mailbox it drains inside a hook, never by blocking in one.

## §5 Provenance

Written on 2026-10-06 with `SimulationStepDriver.h`. Nothing in this file depends on any document
outside this submodule.
