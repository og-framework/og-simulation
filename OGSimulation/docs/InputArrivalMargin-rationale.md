<!-- SPDX-License-Identifier: MPL-2.0 -->
# `InputArrivalMargin.h` — rationale

The narrative behind `InputArrivalMargin`, the server's per-connection measure of how early each
remote input arrives before the tick that will consume it. The header keeps its licence, its docs
pointer and its code; everything that explains it lives here.

**If this file and `InputArrivalMargin.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in
`InputArrivalMargin-guards.md`, reachable from the `⛔G-nn` tag at that site. All seven of its sites are
in `ServerReceptionCoordinator.h`, because the wrong edits are made where the class is fed.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction (the STL and one `OGSimulation/` header). Where a host appears below it is "the host":
whatever program owns the connections, the clock and the log.

---

## §1 What it is

An instrument, owned by `ServerReceptionCoordinator` and fed only by it. For each new remote input
the server parks, it records one **margin**, in sim ticks:

    margin = captureTick + effectiveDelay − nextUndrainedTick

- `captureTick + effectiveDelay` is the tick the input is scheduled to be consumed on: its release
  tick.
- `nextUndrainedTick` is the first sim tick the server's drain has not yet served (§2).

So the margin is how many ticks the input sat in hand before the drain reached its tick. A margin of
0 means it arrived just in time: the very next drain consumes it on that drain's first tick. A
larger margin is slack that a shorter input delay, or a smaller client lead, could give back. A
capture that arrives after its release tick has already been drained has no margin to record. The
queue refuses it as late, and it is counted as `lateRefused` instead (§3).

The margin is kept **per connection**, keyed on the coordinator's `Address`: one wire. Local
players that share one wire (several slots behind one connection) share one margin. The input delay
is a property of the wire too, so the two agree.

Each window (§4) reduces a connection's margins to `InputArrivalWindowSummary`: the sample count,
p05, p50, p95 and the minimum, plus the late-refusal count. Since og-input-latency P3-5 it also keeps
the server's half of a clock signal: which late refusals were first sightings (§8, §9), a margin
without the delay, `m0`, with its EWMA (§9), and the capture ticks each bundle advanced (§10), reduced
per window to `ArrivalMarginSignal` (§11). Since og-input-latency P3-6 each close also runs a back-dating
detector over the wire's m0 p50 (§14). The class does not log. It hands the summaries back, and the coordinator writes one `[InputArrival.Window]` line per connection
(`ServerReceptionCoordinator-rationale.md` §8). Tests assert on the numbers, not on strings.

The measure is in ticks, at the consumer. It needs no clock shared between machines, and it is not
skewed by how long a frame took to read the packet: it compares two tick numbers the server already
holds.

## §2 The reference: the late gate's own `lateGate`

The coordinator has two tick references, and only one of them is right here.

- `m_serverTick` is fed by `reapConnections`, once per physics frame, with the **first** tick of that
  frame's drain. It is the receipt gate's reference.
- `m_nextUndrainedSimTick` is fed by the drain itself, as `firstUpcomingSimTick + numSteps`: the
  first tick **not yet** served. It is the late gate's reference.

A frame that simulates `numSteps` ticks leaves the two `numSteps` apart. Between frames, when inputs
arrive, the next tick that can still consume one is `m_nextUndrainedSimTick`. Measuring against
`m_serverTick` would overstate every margin by `numSteps`. The test's exact case is built to show
it: it fails at `numSteps` 1 and at `numSteps` 2 under that substitution (guard G-01).

The coordinator passes `lateGate` itself: the value it just handed `enqueue` as the late gate's
reference. That is `m_nextUndrainedSimTick` once a drain has run, and the queue's "no drain yet"
sentinel before. Two things follow from using the very same value.

1. **No sample before the first drain.** Before it, `m_nextUndrainedSimTick` is 0 and means nothing;
   a margin against it would be the session's tick count. The sentinel `kNoDrainYet` equals the
   queue's `kNoLateGate` (the coordinator `static_assert`s it), and `noteArrival` takes no sample on
   it.
2. **Every sample is ≥ 0.** The late gate refuses at least every capture with
   `captureTick + effectiveDelay < lateGate` (the queue's first late test), so whatever reaches the
   sample has a margin of at least 0. Arrivals that would be negative are among the late refusals.
   The queue's second late test, a capture at or below the slot's last released capture tick, can
   also refuse a capture whose margin is not negative, for example a re-send of a released capture
   after the wire's delay grew. Either way the receipt is counted as late, not sampled. So every receipt that
   reaches `enqueue` and is not a duplicate is either a sample or a late refusal, against one
   reference.

The `effectiveDelay` passed is read from the queue right after `enqueue` returns, by the same call
`[Park]` and the relay stamp make. The queue's late test read the same value inside `enqueue`;
nothing between them changes it.

## §3 What is sampled, and what is counted as late

**Samples.** `noteArrival` is called whenever `enqueue` parked a new entry (`queuedNewEntry`), before
`receiveRemoteInput` splits on `acceptedNew` (guard G-03). Both arms after the park are sampled:

- the `acceptedNew && queuedNewEntry` arm, which writes `[Park]` and stamps the relay: the first sight
  of a capture tick newer than any this owner has sent before;
- the `else if (queuedNewEntry)` arm: a genuinely new capture older than the owner's newest. When
  packets reorder, a capture can arrive for the first time after a newer one. The coordinator parks
  it, does not relay it, and counts it in `relayOooSkipCount()`.

A redundancy re-send of a parked capture is a duplicate (`enqueue` returns `false`) and is not
sampled again.

**Why both arms.** The reordered captures are the ones that arrived late relative to their
neighbours. Sampling the `[Park]` arm alone, as og-input-latency task 5 first did, leaves them out
and moves the margin **upward** under reorder: it overstates the slack, the wrong sign for a signal
that would be used to cut the input delay or the client lead. On a link without reordering the two
arms' populations are the same, and the change moves nothing. Measured in
`InputArrivalMarginTest.cpp`'s reorder case (two-point jitter: 3 ticks, or 9 with probability 0.65;
one capture per tick, so margins of 7 and 1): 115 arrivals, 82 of them slow, 67 out of order. Both arms
sampled read p50 1, the analytic value; the `[Park]` arm alone sampled 48 and read p50 7. In the jitter
case (uniform jitter of 4 ticks) both arms read p05 3, p50 4 and p95 6 over 116 samples; the `[Park]` arm
alone sampled 76 and read p50 5. `relayOooSkipCount()` still says how many arrivals were out of order.

**`lateRefused`.** `noteLateRefused` is called whenever `enqueue` refused a capture as late. The
coordinator detects it by the queue's own late count moving across that one call (guard G-02); the
queue applies two late tests and owns them both. So `lateRefused` is the queue's late class,
attributed to a wire. It includes:

- a capture whose first copy arrived after its release tick was drained (a negative margin); and
- a redundancy re-send of a capture the server had already released.

In the steady state the re-sends arrive while their capture is still parked, so they are duplicates
and not late. When the slack shrinks below the spacing of the re-sends, they start to count. Summed
over the connections of one window, `lateRefused` equals `[InputStats]`'s `late=` for that window:
the two share the window (§4) and the counter. Since og-input-latency P3-5 each late refusal is also
filed as a late first sight or a late re-send (§9).

## §4 The window

The window is **server-tick time**, of the same length as `[InputStats]`'s: about 2 s of ticks,
derived from `TimeConfig::tickFrequency` by the coordinator's one helper for both. The coordinator
closes it from `reapConnections`, on the same call as `[InputStats]` and before the dwell gate. Both
windows open on the first reap and close when `serverTick` has advanced by the window length since
the last close. So they close on the same frames, and a window's `[InputArrival.Window]` lines can be
read next to its `[InputStats]` line.

- The length is computed once, when the coordinator is constructed, from the configuration it
  borrows. `[InputStats]` reads it on each call. The two agree as long as the host sets the tick
  frequency before it builds the coordinator, and does not change it afterwards.
- A connection gets a line only for a window in which it recorded a sample or a late refusal. An
  idle server and an idle connection are quiet.
- A line can carry `samples=0` with a non-zero `lateRefused`: every arrival in that window was late.
  Its margin fields are then 0 and mean nothing; read `samples` first.
- If the server's tick goes backwards (a clock restart), the open window stays open until the tick
  has advanced a window length past the old start. `[InputStats]` does the same.

**Percentiles are nearest-rank** over the window's sorted margins: the value at rank
`ceil(p · n / 100)`, at least 1. The samples are whole ticks, and an interpolated "4.6 ticks" would be
rounded before anyone compared it with a delay. The same definition the relay probes use.

**`latest(addr)`** is the summary of the last window that carried something for that connection. It
stays through quiet windows, and goes when the connection is forgotten. The coordinator exposes the
object read-only through `arrivalMargin()`, so a consumer of the margin as a signal can read it
without a second instrument.

## §5 Lifetime and memory

The class is keyed on `Address`, like the coordinator's other per-wire state. The coordinator calls
`forgetDeadWires` on the same dwell-gated reap that prunes its `[DelayShift]` memo, and that forgets
every connection whose `Address` reports it is no longer alive. A dead wire's open window is dropped,
not emitted. `forget` removes one connection.

Per connection the class also keeps the back-dating state of §14: four small fields that go with the
connection, so a forgotten wire, or a client that comes back on a new wire, starts its grace again.

Per owner id the class also keeps the seen mask of §8 and the step mark of §10: one small map entry.
The coordinator's `forgetOwner` calls `forgetOwner` here, on the adapter's unregister path, so the
map is bounded by live owners. It is keyed by owner, so the wire reap does not touch it.

A window holds one `int32_t` per sample. Each sample is a new, strictly increasing capture tick for
one owner, and the receipt gate bounds captures to a fixed range around the server's tick, so a
window holds at most about the window length plus that range per owner on the wire. Clearing at the
close keeps the storage for the next window.

## §6 Thread and ownership

Game thread only, the coordinator's thread. Nothing here is synchronized, and nothing needs to be:
every call comes from `receiveRemoteInput` (on receipt) or `reapConnections` (once per frame), and
both run on that thread.

## §7 Tests

`InputArrivalMarginTest.cpp` in the og-simulation-tests suite, tags `[Network][InputArrivalMargin]`.
It drives the real coordinator through the seeded `LinkModel` helper. Its exact case derives each
expected margin in its own setup from `captureTick + D − nextUndrainedTick` at `numSteps` 1 and 2, and
it was seen failing with the reference replaced by `m_serverTick`. Its jitter case checks p05 within
one tick of the analytic value. Its reorder case checks that every parked arrival is sampled and that
p50 equals the analytic value; it was seen failing with the sample narrowed to the `[Park]` arm
(guard G-03). The other cases cover the late split, re-sends as duplicates, no sample before the first
drain, the window's cadence, the dead-wire reap, and the class on its own. The P3-5 cases check the
late split and m0 against the rig's own record of first copies, under loss, re-sends, reorder, two
delays and two owners on one wire; the step sample on the bundle path; and the EWMA's convergence
(`InputArrivalMarginTest-rationale.md` in the suite). The P3-6 cases (`[BackDate]`) check the detector of
§14: an honest jittered session, a back-dated client, dips, a join, and the rule's edges on the class
alone. `ArrivalMarginFenceTest.cpp` is the fence of §13.

## §8 First sight: the per-owner seen mask

`lateRefused` mixes two populations that mean opposite things to a controller (§3): a first copy that
arrived after its release tick (input really was late) and a redundancy re-send of a capture already
released (nothing was lost). og-input-latency P3-5 splits them, and needs to know, for each receipt,
whether this owner's capture tick has been seen before.

`noteSighting(ownerId, captureTick)` answers that. Per owner it keeps the highest capture tick sighted
(the anchor) and a 64-bit mask: bit `k` set means tick `anchor − k` was sighted. A newer tick shifts the
mask up and becomes the anchor; an older one tests and sets its bit. It returns true exactly once per
(owner, tick).

**Why not the watermark.** The coordinator's `acceptedNew` is a maximum per owner. It names first sight
only for in-order traffic, and a first copy that arrives after a newer one, which is how a reordered late
copy arrives, reads as seen. The reordered tail is the late tail, so the watermark misfiles exactly the
captures the split exists for (guard G-04 has the measurement).

**Why 64 bits is exact.** The receipt gate admits only capture ticks in
`[serverTick − rollbackWindowHardCap, serverTick + hardResyncThresholdTicks]`, and it is fed once per
frame, last write wins. Within a session the server tick does not go back, so the anchor, admitted at
some earlier server tick, is at most `serverTick + hardResyncThresholdTicks`, and any capture admitted
now is at least `serverTick − rollbackWindowHardCap`. The distance is at most the sum, 41 with the
defaults. The coordinator `static_assert`s the sum below `kSeenMaskTicks` on `TimeConfig{}`, and
`OG_CHECK`s it on the configuration it is built with.

**Outside the mask.** A tick 64 or more from the anchor, either way, restarts the owner's mask at that
tick and reports a first sight. That happens only before the receipt gate is armed (before the first
reap) or after the server clock restarts. Without the restart, a restarted clock would leave every new
tick below a stale anchor. The cost is bounded: a far stray tick before arming can make one later
re-send read as a first sight.

**Keyed by owner id, never by wire.** Several owners can share one wire (couch co-op: one slot each
behind one connection, slots 0 to `ConnectionSlotKey::kMaxPlayerSlot`), and they send the same capture
ticks, from one clock. Keyed by wire, one owner's
sighting would mark the other's tick as seen. Everything else in this class stays per wire.

**Where it is called.** In `receiveRemoteInput`, for every capture that passed the receipt gate and has
a valid slot, before `enqueue`. So every receipt that reaches the queue has been sighted, whatever the
queue then decides. The mask reads nothing the queue writes, and the queue reads nothing of it.

## §9 The late split, and the D-free margin m0

**The split.** Whenever `enqueue` refused a capture as late, `noteLateRefused` counts it, as before
(§3). Then, on the sighting's answer, `noteLateFirstSight` counts it as a late first sight, or
`noteLateResent` as a late re-send. One `if`/`else` at the one site, so
`lateRefused == lateFirstSight + lateResent` on every line. That sum cannot tell a right split from a
wrong one, so the tests compare each count with the rig's own record of first copies.

**m0.** `m0 = captureTick − lateGate`, the margin without the delay: how many ticks before the drain's
next tick the capture's own tick was. For a parked capture, `m0 = margin − D`. A late first copy refused
by the queue's first late test has `m0 < −D`. m0 is sampled at every **first sighting** with a drain
reference:

- parked: `noteArrival(…, firstSight)` records the margin as before and, when `firstSight` is true,
  m0;
- late: `noteLateFirstSight` records m0;
- before the first drain (`kNoDrainYet`) neither takes a sample.

A capture that the queue parks is a first sighting, except in one case: a capture it had already refused
as late can be parked later, for example after the wire's delay grew. `firstSight` keeps that one out
of m0.

**Why D-free.** Neither `captureTick` nor `lateGate` depends on D, and neither does which captures are
first sightings. So the m0 distribution is the same at any D: the test runs one session at D = 1 and at
D = 6 and requires the same samples, p05, p50 and EWMA. The D-inclusive margin of §1 is censored at the
late gate (below 0 it is a refusal, not a sample); m0 is not, because late first sightings are sampled
too. The margin fields keep their meaning, parked captures only: the `[InputArrival.Window]` line only
appends.

**The EWMA.** Per wire, seeded by the first m0 sample, then `ewma += α · (m0 − ewma)` with
`α = 1 / tickFrequency` per sample: a time constant of about one second at one sample per tick. It is a
filter, so a window's close does not reset it. It is reported as `m0EwmaQ4`, the value times 16 rounded
to the nearest integer (1/16 tick), saturated to `int16_t` (±2048 ticks). A tick frequency below 1 gives
`α = 1`: the EWMA is then the last sample.

**The delay.** `effectiveDelayTicks` is the delay the queue held for the wire at the last parked first
sighting, kept across windows; 0 before the first.

## §10 Steps per bundle

`receiveInputBundle` takes the newest capture tick among the bundle's slots that the core handled
(`ReceiveRemoteInputResult::parked`: in the server's domain, a valid slot, whether parked, a duplicate or
late) and calls `noteBundleNewest(addr, ownerId, newest)` once per bundle. The step is that tick minus
the owner's previous mark:

- in `[1, 16]` it is sampled, per wire;
- `≤ 0` (a reordered older bundle) is not sampled and does not move the mark, so the next in-order
  bundle steps from the newest ever seen;
- above 16 (a gap: a join, a long loss burst) is not sampled but moves the mark;
- 64 or more below the mark moves it too, as the seen mask restarts (§8).

The window reports the p50. A sender that captures once per send reads 1; a 30 frames per second sender
against a 60 Hz server, two captures per send, reads 2. A single lost bundle makes the next step 2, which
the p50 ignores. `receiveRemoteInput` alone does not feed it, so a host or test that bypasses the bundle
path reads 0.

## §11 `ArrivalMarginSignal`, and the appended fields

At each close, a connection that gets a line also gets an `ArrivalMarginSignal`:

| field | meaning |
|---|---|
| `windowEndServerTick` | the server tick the window closed on |
| `samples` | m0 samples: first sightings with a drain reference (§9) |
| `m0P05Ticks`, `m0P50Ticks` | nearest-rank percentiles of m0 (§4's definition) |
| `m0EwmaQ4` | the m0 EWMA in 1/16 tick (§9) |
| `lateFirstSight`, `lateResent` | the late split (§9) |
| `stepsPerBundleP50` | §10; 0 when the window had no step sample |
| `effectiveDelayTicks` | the delay at the last parked first sighting (§9) |
| `backDateEpisodes` | back-dating episodes opened on this connection so far (§14); not reset at a close |

`closeWindowIfDue` hands both back, one `WindowLine` per connection, and `signal(addr)` keeps the last
one, as `latest(addr)` does. The coordinator appends five fields to `[InputArrival.Window]`, after
`lateRefused=`:

    lateFirstSight=%u lateResent=%u m0P05=%d m0P50=%d stepsPerBundle=%d

Every existing field and its position is unchanged. Which connections get a line is unchanged too: a
sample or a late refusal in the window. A window with only step samples gets no line, and those samples
are dropped at its close. The line's fixed text is 154 characters of the 255 the log macro's buffer
holds. A 20-digit `conn=` with four-digit counts measures 210.

## §12 Who reads it

Nothing in this tree decides on these fields: they are a log line and a read-only accessor (`signal()`).
The back-dating detector of §14 reads `samples` and `m0P50Ticks` inside this class, and only reports.
They are the server's half of a signal three consumers were designed against:

- **A client clock controller** (og-time-dilation's MarginTrim): it steers on the m0 EWMA, and its "late"
  (`m0 + D < 0`) is `lateFirstSight`, not `lateRefused`. A controller that read `lateRefused` with
  redundancy on would speed clients up for re-sends that cost nothing. It also needs
  `stepsPerBundleP50`, so that its target respects `m0* ≥ 2k − 1 − D` for `k` captures per send.
- **The input-delay floor steps** (og-input-latency task 17): the "server margin p05 ≥ 1 tick" criterion
  reads `m0P05 + D` over first sightings.
- **A session-adaptive input delay** (og-input-latency's follow-up): the tail of each sender's first-sight
  margin, the baseline for detecting back-dated capture ticks, and the `2 · kmax − 1` bound from
  `stepsPerBundleP50`.

## §13 The fence

No decision of the coordinator may read this object: parks, releases, relay stamps, the `[Park]` and
`[Release]` text and the late count must not depend on it. `ArrivalMarginFenceTest.cpp` proves it. It
runs one seeded session twice, the second time calling `scribbleForFenceTest(seed)` at tick 600 through
the coordinator's `editArrivalMarginForFenceTest()` (guard G-06), and requires every output to stay
byte-identical.

`scribbleForFenceTest` overwrites the mutable state with seeded values: each owner's anchor moves 1 to
40 ticks up and its mask bits become random, so the next ticks' sightings come out wrong; and each
connection gets extra samples, counts, an EWMA, a delay and step samples, and random back-dating state
(§14): its count of windows with samples, its streak, whether an episode is open, and one to three more
episodes; the window's start moves back by less than one window. The anchor moves up, not down. Below a lowered anchor, new ticks would still
shift in as first sightings, and the only misreads would be re-sends taken for first sightings. The
queue refuses those anyway, as released (its second late test) or as duplicates, so the fence's probe
would have little to act on. Above it, a raised anchor with random bits takes some genuinely new ticks
for re-sends, which is what lets the probe change a decision. The counts it adds keep
`lateRefused == lateFirstSight + lateResent`.

Seen failing: with `enqueue`'s late gate skipped whenever `noteSighting` returned true (a probe that
lets the mask steer the queue), the releases, the relay calls, the `[Park]`/`[Release]` lines and the
late count all differed between the two runs.

Since og-input-latency P3-6 the test also requires that the scribble reached the episode state: the two
runs' final `backDateEpisodes` differ, and so do their `[InputArrival.BackDate]` lines.

## §14 Back-dating detection

**What it looks for.** A client can stamp its captures older than they are, so that they seem to arrive
earlier than they did and dodge the input delay. Rewriting a tick it has already sent gains it nothing:
the server's queue keeps the first copy of a tick. A gain needs a constant shift, every input held back
the same k ticks, and that lowers the wire's m0 (§9) by k. m0 is the right signal because it is free of
the delay: the same shift reads the same at any D.

**The rule, at each window close, per connection** (`trackBackDate`):
- A window with no m0 sample does not count at all: it neither ages the grace nor touches a streak or an
  episode. Such a window is one with only late re-sends, and it gets a line with `samples=0`.
- **Grace.** The connection's first `kBackDateGraceWindows` windows with samples never count, whatever
  their m0. At the coordinator's 2 s window that is 10 s, and it covers a join: with a static delay floor
  the only delay change in a session is the join's stall from the no-tier fallback up to the floor, and a
  joining client's lead is still converging.
- **A qualifying window**, from the next one on: at least `kBackDateMinSamples` m0 samples and
  `m0P50Ticks` strictly below the threshold. A window with samples that does not qualify (too few, or at
  or above the threshold) resets the streak.
- **Opening.** `kBackDateSustainWindows` qualifying windows in a row open an episode: the close that
  completes the run hands back an `ArrivalBackDateOpening` in that connection's `WindowLine` (the
  window's m0 p50, the run's length, the threshold, the window's `effectiveDelayTicks`), and
  `backDateEpisodes` counts it. The coordinator writes it as one `[InputArrival.BackDate]` line.
- **While open,** nothing more is reported, however low m0 goes.
- **Closing.** The first window with samples whose `m0P50Ticks` is at or above the threshold closes the
  episode, whatever its sample count. The next episode needs a fresh run.

The threshold is `TimeConfig::backDateM0P50ThresholdTicks`, passed in by the coordinator at
construction: the class takes the value, as it takes its window length and tick frequency, not the
configuration. `TimeConfig-rationale.md` §13 records the honest band behind it and the trade it makes:
the band reaches down to about −3, so a threshold two ticks below it should not flag an honest wire, and
a shift too small to bring the p50 below the threshold goes unseen.

**Why five windows, and why the p50.** One window's p50 moves with the path, not only with the client.
A wire whose path delay swings slowly (a per-2 s draw of ±3 ticks in the honest test case) puts single
windows below the threshold, and a delay spike of a few seconds puts a short run there. Requiring five
in a row (10 s) passes both, and still opens on a client that stays shifted. The p50 rather than the
p05: the tail of an honest wire is its worst arrivals, which jitter owns, while a constant shift moves
the whole distribution, its middle included.

**Detect only.** Nothing is refused, delayed or flagged to the client, and no decision of the
coordinator reads the episode state (guard G-07). The fence of §13 scribbles the episode state too.
