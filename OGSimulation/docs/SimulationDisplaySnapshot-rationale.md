<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationDisplaySnapshot.h` — rationale

The narrative behind `DisplayClockReading`, `DisplayTierReading`, `DisplayCaptureWindow`,
`DisplayAppliedWindow`, `SimulationDisplayEntry`, `SimulationDisplaySnapshotT`,
`fillSimulationDisplaySnapshot` and `SimulationDisplayPublisher`. The header keeps its licence, its
docs pointer, its code, one guard tag and its `static_assert` and `OG_CHECK` messages; everything that
explains it lives here. Prohibitions are in `SimulationDisplaySnapshot-guards.md`.

**If this file and `SimulationDisplaySnapshot.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction. Everything below is true to a reader with no game engine and no other repository.

---

## §1 What it is for

A client host that steps the simulation on one thread and draws on another needs, on the drawing
side, the per-character state its displays read: the visualization state, the newest relayed input of
a remote character, the local character's recent captures, the relayed-read diagnostics, the
correction cache's applied-capture answers, the prediction clock and the connection tier. Each of
these has a reader on the drawing thread today and a writer on the stepping thread (or on the thread
that drains the network into the simulation). Reading them in place is an unsynchronized crossing.

`SimulationDisplaySnapshotT` is the hand-over: one value per step batch, filled on the stepping thread
from the simulation's own containers by `fillSimulationDisplaySnapshot`, carried to the drawing thread
by `SimulationDisplayPublisher` through a `SnapshotChannel` (`Mailbox.h`). The drawing thread then
reads values it holds, never the simulation's containers.

The header does not call itself: a host owns one publisher, calls `publish_Step` at the end of each
step batch, and points its drawing-side readers at `refreshView_GameThread` or `acquire_GameThread` (§5).

## §2 What it carries

**Per snapshot (the header):**
- `publishSequence`: 1 for the first publish, then one more per publish. A reader uses it to tell a
  new snapshot from one it has seen, and to order two it holds.
- `clock` (`DisplayClockReading`, filled by `readDisplayClock`): the prediction step's tick and step
  length (`ClientPredictionClock::getPredictionStep`), the estimator's target tick, newest authority
  tick and prediction offset, the drift action the clock would take now (`evaluateDrift`), its stall
  debt, and the seven words of its event seam (`EventSeamDiagnostics`, read through `getDiagnostics`).
- `tier` (`DisplayTierReading`, filled by `readDisplayTier`): whether a connection tier has arrived,
  its index and the tier's input delay (`ReplicatedTierConsumer`), the session's relay delay floor
  (`TimeConfig::relayDelayFloorTicks`, the one `TimeConfig` field a client rewrites while running, by
  `SimulationManager::setRelayDelayFloorTicks`), and the effective input delay the simulation runs on
  (passed in; the host reads it from `SimulationInputResolution::getClientEffectiveInputDelayTicks`).

**Per simulatable (`SimulationDisplayEntry`, one per simulatable of the snapshot's type in storage):**
- `simId`: the storage key.
- `vizState`: a copy of the simulatable's `getAllState()` (§3 and guard G-01). Its type is that
  accessor's type, which is also the type of `getVizState()` for a simulatable whose visualization
  copy is a copy of its state.
- `lastRelayedInput`: the relay source's answer (§3), by default `SimulationInputResolution::getLastRelayedInput`,
  so empty for a local character and for a remote one whose relay store is still cold.
- `hasLocalInputCache` and `localCaptures`: whether the simulatable has a local input cache (the
  answer a host uses as "is this character controlled on this peer"), and the cache's captures for the
  window of ticks of §4.
- `hasRelayedReadObservations` / `relayedReadObservations` and `hasRelayedInputArrivals` /
  `relayedInputArrivals`: whole copies of the two diagnostic rings of a remote character
  (`SimulationInputResolution.h`; the arrival ring through the relay source, §3), or default-constructed empty
  rings and `false` when the character has none. The rings are copied whole because their readers walk every slot.
- `applied`: whether the correction cache exists (`findCorrectionCache`), and for each tick of §4's
  window the cache's `getAppliedCaptureTickRef` and `slotStateProvenance` answers.

The accessors answer in the shape their sources do, so a reader can be pointed at the snapshot without
re-deriving anything: `DisplayCaptureWindow::at` answers like a capture history's `at` (a pointer, or
`nullptr` for an absent tick), and `DisplayAppliedWindow`'s `appliedCaptureRef`, `slotProvenance` and
`hasCorrectionCache` answer like the reconciliation's.

## §3 Where the fill reads, and on which thread

The fill runs on the thread that steps the simulation, after the batch's last step and before the next one: a
tick driver's client calls its game's `publishForDisplay()` there (`TickDriverConcepts-rationale.md` §11). Its
sources are the storage's simulatables, `SimulationInputResolution`, `SimulationReconciliation`,
`ClientPredictionClock` and `NetworkTimeEstimator`, and `ReplicatedTierConsumer` with its borrowed
`TimeConfig`. The stepping thread writes the storage's states, the local captures, the read
observations, the correction caches and the prediction clock. When the host drains the network on the
stepping thread too (at step start), the relay stores, the arrival ring, the tier and the floor are
written there as well, and the fill crosses nothing.

The fill reads each entry's last relayed input (`getLastRelayedInput`) and arrival ring (the resolution's diagnostics) from
the input resolution itself, so it is for a host whose drain runs on the stepping thread, or whose fill runs on the draining
thread. The tier reading reaches the fill as a value the host hands over, built where the tier arrives.

Until og-session-runtime task 13 the fill also took the relay fields from a relay source, for a host that drained on the
drawing thread (the frame-locked client): a stage copied the relay fields of every stored simulatable on the draining thread
and handed them to the stepping thread through a mailbox of the host's, so the fill read neither the relay stores nor the
arrival rings across threads. Task 13 deleted the stage, its staging function and the fill overload that took a source,
with the frame-locked client, its only user.

A drawing-thread drain would write under the fill what the step's own sweeps already read across threads: the
correction caches' landings, the relay stores the input collect reads, and the storage's membership at a registration.
Those crossings are the netcode's, priced in `ThreadingCrossings.md` (rows 1, 2 and 11); the fill would be one more reader
on the stepping thread beside the step's own, and adds no crossing of its own.

The visualization state is copied from `getAllState()`, not from `getVizState()` (guard G-01). A host's
visualization copy is refreshed on the drawing thread (`updateVisualizationAll`, `SimulationObjectStorage.h`)
and its body positions are overwritten there with interpolated render poses
(`BasicTickDriverPresenter::updateVisualization_GameThread` (`TickDriverPresenter.h`)). The snapshot carries the step's state; a host
that wants the render poses on top applies them on the drawing side, from its render snapshot, as it
does today.

`readDisplayClock` and `readDisplayTier` are separate from the fill so that the fill takes no clock,
estimator or tier type it does not need, and so that a host on which some of them do not exist passes
a default reading.

## §4 The two windows

Both windows end at `clock.predictionTick`, the tick a drawing-side reader would have asked about.

- **The capture window** (`CaptureWindow` ticks) holds, for each tick in
  `(predictionTick − CaptureWindow, predictionTick]`, whether the local input cache still has a capture
  at that tick and, if it has, the capture. Slots are indexed by tick modulo the window and each carries
  its own tick, so `at` answers `nullptr` for a tick outside the window, for a negative tick (the window
  of a session younger than the window) and for a tick the cache has evicted. A static_assert keeps the
  window no wider than `kLocalInputCacheCapacityTicks`: a wider window could only copy evicted slots.
- **The applied window** (`AppliedWindow` ticks) holds the reconciliation's answers for each tick in
  `(predictionTick − AppliedWindow, predictionTick]`, newest first. For a tick outside it,
  `appliedCaptureRef` answers a default `AppliedCaptureRef` (`NoSlot`) and `slotProvenance` answers
  `std::nullopt`, which is what the reconciliation answers for a tick it has no slot for.

A host sizes both to the widest window its readers poll.

## §5 The channel, the publisher and the reader

`SimulationDisplayPublisher` owns a `SnapshotChannel<SnapshotT, 4>` (`simulationDisplay::kChannelSlots`).
`publish_Step(fill)` takes a free slot (`beginWrite`), lets the caller fill it, stamps
`publishSequence`, and commits. Every write to the slot happens before `commit`, which is what makes
it visible to a reader; a reader never sees a snapshot the writer is still writing. The case
`SimulationDisplaySnapshot.TheReaderSeesAPublishOnlyOnceItIsCommitted` pins that a publish is seen only
once committed: with the `commit` removed, the reader's first acquire returns `nullptr`.

The writer and the reader run on different threads, and a publish may land at any instant of a read. The reader
calls `acquire_GameThread(back)` (the channel's `peekNewest`: 0 for the newest, 1 for the one
before it), reads, and calls `release_GameThread` on every pointer it acquired. A held snapshot is never
recycled, so its contents do not change while it is held. With four slots the writer always finds a
free slot while the reader holds at most two (`kMaxHeldByReader`, the channel's
`kMaxHeldKeepingNewest`) and still finds the newest committed snapshot there. A reader that holds three
lets the writer recycle the newest unheld snapshot before the reader has seen it. A reader that holds
all four leaves the writer without a slot: `publish_Step` then fails an `OG_CHECK` and returns `false`
without publishing.

**A view that follows the publishes.** A reader that keeps one snapshot from pass to pass, and swaps it
for the newest once per pass, calls `refreshView_GameThread(held)`: it releases `held` (when not null)
and then acquires the newest, and the reader keeps the pointer it returns until its next call. The
order is the channel's read rule (`Mailbox-rationale.md`, the reader paragraphs of `SnapshotChannel`): a
read runs from a peek made while the reader holds nothing to the release that leaves it holding nothing,
and within a read a peek with `back = 0` returns the read's anchor, never a later commit. Acquiring
first and then releasing the old pointer if it differs therefore never ends the read: the peek returns
the held snapshot itself, the two pointers are equal, nothing is released, and the reader stays on its
first snapshot for good while the writer keeps recycling the slots it does not hold. That was the
Unreal session's refresh from og-input-latency task P5-16 to P6-1: every client's game-thread view stayed
on the first snapshot it acquired, published before any character registered, so the brawler's
visualization found no entry and drew nothing. Releasing first is also safe for a writer on another
thread: between the two calls the reader holds nothing, a slot the writer has open is not listed, so the
peek returns the newest committed snapshot, and a re-acquired slot is marked held again. The case
`SimulationDisplaySnapshot.ARefreshedViewFollowsEveryPublish` pins it: each publish is seen at the next
refresh (sequence 1, then 2 with its entries, then each of 17 more), a refresh with no publish keeps the
same snapshot, and the writer is never refused. With the old order it fails at the second publish
(`1 == 2`).

The stress case `SimulationDisplaySnapshot.StressAReaderHoldingTwoNeverSeesATornSnapshot` runs 100,000
publishes against a reader that holds the newest and the previous snapshot at once, and checks that
every stamped field of both agrees with its `publishSequence` before and after a yield, that the pair is
ordered, and that the writer is never refused.

## §6 Sizing and cost

The template arguments are the simulatable type, the most simulatables a snapshot holds, and the two
window widths. If the storage holds more simulatables of the type than `MaxSimulatables`, the fill fails
an `OG_CHECK` and the excess is not shown. A fill rewrites only `entries[0 .. entryCount)`; a reader
reads only those (`find` searches them).

The two ring copies dominate an entry: each ring has `kRelayedReadObservationCapacityTicks` slots. The
channel holds four snapshots inline, so a publisher is several times a snapshot's size and belongs on the
heap, not on a stack. Every publish copies each entry's rings and windows once; nothing is allocated
after the publisher is built, provided the visualization state and the input type allocate nothing when
copied.

## §7 What it does not carry

- The session's composition (whether a manager exists, whether this peer runs prediction, whether a tier
  consumer exists): fixed when the session starts, before any step, and not written by a step.
- `TimeConfig`'s start-up constants (for example the rollback window and the tier delay table): set
  before the first step and never written after. The relay delay floor, the one runtime-written field,
  is carried (§2).
- The drawing side's own state: the visualization query world, render poses, and anything a display
  keeps between frames.
- The authority's state: an authority has no prediction clock, tier consumer or relay diagnostics, and
  its drawing-side readers are not this header's. A host may still fill a snapshot on an authority, as the
  Unreal session does since og-input-latency task P5-16: `readDisplayClock` and `readDisplayTier` are not
  called there, the host writes the simulation step into `predictionTick` and `stepDeltaSeconds`, and the
  fill's diagnostic reads answer whatever the authority's input resolution and reconciliation hold for each id.

## §8 Provenance

Added by og-input-latency task P2-18, with the inventory of drawing-thread readers it was sized from.
The Unreal session's game-thread readers moved to it in og-input-latency task P5-16, which filled it on the game
thread in the frame's post-physics pass, while the frame's own batch could still be stepping. og-input-latency task
P5-36 moved the client's fill to the stepping thread after the batch (§3) and added the relay source, the relay stage and
its staging function, for a host that drains on the drawing thread; og-session-runtime task 13 deleted the three with the
frame-locked client. og-input-latency task P6-1 added
`refreshView_GameThread` (§5), the Unreal session's refresh moved into the publisher with its order fixed.
