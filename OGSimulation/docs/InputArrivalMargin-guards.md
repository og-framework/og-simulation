<!-- SPDX-License-Identifier: MPL-2.0 -->
# `InputArrivalMargin.h` — guards

Every prohibition that governs how `InputArrivalMargin` is fed. Each entry has an **opaque, stable
id**. A single line `// ⛔G-nn  docs/InputArrivalMargin-guards.md` sits exactly where the forbidden
edit would be typed. All seven sites are in `ServerReceptionCoordinator.h`, the one owner that feeds
the class: the wrong edits are made at the feed, not in the class.

**If this file and `InputArrivalMargin.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is
moved to §R and its number is spent forever. A retired id may be **named** in prose; it may never
again appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** The narrative lives in `InputArrivalMargin-rationale.md`.
A guard is a prohibition plus the consequence of ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine, no other repository and no other file open.

---

## G-01 — the margin's reference is the late gate's own `lateGate`, never the receipt gate's tick

**Tag site:** `ServerReceptionCoordinator.h`, `ServerReceptionCoordinator::receiveRemoteInput`,
inside the `if (queuedNewEntry)` block (guard G-03), immediately above
`m_arrivalMargin.noteArrival(key.address, captureTick, m_inputDelayQueue.effectiveDelay(key), lateGate, firstSight);`.

**The prohibition.** Do not replace the fourth argument `lateGate` with `m_serverTick`, with a
drain's `firstUpcomingSimTick`, or with a bare `m_nextUndrainedSimTick`. Do not replace the third
argument `m_inputDelayQueue.effectiveDelay(key)` with anything but the queue's effective delay for
this key: the value the late test inside `enqueue`, just above, read, and the one `[Park]` reports.
Do not replace the fifth argument `firstSight` (guard G-04's answer) with `acceptedNew` or `true`.

**Consequence.**
- `m_serverTick` is the first tick of the frame the last drain served. It trails the first tick not
  yet drained by that frame's `numSteps`, so every sample would be overstated by `numSteps`.
  Measured: with that substitution the exact case of `InputArrivalMarginTest.cpp` reports 6 where the
  true margin is 5 (`numSteps` 1), and 6 and 7 where it is 4 and 5 (`numSteps` 2). A drain's
  `firstUpcomingSimTick` is the same value.
- A bare `m_nextUndrainedSimTick` is 0 until the first drain. Every arrival before it would record
  a margin of `captureTick + delay`, the session's tick count, into the first window. `lateGate`
  carries the late gate's "no drain yet" sentinel instead, and `noteArrival` takes no sample on it.
- With `lateGate` as the reference, a parked capture can never record a negative margin: the late
  gate refuses at least every capture whose `captureTick + delay` is below it. (That is the queue's
  first late test. Its second, a capture at or below the slot's last released capture tick, can also
  refuse a capture whose margin is not negative, for example a re-send of a released capture after the
  wire's delay grew.) Any other reference breaks that, and the margin and `lateRefused` stop
  describing one split of the same arrivals.
- The fourth argument is also the reference of the D-free sample `m0 = captureTick − lateGate`, which
  `noteArrival` records when `firstSight` is true. With `acceptedNew` as the fifth argument, a
  reordered first copy, the late side of the distribution, takes no m0 sample, and m0 reads high. With
  `true`, a capture parked a second time (for example after a wire's delay grows past a capture it
  had already refused as late) is sampled twice.

**What breaks if it moves.** Cut out of the `if (queuedNewEntry)` block, the call would also sample
duplicates and late refusals, and their margins are not arrivals. The block reads `enqueue`'s result,
so the compiler keeps it below that call. The sentinel that makes "no sample before the first drain"
hold is `InputArrivalMargin::kNoDrainYet`; the coordinator `static_assert`s that it equals
`ServerInputDelayQueue::kNoLateGate`.

---

## G-02 — a late refusal is detected by the queue's late count moving, never by `!queuedNewEntry`

**Tag site:** `ServerReceptionCoordinator.h`, `ServerReceptionCoordinator::receiveRemoteInput`,
immediately above `if (m_inputDelayQueue.lateDroppedCount() != lateDroppedBefore)`.

**The prohibition.** Do not replace this condition with `!queuedNewEntry`, with
`acceptedNew && !queuedNewEntry`, or with a re-derivation of the queue's late predicate.

**Consequence.** `enqueue` returns `false` for two different outcomes: a duplicate of a capture
that is still parked, and a late refusal. A redundant sender re-sends every capture several times
while it is parked, so `!queuedNewEntry` counts every re-send as late. Measured: at redundancy
depth 3 and a margin of 5 ticks, the window that has no late refusal at all reports
`lateRefused=233`. `acceptedNew && !queuedNewEntry` counts only a capture refused at its first sight,
and misses every late re-send of a capture already released, which the queue's count includes. A re-derived predicate can drift from the queue's own: the queue applies two
late tests, and owns them.

**What breaks if it moves.** The condition differences the queue's late count across the one
`enqueue` call just above it, and only `enqueue` increments that count, so a difference means "this
call refused the capture as late". Cut out of `receiveRemoteInput`, no late refusal reaches
`lateRefused`, and every window reports 0.

---

## G-03 — the margin is sampled on `queuedNewEntry` alone, never in the `[Park]` arm only

**Tag site:** `ServerReceptionCoordinator.h`, `ServerReceptionCoordinator::receiveRemoteInput`,
immediately above `if (queuedNewEntry)`, the block that holds `noteArrival` (guard G-01), after the
late-refusal check and before the `acceptedNew && queuedNewEntry` arm that writes `[Park]`.

**The prohibition.** Do not narrow this condition to `acceptedNew && queuedNewEntry`, and do not
fold this block into the `[Park]` arm below. Every new entry `enqueue` parks is an arrival and is
sampled, whichever of the two arms after the park it takes.

**Consequence.** When packets reorder, a capture can arrive for the first time after a newer one
from the same owner. `enqueue` parks it, but it is not `acceptedNew`, so it takes the
`else if (queuedNewEntry)` arm and is counted in `relayOooSkipCount()`. Those captures are the ones
that arrived late relative to their neighbours: sampling the `[Park]` arm alone leaves them out and
moves the margin **upward**. That overstates the slack, the wrong sign for anything that would cut
the input delay or the client lead by it: it would cut too far and get more late refusals.
Measured: in `InputArrivalMarginTest.cpp`'s reorder case, the narrowed condition samples 48 of
115 arrivals and reads p50 7 where the analytic p50 of all arrivals is 1; the jitter case samples 76
of 116.

**What breaks if it moves.** The block reads `enqueue`'s result, so the compiler keeps it below that
call. It reads nothing the `[Park]` arm or the relay tap writes, and they read nothing it writes, so
its order relative to them changes no value.

---

## G-04 — first sight is the per-owner seen mask's answer, keyed by owner id

**Tag site:** `ServerReceptionCoordinator.h`, `ServerReceptionCoordinator::receiveRemoteInput`,
immediately above `const bool firstSight = m_arrivalMargin.noteSighting(id, captureTick);`, after the
malformed-slot return and before `enqueue`.

**The prohibition.** Do not replace this call with `acceptedNew` (the owner's capture-tick
watermark) or with any other derivation of "seen before". Do not pass the wire (`key.address`, or a
hash of it) in place of the owner id `id`.

**Consequence.**
- The watermark is a maximum. A first copy that arrives after a newer capture of the same owner, which
  is how a reordered late copy arrives, reads as a re-send. Measured: with
  `const bool firstSight = acceptedNew;`, the reorder case of `InputArrivalMarginTest.cpp` reported
  `lateFirstSight` 13 where 49 first copies were late, `lateResent` 36 where there was no re-send, and
  71 m0 samples of 107 first sightings. The two-delay case also failed (46 against 47).
- Owners that share one wire (couch co-op: several slots behind one connection) send the same capture
  ticks. Keyed by the wire, one owner's sighting marks the other's tick as seen, and the second owner's
  first copies count as re-sends. Measured: keyed by the wire's hash, the two-owner case reported
  `lateFirstSight` 0 where 106 first copies were late, `lateResent` 421 where 315 were re-sends, and
  108 m0 samples of 214 first sightings.

**What breaks if it moves.** The call reads nothing `enqueue` writes, so its order relative to
`enqueue` changes no value; the compiler keeps it above its two readers. Moved above the receipt gate
or the malformed-slot return, it would also sight captures the coordinator discards: a garbage capture
tick 64 or more ticks from the owner's newest restarts that owner's mask, and the next re-send of a
recent tick then reads as a first sight.

---

## G-05 — the late split reads `firstSight`, never `acceptedNew`

**Tag site:** `ServerReceptionCoordinator.h`, `ServerReceptionCoordinator::receiveRemoteInput`,
inside the late-refusal block (guard G-02), immediately above `if (firstSight)`, the branch that
calls `noteLateFirstSight` or `noteLateResent`.

**The prohibition.** Do not replace the condition with `acceptedNew`, with `queuedNewEntry`, or with
a test of the capture's margin (for example `captureTick + delay < lateGate`).

**Consequence.** `lateFirstSight` is the late class a clock controller may act on, and `lateResent`
the one it must not: a redundancy re-send of a released capture costs no input. `acceptedNew`
misfiles every reordered late first copy as resent (the measurement under G-04). `queuedNewEntry` is
always false here, because the block runs only for a refused capture, so every late refusal would be
filed as resent. A margin test cannot tell the two populations apart: both a late first copy and a
late re-send can have a negative margin. Each substitution keeps
`lateRefused == lateFirstSight + lateResent`, so that sum cannot catch it; the reorder case of
`InputArrivalMarginTest.cpp` compares each count with the rig's own first-copy record.

**What breaks if it moves.** Out of the late-refusal block, a capture that was not refused would be
counted as late. The block reads the queue's late count across the one `enqueue` call (guard G-02),
so the split stays one-to-one with `lateRefused` only inside it.

---

## G-06 — `editArrivalMarginForFenceTest` has no production caller

**Tag site:** `ServerReceptionCoordinator.h`, immediately above
`ArrivalMargin& editArrivalMarginForFenceTest() { return m_arrivalMargin; }`.

**The prohibition.** Do not call it from a host, an adapter or the coordinator itself, and do not use
it to feed, reset or correct the margin. It exists for `ArrivalMarginFenceTest.cpp` alone, which calls
`InputArrivalMargin::scribbleForFenceTest` through it.

**Consequence.** It is the only mutable door to the margin object. The fence test uses it to prove that
no coordinator output (parks, releases, relay stamps, `[Park]` and `[Release]` text, the late count)
reads the margin: it scribbles the object mid-session and requires every output to stay
byte-identical. A production write through it would change what the `[InputArrival.Window]` line and
`signal()` report, the inputs of anything that steers a clock or a delay by them.

**What breaks if it moves.** Deleted, the fence test no longer compiles. Made `const`, it can no longer
scribble, and the test does not compile either.

---

## G-07 — a back-dating episode is written to the log, and nothing decides on it

**Tag site:** `ServerReceptionCoordinator.h`, `ServerReceptionCoordinator::maybeEmitInputArrival`,
inside the loop over the closed window's lines, immediately above `if (backDateOpening.has_value())`,
the branch that writes `[InputArrival.BackDate]`, after that wire's `[InputArrival.Window]` line.

**The prohibition.** Do not add to this branch anything but the line: no refusal, delay, drop or
re-tier of the wire's captures, no change to its input delay, no message or flag to the client. Do not
read the episode anywhere else in the coordinator either, to decide anything: not a `WindowLine`'s
`backDateOpening`, and not `ArrivalMarginSignal::backDateEpisodes` through `arrivalMargin().signal()`.

**Consequence.**
- The detector has false positives by design. The threshold sits 2 ticks below the lowest honest m0
  the clock's dead band allows, and a wire whose path delay swings slowly drops single windows below
  it. A decision on the episode punishes honest clients for their network.
- It also has false negatives by design: a constant shift small enough to keep the m0 p50 at or above
  the threshold is never seen. A block would stop only a careless shift, and cost every false positive.
- A decision that reads the episode state also makes the coordinator's outputs depend on the margin
  object. `ArrivalMarginFenceTest.cpp` scribbles the episode state mid-session and requires every
  release, relay stamp, `[Park]`/`[Release]` line and late count to stay byte-identical, so such a
  decision fails it.

**What breaks if it moves.** The opening is a member of one `WindowLine`, so out of the loop it has no
wire. Moved above the wire's `[InputArrival.Window]` line, the episode's line would precede the window
that opened it; the back-dated-client case of `InputArrivalMarginTest.cpp` requires it to follow that
window's line.

---

## §R Retired ids

None.
