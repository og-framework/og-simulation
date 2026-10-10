<!-- SPDX-License-Identifier: MPL-2.0 -->
# `RenderInterpolation.h` — rationale

The narrative behind `RenderInterpolationParams`, `RenderInterpolationFrame`, `findRenderBody`,
`renderInterpolationAlpha`, `blendRenderPoses` and `interpolateRenderPoses`. The header keeps its
licence, its docs pointer, its code, one guard tag and its `static_assert` and `OG_CHECK` messages;
everything that explains it lives here. Prohibitions are in `RenderInterpolation-guards.md`.

**If this file and `RenderInterpolation.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: STL and glm only, no logging. Everything below is true to a reader with no game engine
and no other repository.

---

## §1 What it is for

A host that steps the simulation at a fixed rate and draws at another rate sees the simulation in
whole steps. Drawing the newest step every frame holds a body still on the frames with no new step and
moves it a whole step on the others: at 100 frames per second over 60 steps per second that is a 3:2
step/hold judder. The usual cure, and what a physics engine's asynchronous mode does, is to draw a
little in the past: pick a render time a fixed delay behind the host's clock, find the two published
steps whose deadlines bracket it, and blend their poses by where the render time falls between the
two deadlines. Every frame then moves by its share of the step, whatever the two rates.

This header is that blend, over the render snapshots of `RenderSnapshot.h` carried by a pooled
channel such as `SnapshotChannel` (`Mailbox.h`). It is the game-independent half of a render path:
- **here:** the bracket selection over the channel (§2), the alpha (§3), the blend into a pose set
  the caller owns (§4), the snap threshold (§5), the per-frame summary (§6);
- **the caller's:** the render clock (the render time is an input, §7), how often it runs, what it
  moves to the poses (actors, scene nodes, a query world), and any logging or diagnostics.

## §2 The bracket

`interpolateRenderPoses` picks two snapshots from the channel for a render time `t`:
- *next* is the **oldest** published snapshot whose `stepDeadlineSeconds` is at or after `t`;
- *prev* is the snapshot just before *next*, whose deadline is before `t`.

Three edges, each named in the frame (§6):
- **past the newest** (`atNewest`): every published deadline is before `t`. *next* is the newest and
  there is no *prev*: the newest is drawn as it is, never extrapolated. A render time exactly at the
  newest deadline draws the newest too, without counting as `atNewest`;
- **before the oldest** (`beyondOldest`): the walk reaches the oldest snapshot the channel still
  holds, or the channel's capacity, without finding a deadline before `t`. *next* is the oldest one
  reached and there is no *prev*. This is the first frames after the channel starts filling, a render
  time delayed further than the channel reaches, or a writer's recycle racing the walk (a peek that
  returns nothing ends the walk the same way);
- **newest only** (`RenderInterpolationParams::newestOnly`): the render time is ignored and the newest
  is drawn, for a host that wants to compare against no interpolation.

**Equal deadlines.** The comparison that ends the walk is strict: a snapshot whose deadline equals
`t` becomes *next* and the walk goes on. So among snapshots that share a deadline at `t`, *next* is the
oldest of them and *prev* is the one before. Its alpha is then 1 and the pose is *next*'s, which is
the pose at `t`. Two neighbouring snapshots with equal deadlines can never become *prev* and *next*,
because *prev*'s deadline is before `t` and *next*'s is not.

**The walk** (guard G-01). It peeks `back = 0` first, so the whole read sees the channel as it was
when the read began (`Mailbox-rationale.md` §9, the usage rule for a fixed read-start view). It then
peeks `back = 1, 2, …` up to the channel's capacity. While the peeked snapshot is still at or after
`t`, it becomes *next* and the previous *next* is released; the first one before `t` becomes *prev*.
So the walk holds at most two snapshots at any time, the current *next* and the one it is looking at,
and it releases *prev* and *next* before it returns, on every path. The order inside the step matters:
the older snapshot is peeked while the newer one is still held, so `back` keeps counting from a held
snapshot and a writer's commit during the walk cannot shift it. A `static_assert` demands a channel
that keeps its newest snapshot while the reader holds two (`kMaxHeldKeepingNewest >= 2`; a
`SnapshotChannel` of four slots). Another demands that the channel carries the pose set's own
snapshot type.

**How far back the walk reaches.** With a delay of `d` steps behind the host clock and steps published
at a steady interval, *prev* is at most `floor(d) + 1` snapshots behind the newest, so a channel of
`N` slots brackets a delay of up to `N − 2` steps. A caller that clamps its delay states that bound
next to its own channel. Uneven spacing (a step interval that changes) can need one more slot; the
walk then ends at `beyondOldest` rather than failing.

**Generic over the channel.** The function is a template on the channel type, which must provide
`kCapacity`, `kMaxHeldKeepingNewest`, `peekNewest(back)` and `release(snapshot)` with
`SnapshotChannel`'s meaning. That lets a test wrap a real channel to count the held snapshots, or to
commit a step in the middle of the walk.

## §3 The alpha

`renderInterpolationAlpha(prevDeadlineSeconds, nextDeadlineSeconds, renderTimeSeconds)` is (render time − prev
deadline) / (next deadline − prev deadline), clamped to [0, 1]; when the span is not positive (equal deadlines, or out of order) it
is 1, the later pose. Inside a bracket the clamp never acts: *prev* is before the render time and *next* at or
after it. The alpha comes from the deadlines alone, never from simulation ticks: a step that stalls the
tick (`StepKind::Stall`) or skips ticks (`StepKind::Skip`) is still one published snapshot with its own
deadline, so the render clock neither stops nor jumps with the simulation clock.

The frame keeps the alpha as a `double`; the blend uses it as a `float`, the precision of the poses.

## §4 The blend and the pose set

`blendRenderPoses(prev, next, alpha, snapDistanceCm, poses)` writes a copy of *next* into `poses`
and blends each of its bodies from the body with the same key in *prev* (the key and the key order are
`RenderSnapshot-rationale.md` §3; `findRenderBody` is a binary search over that order):
- the position by linear interpolation, `prev + (next − prev) × alpha`;
- the rotation by spherical interpolation, only for a body whose `hasRotation` is 1. A body without a
  rotation keeps *next*'s rotation field unblended (the identity, in a snapshot `fillRenderSnapshot`
  wrote), which the caller ignores (`RenderSnapshot-rationale.md` §5);
- at alpha 0 the body is *prev*'s pose exactly, at alpha 1 *next*'s exactly;
- a body that is in *next* but not in *prev* (a simulatable that just joined) takes *next*'s pose; a
  body only in *prev* (one that just left) is not in the pose set.

The copy carries *next*'s step identity (`physicsStep`, `tick`, `kind`, …) into the pose set. When
there is no *prev*, `interpolateRenderPoses` copies *next* unblended.

**The pose set is the caller's.** The function writes into a `RenderSnapshotT` the caller owns, sized
like the channel's snapshots, and the optional `nextCopy` receives *next* unblended (for a caller that
traces the step it rendered toward). Nothing else is written, and nothing is allocated: every type the
functions touch is a flat value (a `static_assert` on the params and the frame, another on the
snapshot), the snapshots are read in place in the channel, and the blend is a loop over a fixed array.
A caller that adds a per-body correction (for example the remaining error of a resimulation, decayed
over time) applies it to the pose set after this function, so every consumer of the pose set sees
the same pose. An `OG_CHECK` refuses a pose set that is the *prev* snapshot itself, since the copy of
*next* would overwrite what the blend reads.

## §5 The snap threshold

A body whose *prev* and *next* positions are more than `snapDistanceCm` apart takes *next*'s pose
unblended and counts as a snap. The threshold is the caller's, because what counts as a jump that is
not motion depends on the game: a pooled object parked far outside the world and brought back, a
teleport. The default is infinity, which never snaps. A respawn or a correction within the threshold
is blended like any motion, as an engine's own interpolation blends it. The comparison is strict: a
jump exactly at the threshold is blended.

## §6 The frame

`RenderInterpolationFrame` is the per-frame summary a host's diagnostics need, written only when the
function returns true (it returns false, and writes nothing, while the channel is empty):
- `renderTimeSeconds` (the input), `alpha`;
- `prevStep` / `prevKind` (valid when `hasPrev`), `nextStep` / `nextKind`, `newestStep`;
- `nextBack`: how many snapshots behind the newest *next* is;
- `snaps`: bodies that took *next* unblended (§5);
- `hasPrev` (the frame was blended), `newestOnly`, `atNewest`, `beyondOldest` (§2).

## §7 What it does not do

- **No clock.** The render time is a parameter: the host computes it from the same clock its step
  deadlines come from, minus its delay. A render time from another clock drifts against the deadlines.
- **No delay policy.** How far behind to render, and the bound that keeps the bracket within the
  channel, are the caller's.
- **No extrapolation**, by design (§2).
- **No smoothing of corrections.** A snapshot that a resimulation corrected is drawn as published; a
  caller that smooths corrections does so on the pose set (§4).
- **No logging.** Everything the function decides is in the frame.

## §8 What the tests show

`RenderInterpolationTest.cpp`, tag `[RenderInterpolation]`, over a real `SnapshotChannel` of four
`RenderSnapshotT<4>` (and a counting wrapper of one):
- `RenderInterpolation.Bracket.RenderTimeInsideThePublishedSteps`: render times between steps 2–3,
  1–2 and 3–4 of four pick those pairs, with `nextBack` 1, 2 and 0 and the lerped pose.
- `RenderInterpolation.Bracket.PastTheNewestRendersTheNewestWithoutExtrapolating`: past the newest
  deadline, and exactly at it.
- `RenderInterpolation.Bracket.BeforeTheOldestRendersTheOldestReachable`: six steps published into
  four slots, a render time before all of them.
- `RenderInterpolation.Bracket.EqualDeadlinesTakeTheOldestAtOrAfterTheRenderTime`: two steps share
  the render time as their deadline.
- `RenderInterpolation.Bracket.ASingleSnapshotAndAnEmptyChannel`: an empty channel writes nothing; one
  snapshot is `beyondOldest` before its deadline and `atNewest` after it.
- `RenderInterpolation.Bracket.NewestOnlyIgnoresTheRenderTime`.
- `RenderInterpolation.Alpha.FromTheTwoDeadlinesClampedToTheBracket`: the clamp, a zero and a negative
  span, and a bracket across a stalled and a skipped step.
- `RenderInterpolation.Blend.PositionLerpRotationSlerpOnlyWhenTheBodyCarriesOne`: a full body, a
  linear body whose rotation fields differ (not blended), a joined and a left body, alpha 0 and 1.
- `RenderInterpolation.Blend.ABodyFartherThanTheSnapDistanceTakesNextUnblended`: a parked body snaps,
  bodies at and just under the threshold blend, and the frame counts the snap.
- `RenderInterpolation.Slots.AtMostTwoHeldAndAllReleasedOnEveryPath`: every path of §2 holds at most
  two and releases all, then about 2,000 more frames with a step published before each; the writer then fills all four slots with new steps.
- `RenderInterpolation.Slots.AWriterCommitWhileTwoAreHeldNeitherDropsNorShiftsTheBracket`: the
  wrapper commits a step while the walk holds two; no drop, and the bracket is the one at the read's
  start.
- `RenderInterpolation.NoAllocation.ThePoseSetAndTheFrameAreFlatCallerOwnedValues`: the types are flat
  values, the pose set keeps its storage, and `nextCopy` receives *next*.
