<!-- SPDX-License-Identifier: MPL-2.0 -->
# `Network/RelayReadProbe.h` + `Network/RelayWritePathProbe.h` — rationale

The two headers keep a one-line guard at every site that has one, plus an orientation block
naming each object's thread, its feeder and what closes its window. **This file holds the full
text those guards were compressed from** — the derivations, the rejected alternatives, the
measurement archive and the migration history.

**One document for two headers**, because the five probes are one instrument: the client-side read
and arrival probes measure the EFFECT whose CAUSE the server-side write and budget probes measure,
and the frame-health probe sits between them on both roles. The `§N` marks in either header
resolve to the sections here.

**If this file and a header disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften a header to match it.

⛔ **Do not move a guard into this file.** Every section below is the *expansion* of a fence that
still fires at its own line. If you find yourself deleting a one-liner because "it is in the
rationale doc now", the guard has stopped working — the person about to make the change is reading
code, not documentation.

⚠ **Some of what §16 quotes was FALSE when it was quoted.** Task 35 verified the load-bearing
claims against the tree before compressing them (rule R0) and found five defects; they are listed
in §14 with their evidence. The archive keeps the false text verbatim, because that is what an
archive is for. **The corrected statement is in the headers and in §14, and nowhere else.**

⚠ **Provenance note.** These headers were written inside a netcode initiative whose task numbers
(`T19`, `T20`, `T22`, `T34`, `T38`, `T39`, `T43`, `T49`, `item 34`, `item 63`, `RN-13`) resolve to
nothing for a reader with no workspace. Task 35 moved that provenance out of the headers and into
§15, where a doc can carry it; the headers now name only symbols and roles.

<!--
  LINT-ANCHOR ESCAPES. Every token below is intentionally unresolvable, and each
  says why. `doc_anchor_lint.ps1` prints them all, because an escape nobody reads
  is a way to silence a real hit.
-->
<!-- lint-external-ref: ServerFrameProbe -- RETIRED NAME: PROBE 4's type was renamed to FrameHealthProbe when it gained the client role. The old name must not resolve; SS9 and SS14 name it as history -->
<!-- lint-external-ref: SimulationNetSync::collectInputAll -- DEAD OWNER, the subject of F-35-1: the method lives on SimulationInputResolution. Quoted here unrepaired because SS16 is an archive; the shipped header states the correct owner. `CorrectionCache-rationale.md` declares the same token for the same reason -->
<!-- lint-external-ref: USimmableUpdateComponent::OnRep_RelayedInputRing -- DEAD SYMBOL, the subject of F-35-2: the OnRep moved to ASimulationInputRelay with the property it notifies. It must not resolve -->
<!-- lint-external-ref: kMaxTrackedGap -- NEVER EXISTED, the subject of F-35-7: the real constant is kRelayArrivalMaxTrackedGap. Quoted so the defect is on the record; it must not resolve -->
<!-- lint-external-ref: FReplicationWriter -- ENGINE TYPE, outside every scan root by construction: this core is engine-free. Named once as ONE adapter's binding -->
<!-- lint-external-ref: FReplicationWriter::HandleDroppedRecord -- ENGINE MEMBER, same reason: the near-miss send-success signal that recovers the changemask and not the values -->
<!-- lint-external-ref: UNetConnection::Tick -- ENGINE MEMBER, outside every scan root: the source of the per-tick allowance formula in SS12 -->
<!-- lint-external-ref: UDataStreamChannel::Tick -- ENGINE MEMBER, outside every scan root: one adapter's writing channel -->
<!-- lint-external-ref: UWorld::NotifyControlMessage -- ENGINE MEMBER, outside every scan root: one adapter's rate-negotiation path -->
<!-- lint-external-ref: SendBuffer -- ENGINE FIELD, outside every scan root: half of one adapter's readiness test -->
<!-- lint-external-ref: PktLoss -- ENGINE NETWORK-EMULATION KNOB, outside every scan root: the configured loss percentage SS12 contrasts against the ack-derived one -->
<!-- lint-external-ref: dFrame -- A PROSE VARIABLE, not a symbol: the frame delta between two PROBE 4 invocations, named only in the log line and in these formulas -->
<!-- lint-external-ref: serverFrameRate -- A PROSE VARIABLE in the relation `G ~ 60 / serverFrameRate`; the shipped setting is named by role, not by identifier -->
<!-- lint-external-ref: previousNewest -- A PROSE VARIABLE naming the lower bound of the half-open interval the loss clamp argues about -->
<!-- lint-external-ref: prevCaptureTick -- A PROSE VARIABLE in PROBE 5's consecutive-tick test -->
<!-- lint-external-ref: writingFrames -- A PROSE VARIABLE in PROBE 5's observability inequality -->
<!-- lint-external-ref: RelayDepthCoverageHypothesis.md -- WORKSPACE ARTEFACT outside both git trees, recorded as provenance in SS15 and reachable to nobody but the initiative archive -->
<!-- lint-external-ref: ReviewNotes.md -- WORKSPACE ARTEFACT, same reason -->
<!-- lint-external-ref: impl/pie_script_t22.md -- WORKSPACE ARTEFACT, same reason -->
<!-- lint-external-ref: finding_task43_resim_gate_live.md -- WORKSPACE ARTEFACT, same reason -->

---

## 1. The five probes, and which header holds which

| | probe | object | header | thread | window closes on |
|---|---|---|---|---|---|
| 1 | scheduled-read outcome | `RelayReadProbe` | `RelayReadProbe.h` | PHYSICS | the prediction tick |
| B | scheduled-read miss class | `RelayReadProbe` | `RelayReadProbe.h` | PHYSICS | the prediction tick |
| 3 | D4 stale-hold run | `RelayReadProbe` | `RelayReadProbe.h` | PHYSICS | the prediction tick |
| 2 | relay-ring arrival cadence | `RelayArrivalProbe` | `RelayReadProbe.h` | GAME | 120 gap samples |
| 4 | frame health | `FrameHealthProbe` | `RelayReadProbe.h` | GAME, either role | 120 samples |
| 5 | relay writes per server frame | `RelayWriteProbe` | `RelayWritePathProbe.h` | GAME, server | 120 completed runs |
| 6 | per-connection send budget | `ConnectionBudgetProbe` | `RelayWritePathProbe.h` | GAME, server | 120 samples |

The two files are split along a **disposability** line that no longer holds and is worth stating
plainly, because the header used to argue it at length. `RelayWritePathProbe.h` was created as a
diagnostic unit with a stated end date: if its measurement had come back "the mechanism is
elsewhere", deleting the header, its two call sites and its test file would have removed the whole
instrument in one reviewable change. That is no longer true — see §11 — and the surviving reason
for the split is simply that the two files sit on opposite sides of the wire.

**Feeders, as shipped.** No probe is called from the class that owns the algorithm it measures;
every write goes through a telemetry sibling:

| probe write | called from | which is called from |
|---|---|---|
| `RelayReadProbe::notePredictionRead` | `InputResolutionTelemetry::emitPredictionInputRead` | `SimulationInputResolution::collectInputAll` |
| `RelayReadProbe::noteResimRead` | `InputResolutionTelemetry::emitResimScheduledRead` | `SimulationInputResolution::collectResimInputAll` |
| `RelayArrivalProbe::noteArrival` | `NetSyncTelemetry::emitRelayArrival` | the relay-ring arrival callback that `SimulationNetSync::registerPredictionOwner` binds |
| `FrameHealthProbe::noteFrame` | the adapter's per-frame game-thread hook | — |
| `RelayWriteProbe::noteWrite` | the adapter's relay-write tap | — |
| `ConnectionBudgetProbe::noteSample` | the adapter's per-frame per-connection sample | — |

⚠ **That table is the correction, not the original.** Both headers named `SimulationNetSync` as the
owner of the two collect paths; it has not owned them since the input-resolution extraction. §14,
F-35-1.

---

## 2. PROBE 1 — the four scheduled-read outcomes, and why not two

`resolveScheduledRelayedInput` is a three-rung ladder, and it serves `fallback()` from **three**
different situations that behave identically and mean completely different things. Collapsing the
result to hit/miss is the mistake the enum exists to prevent.

- **`NoProbe`** — rung 0, `!findLatest().valid`. Nothing has ever arrived for this character: the
  pre-registration / join window. It is **not** starvation. There is no data yet because the
  channel has not started, and counting it as starvation would make the join window look like a
  fault on every single join.
- **`Hit`** — the probe at `tick - dLatest` found a candidate *and* that candidate's own stamp
  equals `dLatest`. The proxy is consuming the server's actual schedule, which is the whole claim
  of the scheduled regime.
- **`Miss`** — nothing resident at the scheduled capture tick. Starvation: the entry we should be
  running on has not arrived, or has been evicted. It also covers the `tick < dA` early-session
  case, where the probe tick would underflow.
- **`VerifyFail`** — a candidate *was* resident but its stamp differs from `dLatest`. The delay
  REGIME shifted under the reader: the data is arriving fine, but the schedule it was stamped
  against is no longer the current one, so replaying it would reproduce a schedule the authority
  is not using.

**`Miss` and `VerifyFail` are the pair that must never be merged.** A window that is all `Miss`
says the wire is starving the proxy. A window that is all `VerifyFail` says the wire is healthy and
the delay is thrashing. Same fallback behaviour, opposite diagnosis — and distinguishing them is
exactly what tells "the schedule is working" from "the schedule is thrashing".

**The report struct.** `resolveScheduledRelayedInput` hands its caller a `ScheduledRelayedReadReport`
so the caller can count and log without the ladder itself gaining any state. Every field except
`outcome` is diagnostic detail for the per-event Verbose line, and several fields are meaningless
on the outcomes that never computed them — `probeTick` on `NoProbe` and on the underflow guard,
`dLatest` on `NoProbe`, `candidateDA` on anything but `Hit` and `VerifyFail`. On `VerifyFail`,
`candidateDA` is the value that differed, which is the single most useful number in a
delay-transition trace.

---

## 3. PROBE B — the three miss classes, and why a second enum

The four outcomes record **that** a scheduled read missed. They cannot say **why**, and that gap
left one measured client at a 0.8 % hit rate unexplained while the coverage model predicted ~50 %.
The three whys have three different remedies, and picking the wrong one is the failure this enum
exists to prevent.

- **`InSpan`** — `probeTick` lies between the store's oldest and newest resident capture ticks and
  is absent. A **coverage hole**: the sender produced that tick and it was clobbered in the
  replace-latest relay ring before replication published it. This is the class the coverage
  hypothesis predicts, and **the only one raising the ring's retention depth would move**.
- **`AboveNewest`** — `probeTick` is newer than anything the store holds. The receiver is asking for
  a capture that has not been produced or has not landed. **Depth is irrelevant here** — no amount
  of ring redundancy delivers a capture that does not exist yet. This is the design's own deficit
  condition `D_A >= lead_B + downlink_B` failing for that particular sender/receiver pair.
- **`BelowOldest`** — `probeTick` is older than the store retains: clock misalignment, or the
  store's 64-tick capacity being outrun.
- **`NoProbeTick`** — no probe tick could be formed at all (the `tick < dA` underflow guard: a
  session younger than the delay). Kept **separate** from `BelowOldest`, which it superficially
  resembles: folding an early-session artefact into a clock-misalignment bucket would corrupt the
  exact discrimination this probe exists to provide.

**Why a second enum rather than six outcomes.** Splitting `Miss` into three enumerators would have
rewritten every existing use of `ScheduledRelayedReadOutcome::Miss` — the test suite, the stale-run
rule (`isStaleFallbackOutcome`) and both shipped call sites — to buy nothing the orthogonal field
does not. `outcome` answers *which rung of the ladder replied*, which is a property of the LADDER.
`missClass` answers *where was the receiver asking relative to what it holds*, which is a property
of the STORE. They are genuinely different questions, and the pair is strictly more informative
than a flattened six-way enum: a window can report six classes **and** still be compared against
every measurement taken before the split.

`RelayReadCounters::miss` is therefore kept as the **total**, with the four sub-counters
partitioning it and always summing to it exactly. `missClassTotal()` is exposed so a test can
assert the partition rather than trust it. The `note(const ScheduledRelayedReadReport&)` overload
is an **overload, not a replacement**, so every earlier call site and test compiles and counts
identically.

---

## 4. The signed-delta histogram

`ScheduledRelayedReadReport::deltaToNewest` is the signed distance `probeTick - newestResident`.
The three miss classes are buckets over exactly this quantity, so the distribution separates them
**continuously** rather than categorically: a window whose deltas cluster at +2 is a receiver
reading ahead of what it has been sent — no depth will help — and one whose deltas cluster at −3
with misses is a receiver reading inside a span full of holes, where depth will.

It is set on **every** outcome that formed a probe tick, `Hit` and `VerifyFail` included, because
"how far behind the newest is a HIT" is the calibration the miss deltas are read against. It costs
nothing: `newestResident` on those arms is `findLatest().captureTick`, which the ladder has already
computed.

`RelaySignedDeltaHistogram` is exact for `|delta| <= kRelayDeltaHistogramRange` with one saturating
bucket at each end, and the exact min and max are tracked alongside so **a saturated percentile is
always accompanied by a real number** — the same contract `RelayArrivalProbe`'s gap histogram
carries, for the same reason. The two saturating buckets report the first value *outside* the exact
range, which is a floor/ceiling on the truth and never a claim of precision they do not have.

**The range is the store's capacity on purpose.** Beyond ±64 capture ticks the receiver is asking
outside anything the store could ever have held, so the exact value stops carrying diagnostic
information the min/max does not already carry — it only says "far outside", which is what the
saturating bucket says.

`p10` and `p90` are reported rather than a mean because the distribution is expected to be bimodal
— hits clustered just below zero, above-newest misses clustered above it — and a mean of a bimodal
distribution names a value that never occurs.

---

## 5. PROBE 3 — the D4 stale-hold run, and why rung 0 is excluded

The run is *"consecutive ticks this character was served a fallback"*, and its purpose is to set
`K` for the deferred stale-hold rule **from data instead of a guess**.

**Only the prediction call site feeds it**, and that is deliberate: the run is only meaningful over
a MONOTONIC per-tick stream. Resim replays ticks the prediction has already run, out of order and
repeatedly, so interleaving it would produce a "consecutive run" that describes nothing. The resim
call site — `collectResimInputAll`'s `NoRef` / remote row — is counted, but it drives neither the
window nor the run, and takes no id because nothing per-id is derived from it.

**Rung 0 is excluded from the run.** Rung 0 also serves `fallback()`, but it is the
pre-registration / join window — "no data has ever arrived" — not staleness, which is "data
arrived and then stopped scheduling". Counting it would inflate every session's maximum run by the
whole length of its join window and bias `K` upward, and setting `K` from data is the entire reason
this probe exists.

It neither increments the run **nor** resets it. Once anything has been pushed into a store,
`findLatest().valid` stays true forever — slots are reclaimed by overwrite, never cleared — so
`NoProbe` can only ever be a LEADING prefix, and the two treatments are observationally identical.
Not-incrementing is the load-bearing half; not-resetting is the literal reading of "exclude rung 0
from the run" — the sample is excluded, rather than treated as a break.

**The two call sites are counted separately and never summed.** Prediction and resim run the same
ladder over the same store, so a divergence in their hit rates is a real signal about the frontier:
the resim resolves ticks the prediction already ran, so entries that were missing then may have
landed since. Summing them would average that signal away, and it is a signal nothing else in the
system reports.

**Window mechanics.** `maybeCloseWindow` returns true **only** when a window closed *and* carried at
least one read, so an authority — which allocates no relay stores and therefore never reaches
either call site — and an idle client never heartbeat a line. It is driven by the prediction tick
and only by it: that is the one monotonic per-frame clock either call site has. A hard resync can
move it backwards, which is not an error here — the window simply restarts at the new tick rather
than staying open for the ~4 billion ticks an unsigned subtraction would compute. The per-id
*current* runs deliberately survive the window boundary, because a starvation that straddles two
windows is one run, not two; only the window's maximum is reset.

---

## 6. PROBE 2 — arrival cadence, in CAPTURE ticks

**This is a correctness requirement, not a unit preference.** The rule this feeds is
`depth >= gap_p99 + margin`, and `depth` is denominated in CAPTURE ticks — a ring entry *is* a
capture tick. A gap measured in the receiver's local ticks conflates the sender's cadence with the
receiver's clock geometry (two different leads over the server, plus stalls and resyncs), so the
resulting p99 would be in the wrong units and could not legitimately be compared against `depth` at
all.

The measurement is therefore the newest `captureTick` in the ring on THIS arrival minus the newest
on the PREVIOUS arrival, per component. Same units as depth, immune to local clock skew, and free —
`populateRemoteInputCache` already walks every entry, so the newest-captureTick field was added to
the report it already returned rather than adding a second walk.

**Structurally local-tick-proof.** `noteArrival` is not given a local tick and there is no clock in
the file, so a local-tick implementation cannot be written there by accident. The test that pins
this drives arrivals whose *local* spacing differs from their *capture* spacing and asserts the
capture answer.

**A histogram, not a mean — and not a mean plus a max either.** A mean hides exactly the tail that
sets `depth`: a channel that delivers every tick except for one 20-tick stall per second has a mean
gap near 1 and a p99 near 20, and it is the 20 the depth rule must clear. Buckets are exact for
gaps `1..kRelayArrivalMaxTrackedGap` — the whole range that can matter, since the store holds only
64 capture ticks — and anything larger lands in one saturating bucket while still being reported
exactly through `maxGap`.

⚠ **The header used to write that constant as `kMaxTrackedGap`, which is not a symbol in this
tree.** §14, F-35-7.

**Four arrivals are not samples**, and each is a different thing:

1. the **first** arrival for an id — there is no previous newest to subtract, so there is no gap.
   It only seeds the watermark.
2. an arrival whose newest capture tick did **not advance**. A `dA` re-stamp of an already-resident
   tick is the realistic cause. These are reported separately as `noAdvance`, because counting them
   as a gap of 0 would drag the percentiles down and understate the depth requirement — and a
   window that is mostly these is itself worth seeing.
3. an arrival whose newest capture tick went **backwards**. The relay stream is monotonic in
   capture tick by construction — the server relays only on `parked && acceptedNew` — so this
   should not happen; if it ever does, a signed-negative "gap" would be nonsense, so it is treated
   as a no-advance and the watermark is left alone rather than being rewound.
4. an arrival whose gap exceeds the discontinuity guard — §7.

**The per-id watermark deliberately survives the window boundary.** The gap across a window edge is
a real gap, and dropping the watermark would silently discard one sample per component per window.

---

## 7. The discontinuity guard, and why the value is 16

`kRelayArrivalDiscontinuityTicks` is the same guard, for the same reason, that
`kFrameHealthDiscontinuityTicks` and `kRelayWriteDiscontinuityTicks` already carry. A capture-tick
jump larger than it is a **single correlated event** — a connection hiccup, a host stall, a
relevancy pause, a re-join, the client's game thread blocking so OnReps queue — not `gap - 1`
independently lost inputs. Charging it to the per-input loss rate mixes two distributions and
reports catastrophic failure on a working relay.

**Why it matters more here than on either sibling.** `lostCaptureTicksX1000` is the discriminating
term of the flush-on-poll acceptance gate, whose pass condition is ~11 per mille. The archived
`t39_runA` window reconstructs exactly as `116×1 + 2×2 + 20 + 47 = 120 samples`, faithful to its
recorded `p50=1 p99=20 max=47 saturated=0`; that is Σ = 187 expected, 67 lost, **358 per mille** —
about 30× the pass condition, on a window whose other **118** samples are healthy. With the guard
the same window reads **16 per mille**.

⚠ **The header used to say "a single 47-tick gap … ~355 per mille".** Both halves are slightly off:
the figure is 358, and it comes from **two** excluded outliers (20 and 47), not one. §14, F-35-4.

**The value is 16 because the archives force it, not because it is round.** Every archived arrival
window in the three reference runs was re-read; the observed gap distribution is **bimodal with a
completely empty band**:

| | observed |
|---|---|
| healthy | every window's `max=` is in 1..6, and p99 never exceeds 4 |
| **EMPTY** | no sample anywhere in **7..17**, across ~28,000 samples in three runs |
| outliers | 18, 20, 20, 22, 46, 47, and four samples ≥ 64 (max 229) |

16 sits at the top of that empty band: 2.6× the worst healthy `max` and 4× the worst healthy p99
below it, with 2 ticks of margin below the smallest outlier above it. It is also **exactly
2 × `relayedInputRing::kMaxDepth`** — one flush round can publish at most `kMaxDepth` capture ticks,
so a gap of more than two full rounds cannot be produced by the flush path at all; something
upstream stopped. And it cannot be produced by ordinary wire loss: at the measured 1.122 % loss per
capture tick, 16 consecutive losses has probability ~1×10⁻³¹.

**A suggested 15–30 range is only satisfiable at its very bottom.** A threshold of 20 or 24 would
let the 18/20/22 class through, and the archived window carrying 18 and 22 still computes to
~250 per mille (its `p99 = 18` is the row that forces the threshold). **Anything above 17 fails to
fix the defect**, so 16 is not a taste call.

**Discard semantics.** A gap above the threshold re-seeds the watermark, is counted in
`discontinuities`, and is **a sample of nothing**: it enters neither loss accumulator, neither the
histogram nor `maxGap` nor `m_samples` — and therefore it cannot close a window either, which is
deliberate. A window must close on 120 real samples. Its magnitude is preserved exactly in
`maxDiscontinuityGap`, so nothing is hidden, only re-classified; it rides the window line as
`discontMax=`, beside `discont=`. The watermark **is** advanced, so
the next arrival measures from the far side of the interruption rather than re-charging it, and
`*outGapCaptureTicks` stays 0 because its documented contract is "the gap this arrival
CONTRIBUTED", and this one contributed none.

**The operator rule**, and it is why the field exists at all: a window reporting `discont= > 0` is
**DISCARDED, not averaged in** — exactly as `RelayWriteWindowSummary::discontinuities` is.
`lostCaptureTicksX1000` is still honest for the samples it kept, but the excluded event was real
and the window no longer covers a contiguous span. Without `maxDiscontinuityGap` the guard would
silently swallow the one number that says how bad the interruption was — and before the guard
existed, `max=` was the only tell those windows had.

**`saturatedSamples` and `p99Saturated` are structurally unreachable since the guard**, and that is
deliberate rather than an oversight: the bucket sits at `kRelayArrivalMaxTrackedGap = 64` and the
guard fires at 16, so every gap big enough to saturate is re-classified before it is ever sampled.
They are **kept** because every archived window carries `saturated=`, and two poisoned windows in
one reference run were found by it — deleting the field would break comparability with exactly the
logs that motivated the guard. But an operator reading a **new** log must read `discontinuities`
instead: `saturated=` never saw the 46/47 class at all, which is precisely why it was not enough.

`m_discontinuities` and `m_maxDiscontinuityGap` are **per window**, like `FrameHealthProbe`'s: the
field's job is to tell an operator whether THIS window was interrupted, and a cumulative count
would mark every window after the first as suspect forever.

---

## 8. The R = 0 loss counter

**Mandatory, not diagnostic garnish, and the reason is structural.** With bare flush-on-poll at
R = 0 an input is sent exactly ONCE, and the replication system exposes **no send-success signal
that game code can read** — so permanent input loss is INVISIBLE on the server by construction. For
one adapter the near-miss is `FReplicationWriter::HandleDroppedRecord`, which recovers the
changemask, not the values; the header's binding block names it and says why it is not one.

**The countable end is the client.** Capture ticks are per-character monotonic at ~60/s, so every
permanently lost input is a visible ARITHMETIC GAP in the received stream, and nothing else can
produce one: a gap of `g` means `g - 1` capture ticks were produced by the sender and never
arrived here.

**The numerator is `gap - delivered`, not `gap - 1`, and the difference is the whole instrument.**
`gap - 1` measures *advance of the newest watermark minus one*, which equals the lost count only
while an arrival carries exactly ONE new capture tick — the retired replace-latest regime. Under
flush-on-poll one arrival publishes the whole staged burst, so a 2-entry burst advances the
watermark by 2 and `gap - 1` charges 1 as lost **while both entries arrived**. Measured on the
archived 2-character flush run, that read 122 / 129 per mille against a ~11 per mille pass
condition — and it was measuring the burst rate: the run's `[RelayFlush] entriesPerRoundX100` was
112–113, and `1 − 1/1.13 = 115` per mille. The instrument was reporting, as loss, precisely the
thing the flush now delivers instead of losing.

| field | definition |
|---|---|
| `lostCaptureTicks` | `Σ(gap − delivered)` over the window's accepted arrivals. **Exact, not estimated:** the watermark advanced by `gap`, `delivered` of those ticks arrived, so the remainder provably never did. It reduces to `Σ(gap − 1)` at depth 1, so every archived comparison and the replace-latest counterfactual stay meaningful. |
| `deliveredCaptureTicks` | `Σ(delivered)`, clamped per sample to `gap`. Reported so the window is **self-checking**: `lost + delivered == expected` must hold exactly, on the log line as well as in a test. Without it a reader cannot tell a window that lost nothing from a window whose delivered count was never plumbed through. |
| `expectedCaptureTicks` | `Σ(gap)` — the capture ticks the senders PRODUCED over the span this window covers. The only honest denominator: it is derived from the same samples, so it stays correct across window edges, across a varying number of remote characters, and on a client whose own frame rate has nothing to do with the senders'. |
| `lostCaptureTicksX1000` | per mille of expected. Steady-state expectation ~11, the measured 1.122 % wire loss. Sustained material excess over the server's own reported loss rate means scheduler SKIPS are happening on top of wire loss, which is the evidence-driven trigger to re-check the diet margins or put R = 1 back on the table. It must be reported for the join-settling window SEPARATELY from steady state. |

**`newCaptureTicksDelivered` is required and has no default, deliberately.** A default of 1 is
exactly the retired replace-latest premise — "one arrival carries one entry" — and silently
re-defaulting to it is how this instrument came to report ~120 per mille on a flush that lost
nothing. Making it a positional requirement turns every call site into a statement of what that
arrival delivered, and makes a pre-fix call site a compile error rather than a silently-wrong
number. At the shipped call site the count comes from
`RelayedInputIngestReport::newCaptureTicksIngested`, which the ingest already computes while
walking the ring; it counts entries whose capture tick was NOT already resident, because
re-delivery of a tick this receiver already holds is not new coverage.

**It is clamped to `gap` before it is used.** The exactness argument — "the watermark advanced by
`gap`, `delivered` of those ticks arrived, so the rest never did" — is an argument about the
half-open interval `(previousNewest, newestCaptureTick]`, which holds exactly `gap` tick slots. An
arrival that additionally back-fills a hole BELOW the previous watermark — not reachable under
R = 0's monotonic single-publish stream, but not excluded by any type — would otherwise subtract
coverage that belongs to an earlier interval, and could underflow the unsigned difference. The
clamp makes the counter unable to report negative loss as an enormous positive one.

**Why it rides the existing window rather than a new one:** it is a ratio of two quantities this
probe already computes per sample, and a second window over the same samples would be a second
clock to keep honest for no new information. The window is shared across remote ids, as it always
has been, and the ratio aggregates correctly across them because both numerator and denominator are
sums over the same sample set.

⚠ **A no-advance arrival is not a sample** and contributes to NONE of the three terms — it advanced
no capture tick, so it says nothing about loss. And a **discontinuous** arrival's
`newCaptureTicksDelivered` is discarded too: it may well carry a full burst, but the interval it
spans is not a contiguous capture stream, so neither its loss nor its coverage is a statement about
the wire. Crediting its delivered ticks while not charging its gap would let an interruption
*improve* the reported rate — an instrument that reads healthier the worse the connection gets.

---

## 9. PROBE 4 — frame health, and the role extension

**Origin, server-only.** This probe was built to measure the CAUSE of the quantity PROBE 2 measures
the EFFECT of. PROBE 2 reports `gapCaptureTicks` — how many capture ticks a client's relay ring
skipped between two arrivals. This probe reports how many sim ticks a game thread advanced between
two frames. On the server the two are predicted to be the same number, and that prediction was the
whole experiment:

- Property replication runs once per server GAME-THREAD FRAME, and **the server frame-rate setting
  is a CAP on that rate, never a floor**.
- A 60 Hz async sim on a 30 fps server therefore advances TWO sim ticks per replication, and a
  replace-latest ring at depth 1 can only carry the newer one. `G ≈ 60 / serverFrameRate` — every
  measured number in the coverage hypothesis falls out of that one relation.
- So if ticks-per-frame p50 EQUALS the measured gap p50, `G` is a HOST PERFORMANCE artefact — the
  observed sessions ran a dedicated server, two clients and the editor on one machine — and there
  is no netcode defect. If ticks-per-frame is 1 while the gap is still 2, replication is being
  skipped for some other reason and the depth hypothesis is not the explanation.

**Extended to the client.** Nothing above is server-specific: it is a hook-independent ratio over
two caller-supplied counters. The frame-health line was the ONLY wall-clock timing instrument
anywhere in this netcode surface, and it was server-only by construction even though the server
never resims and the client does. On a client the same ratio, plus its own `meanFrameMicros` / p99
/ max, is frame HEALTH under resim load — the quantity every client cost figure in the resim-gate
findings had to be DERIVED from `ResimGateProbe` cadence instead of measured, because nothing
sampled wall-clock time on that thread. The type was renamed from `ServerFrameProbe` accordingly:
nothing role-specific survives in its math, so nothing role-specific should survive in its name.

**Two routes to a ratio above 1, and they are not the same defect.** The physics engine may run
several fixed sub-steps inside one game frame (`numSteps > 1`), or the game thread may simply be
slow. Both show up as ticks-per-frame > 1 and they need different fixes, so this probe reports the
sub-step count ALONGSIDE the ratio and never collapses them. **On a resimming client that
separation is the whole point:** a resim burst folds extra sim ticks into `numSteps` at the SAME
hook, so a ratio above 1 with `numStepsAboveOne > 0` reads as "resim ran", not "the game thread
hitched" — the two client-cost questions the role extension exists to keep apart.

**Hook-independent by construction.** The caller supplies a GLOBAL frame counter, not an invocation
count, so the ratio is correct no matter which game-thread hook feeds it and no matter how often
that hook fires — the same property that let this probe move to a second role without a second
implementation. The probe additionally reports how its own invocations distributed over frames:
`dFrame == 1` (once per frame, the assumed case), `dFrame == 0` (fired more than once in a frame,
i.e. per sub-step) and `dFrame > 1` (frames it did not fire on). **That is the verification:** the
hook's cadence is measured, never assumed — on either role, from its own counters, independent of
which hook the caller chose. The whole-window aggregate is likewise cadence-independent, because
the totals are differences of the caller's own counters.

**Game thread only, on whichever role the owning object runs on.** Like the other two objects it
holds no lock and needs none — it is touched from exactly one thread. Each role gets its OWN
instance: the adapter's frame-health probe is a plain member, constructed once per actor, and both
a server-role and a client-role actor exist as separate objects whenever ONE PROCESS HOSTS BOTH
ROLES. Never shared, so there is no cross-role synchronization question either.

**Tick source is the caller's problem**, and there is only one right answer on the game thread: the
tick↔physics-frame mapper's atomic offset. The underlying clock — the server tick, or the client's
prediction tick — is written on the physics thread and must not be read here on either role. This
probe takes a plain number and therefore cannot make that mistake on the caller's behalf; the call
site carries the argument for why the same atomic read already proven safe for the server call site
is equally safe for the client one.

**The two roles emit under different log tags and categories.** The server line keeps
`[RelayProbe.Frame]` on the relay-probe category, because it measures the CAUSE of the cadence the
arrival line measures the EFFECT of and the two are only interpretable together. That reason does
not transfer to the client, where the natural pairing is frame health against RESIM COST, so the
client line rides the resim-probe category under `[ResimProbe.Frame]`. **They are deliberately
different families, not one tag with a role suffix** — a review of the resim-gate findings once
mis-assigned which process emitted the frame line and had to re-derive roles from other receipts,
and a client line that merely *looked* like the server's would repeat that. Both lines also carry
an explicit `role=` field, so a reader does not have to already know the tag convention.

A sim-tick jump larger than `kFrameHealthDiscontinuityTicks` is a DISCONTINUITY — the mapper offset
being established, a level transition, a multi-second editor stall — not a hitch, and it is counted
and reported rather than silently dropped: sampling it would put a five-digit outlier in `max` and
hide every real number behind it. Its bucket range starts at **0**, unlike the arrival histogram's,
because a frame in which physics did not step is a real observation here.

---

## 10. PROBE 5 — relay writes per server frame, and the hole it closed

The elimination chain that motivated this probe concluded *"the loss is in the replication send
path"*. Its elimination table has a hole, and it is a precise one:

| stage | evidence cited |
|---|---|
| server writes the ring | writes on every accepted receipt, and receipts are complete |
| server frame rate | 1.003 sim ticks/frame, p99 = 1 |

**Both statements are true and neither covers this case.** The ring is written from the RPC RECEIPT
path — `ServerReceptionCoordinator::receiveRemoteInput`'s relay tap → `relayRemoteInput` →
`relayedInputRing::writeLatest` — on the game thread, once per genuinely-new capture tick. (Under
flush-on-poll `writeLatest`'s only caller is `stageArrival`, which passes `kMaxDepth` as a literal;
the tap now reaches it through the stage. §11.) It is **not** written from the sim
tick. So:

- *"receipts are complete"* says no capture tick is LOST on the wire into the server. It says
  nothing about how many of them land in the SAME game-thread frame.
- *"1.003 sim ticks per frame"* measures the SIM's pacing. The relay writes are paced by PACKET
  ARRIVAL, which is a different clock with different jitter.

And the ring shipped, at the time this probe was built, at a compiled retention depth of 1 entry —
replace-latest. The replication system polls a replicated property ONCE PER SERVER GAME-THREAD
FRAME and compares the live value against its shadow copy: three writes produce one compare against
the third. Therefore:

> **If two relay writes land in one game-thread frame, the first one is unobservable.** It is not
> dropped by the network, not deferred by the replication writer, and not visible to any
> client-side probe. It is overwritten in server memory before replication ever looks at it.

That loss is INDISTINGUISHABLE, from the client, from a send-path drop: the client sees a relay
ring whose newest capture tick advanced by more than 1, and `RelayArrivalProbe` reports it as
`gapCaptureTicks > 1`. Everything the elimination chain measured is consistent with either cause.

**The arithmetic that makes it testable.** Over a window, writes arrive at the capture rate (60/s,
since client→server receipt is measured complete) and are observable at most once per WRITING
FRAME, so `observable fraction ≤ writingFrames / totalWrites = 1 / (mean writes per writing frame)`.
The clean-run baseline measured a delivered fraction of 59.3 %, i.e. a mean arrival gap of
`60 / 35.6 = 1.685` capture ticks. If write coalescing is the mechanism, the probe must report a
mean of ~1.69 writes per writing frame and a run-length histogram matching the client's gap
histogram shape for shape (p50 = 1, p99 = 3–7, max = 8). If instead it reports ~1.00, coalescing
contributes nothing and the loss really is downstream — the outcome that keeps the send-path
candidates alive. **Both outcomes were interpretable in advance, which is the point:** the
interpretation table was written before the run, not after.

**What it deliberately does not claim.** A run length of N means N writes shared a frame. It does
**not** prove the N−1 older ones would otherwise have arrived — a send-path drop can be stacked on
top of coalescing, and the two are additive. The probe reports the CEILING on observability that
the write pattern imposes; comparing that ceiling against the client's measured arrival rate is
what separates "coalescing explains all of it" from "coalescing explains part of it".

**Mechanics.** A run is the number of relay writes for ONE owner inside ONE game-thread frame, so 0
is not a possible observation and the histogram starts at 1 — the same convention
`RelayArrivalProbe`'s gap histogram uses, deliberately, so the two are read side by side without an
index shift. `kRelayWriteMaxTrackedRun = 16` is well past the measured max gap of 8. The window is
driven by **completed runs**, not by writes, so the sample count means the same thing as
`RelayArrivalProbe`'s: 120 observable events — ~3.4 s at the predicted ~35 writing frames/s, ~2 s
at a healthy 60.

**A window closes on a run boundary, never on a write.** The run that is still open cannot be
counted: its length is not known until a later frame's first write proves it ended. Closing on
writes would put a truncated run in the histogram once per window and bias `maxRun` and the mean
downward. When a new frame starts, the previous run is complete and countable, and its LAST write
is what closes the window's capture span; the incoming `captureTick` belongs to the run only now
opening, and therefore to the NEXT window — which is why the next window's span is anchored at THIS
capture tick, since anchoring it anywhere else would leave a one-write hole between consecutive
windows.

**The open run, the frame anchor and the capture-tick watermark survive a window reset.** Only the
window's own accumulators are cleared. Resetting the run or the watermark would inject a false run
boundary and a false capture gap at every window edge: one per 120 runs, which is exactly the size
of effect this probe is trying to resolve.

**Discontinuities.** A capture-tick jump larger than `kRelayWriteDiscontinuityTicks` is a
discontinuity — registration, a client re-join, the tick domain being re-established — not a
coverage hole, and it restarts the window rather than poisoning it: a `captureSpan` that no longer
matches the writes it spans makes every fraction meaningless. `discontinuities` survives the
restart so the next emitted line still says the window was interrupted, and a window reporting one
is DISCARDED rather than averaged in.

**Upstream loss is kept strictly separate.** A write whose `captureTick` is not exactly
`prevCaptureTick + 1` means the server never received that capture tick at all;
`missedCaptureTicks` counts those ticks and is the numerator behind `receivedX1000`. Capture-tick
accounting runs on EVERY write, before any frame reasoning, so it stays correct whatever the frame
pattern turns out to be.

**The four fractions are deliberately not collapsed**, because each names a different stage and a
single "loss" number would make the remedy decision on merged evidence — see §11 for
`observableX1000` and `replaceLatestObservableX1000`, whose meanings depend on which write path is
in play. The capture span `lastCaptureTick − firstCaptureTick + 1` is the only denominator that
makes the server's numbers directly comparable to the client's arrival rate, which is itself
measured against 60 captures/s.

---

## 11. The answer: coalescing, and what flush-on-poll changed

**The measurement came back "coalescing".** The write path lost ~11.6 % of relayed inputs to
same-frame coalescing — an observability of ~884 per mille. The fix replaced replace-latest with
**bare flush-on-poll**: arrivals are staged and the whole stage is published once per replication
poll (`relayedInputRing::stageArrival` / `flushStagedInto`), so a second write in a frame no longer
overwrites the first — it is published beside it.

⇒ **Read everything in §10 as the QUESTION this probe was built to settle, not as a description of
the shipped write path.** The paragraph beginning *"If two relay writes land in one game-thread
frame"* is HISTORICAL: true of the path this probe measured, false of the path that ships.

**The probe survives because the question did not go away, it shrank.** A burst LONGER than the
stage capacity still loses its oldest entries, and that ceiling is what `observableX1000` now
reports. The probe is also the flush fix's own acceptance instrument: its pass condition is
`observableX1000 >= 990`. **It is no longer disposable** in the sense the original "why a separate
file" note intended.

**`observableX1000`'s definition is unchanged; its arithmetic moved with the mechanism.** Under
replace-latest a writing frame published exactly ONE entry, so the ceiling was `runs / writes`
(~884 measured). Under flush-on-poll the whole staged burst is published, so the ceiling is
`Σ min(runLength, stageCapacity) / writes` — only a burst LONGER than the stage loses anything,
which at a stage capacity of 8 against a measured max run of 8 is ~1000. **Leaving the old
arithmetic in place would have made the acceptance gate unpassable by construction while the
mechanism worked perfectly.**

`replaceLatestObservableX1000` is `runs * 1000 / writes` — the SAME ceiling computed the way the
replace-latest path imposed it. It is kept **only** so archived windows stay directly comparable
and so the improvement flush-on-poll bought is visible as two numbers on one line rather than as a
claim. **It measures nothing live: no code path has that ceiling any more.**

`observableWrites` is the raw numerator behind `observableX1000`, reported so a window's ceiling can
be re-derived rather than trusted, and so a burst that genuinely overflowed the stage is visible as
`writes - observableWrites` rather than only as a rounded per-mille.

**`RelayStageCapacity` is a named type rather than a second `uint32`, deliberately.** The stage
capacity is `relayedInputRing::kMaxDepth` under flush-on-poll and 1 under the retired
replace-latest path, and it is **injected rather than included**, so the header stays STL-only and
a test can drive both regimes without a build flag. The probe's other constructor argument is also
a plain count, and `RelayWriteProbe probe(4u)` silently meaning something new is precisely the class
of defect this initiative keeps paying for; with the wrapper, every pre-flush call site is a COMPILE
error that has to be read, not a runtime surprise. `recordRun` is no longer static for the same
reason: the observable numerator needs the per-probe stage capacity.

---

## 12. PROBE 6 — the per-connection send budget

**What it settles, and why the arithmetic alone was not enough.** The budget model is: the server's
per-tick send allowance is the NEGOTIATED RATE divided by the desired tick rate, in bytes, and a
connection may bank at most two ticks of unused allowance. Both halves are read from engine source
— for one adapter, `UNetConnection::Tick`'s
`DeltaBits = CurrentNetSpeed * clamp(DeltaTime, 0, 1/DesiredTickRate) * 8`, with `QueuedBits`
floored at `-2 * DeltaBits`. At a negotiated **250,000 bytes per second** and 60 Hz that is
4,166.67 B/tick with 8,333 B of bankable credit. Against a modelled 1,413.75 B/round for three
characters that is ~34 % occupancy — nowhere near saturation.

⚠ **The header used to say "250000 bit/s".** It is bytes: the shipped configuration's own comment
records `MaxClientRate` as *"server→client send ceiling, in BYTES PER SECOND"*, and the paragraph's
own `* 8` only makes sense in bytes. §14, F-35-3.

**Every number in that paragraph is derived, not observed.** The negotiated rate is what the SERVER
clamped the client's request to, at runtime, on a rate-negotiation path nobody on this initiative
has watched execute; and the modelled payload counts two properties out of an unknown total. This
probe replaces every derived term with a measured one:

- **`netSpeedBps`** — the connection's ACTUAL negotiated rate. If this is not 250,000 the whole
  34 % reading is wrong and the protocol's prediction inverts.
- **`outBytes` / `outPackets` deltas** — the REAL per-tick payload: all properties, all framing, all
  headers, at connection resolution. The two properties measured elsewhere are a known
  401 B/char/round, so the residual is everything else.
- **`queuedBits` min/max and `notReadySamples`** — the saturation state itself. Send debt plus
  buffered bytes at or below zero IS the connection's READINESS test, and the writing channel
  returns WITHOUT WRITING ANYTHING when it fails. A non-zero `notReadySamples` is the send-path
  deferral candidate's smoking gun; a zero one with `queuedBits` pinned near its floor is that
  candidate's refutation, **measured rather than argued**.
- **`outPacketsLost` delta** — ack-derived, so it is the emulation's REAL outgoing loss rate rather
  than the configured `PktLoss` percentage. That is the wire-loss candidate's evidence without changing a
  single config value.

**The send-debt counter is negative when there is headroom** — the per-tick allowance pays it down
— so `queuedBitsMin` is the MOST headroom seen and `queuedBitsMax` is the CLOSEST to saturation,
not the other way round. `notReadySamples` counts samples at or above zero: the state in which the
replication system writes nothing at all this frame.

⚠ **The struct field is `notReadySamples`; the log line spells it `notReadyFrames=`.** Both names
are live and neither grep dead-ends, but they are not the same token — the header's prose used to
name only the log spelling while describing the field. §14, F-35-8.

**It takes plain numbers**, exactly like `FrameHealthProbe`, so it is engine-free and unit-testable;
reading the transport connection's fields is the CALLER's job, in the adapter. The three
`outTotal*` arguments are the connection's **session-cumulative** byte / packet / loss counters,
never its periodic stat accumulators — the engine resets those on its own schedule, so differencing
them across our window would drop whatever it zeroed mid-window. `tickRateHz` is passed in rather
than assumed so that a run on a differently-configured server still reports a correct allowance. A
counter that went backwards means the connection was replaced under the same id (reconnect), so the
probe re-anchors rather than reporting a negative delta as a gigantic unsigned one.

---

## 13. Conventions shared by all five probes

- **Instrumentation only.** Counters plus a windowed summary; every code path that consults them is
  a log call. Removing either header changes no simulated value.
- **Neither object logs.** They accumulate and hand back a summary struct; the caller owns the
  logger and does the `SIMLOG`. That is the same convention `populateRemoteInputCache` and
  `RemoteMoveQueue` already follow — core containers do not log — and it is what lets the
  Low-Level-Tests assert on the numbers rather than on strings.
- **Engine-agnostic, STL only.** No UE types, no `OGTypes`, no other `OGSimulation` header, so the
  whole of both files is testable from `og-simulation-tests` without a simulatable, an owner-traits
  specialization or a logger. The engine names that remain are confined to one binding block per
  file, marked as ONE adapter's binding.
- **Separate objects, separate windows.** A single shared window would have one thread reset
  counters the other thread is mid-increment on, corrupting a whole window's totals — strictly
  worse than the torn-SLOT debt `Network/RemoteInputCache.h` documents and accepts, because a torn slot
  costs one input on one proxy for one tick while a torn window reset costs the entire measurement.
  Neither object is touched by the other's thread, so no atomics and no seam are needed; that is
  the whole reason for the split.
- **NEAREST-RANK percentiles everywhere.** `RelayArrivalProbe::percentile`,
  `RelayWriteProbe::percentile`, `RelaySignedDeltaHistogram::percentile` and
  `FrameHealthProbe::percentile` all use the same definition, so a p99 in one summary and a p99 in
  another are comparable without a footnote. Nearest-rank is chosen over interpolation because the
  samples are integer tick counts and an interpolated 3.7-tick p99 would have to be rounded up to 4
  before it could be compared against `depth` anyway. `FrameHealthProbe`'s bucket 0 is the one
  exception to the index convention, and it is a real observation rather than an unused slot.
- **`forgetOwner` on every id-keyed object.** `RelayReadProbe`, `RelayArrivalProbe` and
  `RelayWriteProbe` each expose one, called from the same unregister contract that reaps the
  reception coordinator's claim map, so the maps stay bounded by live ids rather than by every
  character the session has ever had. `RelayArrivalProbe::trackedOwnerCount()` mirrors
  `RelayReadProbe::trackedOwnerCount()` for the same reason: a direct, no-window-required proof that
  `forgetOwner` actually shrinks the map, for a leak-freedom test that does not want to depend on
  `noteArrival`'s first-vs-subsequent-arrival semantics as an indirect signal.
- **`peekSummary` on the windowed objects**, so a test can assert a distribution without having to
  fill a 120-sample window.
- **Window cadence.** `kRelayReadProbeWindowTicks` is ~2 s at 60 Hz, matching the cadence
  `ServerReceptionCoordinator::maybeEmitInputStats` already emits `[InputStats]` on, so an operator
  reading a log sees the two channels tick at a comparable rate. It is deliberately NOT derived from
  `TimeConfig`: none is held on that path, and threading one in for a log cadence would be a real
  dependency bought for a cosmetic gain.
- **Global namespace**, matching the rest of the OGSim core.

---

## 14. R0 corrections — five claims verified against the tree, and what they said

Rule R0 says: do not compress a claim you have not checked. Task 35 checked the load-bearing claims
in both headers before compressing them. Five were wrong. **Each was corrected in the header and
compressed in its corrected form; the false original is preserved verbatim in §16.**

### F-35-1 — the two collect paths had the wrong owner, twice

`RelayReadProbe.h`'s thread table said the probe is *"Fed from `SimulationNetSync::collectInputAll`
… and `::collectResimInputAll`"*.

**Both are members of `SimulationInputResolution`**, and have been since the input-resolution
extraction. `SimulationNetSync.h` says so in terms — its own ownership table lists both methods
against `SimulationInputResolution`, and a fence in that file records that both *"LEFT THIS
CONCEPT"*. The declarations are in `SimulationInputResolution.h`.

**And the feeder is one layer further out than the header claimed.** Neither collect method calls
the probe directly: `InputResolutionTelemetry::emitPredictionInputRead` calls
`notePredictionRead`, and `InputResolutionTelemetry::emitResimScheduledRead` calls `noteResimRead`.
The corrected table is in §1 and in the header's orientation block.

⭐ **Neither the coverage check nor the subject gate nor the anchor lint could have caught this**,
because `SimulationNetSync`, `collectInputAll` and `collectResimInputAll` all still exist. They
verify existence, never ownership.

### F-35-2 — the binding block named a symbol that no longer exists

The engine-name binding block said the adapter's replicated-ring OnRep is
`USimmableUpdateComponent::OnRep_RelayedInputRing`.

**That OnRep moved to the relay actor with the property it notifies.** The component's own source
records the move — *"`OnRep_RelayedInputRing()` stood here. The OnRep moved to
`ASimulationInputRelay` with the property it notifies"* — and the component header records the
property, the OnRep and the registration all moving together. The live symbol is
`ASimulationInputRelay::OnRep_RelayedInputRing`. The binding block now names it.

⚠ **This symbol was moved INTO the binding block by the engine-name pass, from the body**, which is
worth recording as a general hazard: **a relocation pass can promote a dead symbol into the very
block that exists to make symbols findable**, and nothing in that pass's checks looks at whether
the symbol is still real.

### F-35-3 — a unit word that the paragraph's own arithmetic contradicts

The send-budget block said *"At a negotiated 250000 bit/s and 60 Hz that is 4,166.67 B/tick"*.

**It is 250,000 BYTES per second.** The shipped configuration's own comment states that
`MaxClientRate` is *"server→client send ceiling, in BYTES PER SECOND per client"*, and the
configured value is 250000. The paragraph's own formula multiplies by 8 to reach bits, which only
makes sense if the rate is in bytes — and 250,000 bit/s ÷ 60 is 4,167 **bits**, not the 4,166.67
bytes the next sentence uses. **Every number downstream was right; only the unit word was wrong**,
which is the hardest kind of error to see.

### F-35-4 — a figure attributed to one outlier when the archive records two

The discontinuity-guard block said *"A single 47-tick gap in a 120-sample window computes to ~355
per mille … on a window whose other 118 samples are healthy."*

The archived window reconstructs as `116×1 + 2×2 + 20 + 47 = 120 samples`, faithful to its recorded
`p50=1 p99=20 max=47 saturated=0`. That is Σ = 187, lost = 67, **358 per mille from TWO excluded
outliers**. *"The other 118 samples are healthy"* is exactly right; *"a single 47-tick gap"* names
only the larger of two. Corrected to 358 and to "two outliers" in both the header and §7.

### F-35-5 — a field attributed to the wrong struct

*"exactly as `RelayWriteProbe::discontinuities` is"* — the field an operator discards a window on is
`RelayWriteWindowSummary::discontinuities`. `RelayWriteProbe` holds only the private per-owner copy
in `OwnerState`. A reader grepping the named symbol lands on a private member, not the summary field
the sentence is about. Corrected.

### F-35-7 — a constant named in prose that exists nowhere

`RelayReadProbe.h`'s PROBE 2 banner said *"Buckets are exact for gaps 1..`kMaxTrackedGap`"*.

**`kMaxTrackedGap` is not a symbol in this tree.** A repo-wide grep returns exactly one hit: the
comment that names it. The real constant is `kRelayArrivalMaxTrackedGap`, and its two siblings are
`kRelayWriteMaxTrackedRun` and `kFrameHealthMaxTrackedTicks` — a family whose members all carry
their probe's prefix, which is presumably how the prefix got dropped in prose. **A reader grepping
the named handle lands on the comment that named it and nowhere else.** Corrected in the header and
in §6.

⭐ **The (b1) subject gate found this, and nothing else could have.** The seal records every grep
handle the pre-compression text offered; the gate then asks whether each one survives. A dead handle
survives compression perfectly well — it is the *seal* comparison against the tree that exposed it.

### F-35-8 — a field described under its log spelling, not its own name

The send-budget block wrote *"`queuedBits` min/max and `notReadyFrames`"* while describing the
struct. The field is `notReadySamples`; `notReadyFrames=` is the token the emitted log line uses.
Both are live and both resolve, so this is a precision defect rather than a falsehood — but a reader
grepping `notReadyFrames` in the engine-free core finds no field, and one grepping `notReadySamples`
in a log finds no line. **Both names are now stated, in §12.**

### A ninth, smaller: a heading that counted three where the block declares four

The write-path summary's banner read *"THREE FRACTIONS, PARTS PER THOUSAND"* above a list of
**four** — `receivedX1000`, `observableX1000`, `replaceLatestObservableX1000`, `deliverableX1000` —
and four declared fields. `replaceLatestObservableX1000` was added by the flush-on-poll rework and
the count in the heading was not updated. Now "FOUR FRACTIONS".

---

## 15. Provenance — the workspace citations these headers used to carry

Standalone truth: a header may contain only what is TRUE and MEANINGFUL to a reader with no
initiative workspace, no game engine and no other file open. The task numbers below resolved to
nothing for such a reader, so they were removed from the headers as part of compression. **A doc
can carry them, and this section is where they live.**

| citation | what it names |
|---|---|
| `T19` | the task that built `RelayReadProbe` / `RelayArrivalProbe` — probes 1, 2 and 3, and the four-outcome enum |
| `T20` | the task that added PROBE B (the miss classes), the signed-delta histogram, and PROBE 4 as `ServerFrameProbe` |
| `T22` | the task that built `RelayWritePathProbe.h` — probes 5 and 6 |
| `T34` / `item 34` | the flush-on-poll rework: staging, the R = 0 loss counter, `RelayStageCapacity`, and the `observableX1000 >= 990` acceptance gate |
| `T34 rework` | the discontinuity guard at 16, and the loss-counter numerator fix |
| `T38` | the analysis establishing that the replication system exposes no readable send-success signal |
| `T39` | the task that moved the relay ring and its OnRep to a dedicated relay actor (see F-35-2) |
| `T43` | the resim-gate live findings whose client cost figures had to be derived rather than measured |
| `T49` / `item 49` | the role extension of `ServerFrameProbe` to `FrameHealthProbe`, and the two-tag ruling |
| `item 63` / `RN-13` | the retirement of the session-configurable relay depth field |
| `item 91 part C` | the leak-freedom test that motivated `RelayArrivalProbe::trackedOwnerCount()` |
| `RelayDepthCoverageHypothesis.md` §9.11 / §9.11a | the clean-run baseline and the elimination chain of §10 |
| `ReviewNotes.md` | the review record that carries the depth-field retirement (`RN-13`) |
| `impl/pie_script_t22.md` | the interpretation table for PROBE 5, written before the run rather than after |
| `finding_task43_resim_gate_live.md` | the resim-gate live findings whose client cost figures §9 refers to |
| the archived runs | `runs/t39_runA_3char`, `runs/t39_runB_2char` and `runs/t33_depth1_control` — the three reference arrival runs behind §7 (runA is the 358-per-mille window; runB is the `max=188/229` pair) — and `runs/t34_run1_2char_2026-08-09_1938`, the 2-character flush run behind §8 |

⚠ **These are workspace artifacts, outside both git trees.** They are recorded here as provenance,
not as anchors a lint can check. Nothing in either header depends on them any more.

---

## 16. The verbatim pre-compression archive

Everything below is the comment text as it stood in the working tree immediately
before task 35 compressed it, **emitted from those files by `impl/task35/gen_doc_35.py`
rather than retyped**, so this section cannot have quietly edited what it quotes.

⛔ **Do not repair a quotation here.** Several passages were verified FALSE and are
quoted anyway — that is what an archive is for. The corrected statement lives in the
header and in §14, and nowhere else.

⛔ **Blocks are labelled by the DECLARATION they were attached to, never by a line
number.** A line-number label in an archive is a join to a file that has since moved.


### 16.1 `Network/RelayReadProbe.h` — as it stood before compression

**attached to** `#include <array>`

```
// SPDX-License-Identifier: MPL-2.0
```

**attached to** `// ---------------------------------------------------------------------------`

```
// ---------------------------------------------------------------------------
// RelayReadProbe / RelayArrivalProbe — the CLIENT-side relay telemetry.
// (og-netcode-v2-input-relay T19; unblocks T9 scenarios 4, 5 and 6.)
//
// WHAT THESE ARE FOR. Two questions T9 asks that the shipped code could not
// answer, because neither emitted anything at all:
//
//   * "is the SCHEDULE being CONSUMED?" — `resolveScheduledRelayedInput` had zero
//     log calls, so a proxy that missed on every single probe and a proxy that hit
//     on every single probe produced byte-identical logs. `[CollectInput] …
//     source=RemoteInputCache hasStore=1` reports that a store EXISTS, which is a
//     different claim and is present either way.
//   * "how often does the relay ring actually ARRIVE?" — the OnRep was unlogged, so
//     T9's `depth >= gap_p99 + margin` rule had no gap distribution to be derived
//     from and could only ever have been guessed.
//
// INSTRUMENTATION ONLY. Nothing here feeds back into resolution: the probes are
// counters plus a windowed summary, and every code path that consults them is a
// log call. Removing this header would change no simulated value.
//
// ---------------------------------------------------------------------------
// TWO OBJECTS BECAUSE THERE ARE TWO THREADS. THIS IS THE HEADER STATEMENT THE
// TASK ASKS FOR (T19 review F3), and it is a correctness property, not tidiness.
//
//   RelayReadProbe     PHYSICS thread. Fed from SimulationNetSync::collectInputAll
//                      (T7's prediction proxy branch) and ::collectResimInputAll
//                      (T6's resim frontier row). Window driven by the PREDICTION
//                      tick.
//   RelayArrivalProbe  GAME thread. Fed from the relay-ring arrival callback bound
//                      in registerPredictionOwner (the adapter's replicated-ring
//                      OnRep -> populateRemoteInputCache).
//                      Window driven by SAMPLE COUNT — the game thread has no
//                      simulation tick to hand, and inventing one would be a
//                      second clock to keep honest.
//
//   FrameHealthProbe   [T20; renamed T49] GAME thread, on EITHER role. Fed from the
//                      game-thread hook that precedes each physics frame. Window
//                      driven by SAMPLE COUNT. Originally SERVER-only (named
//                      `ServerFrameProbe`) because only the server had a call site
//                      that fed it; T49 gives the client a call site too, since the
//                      math here was always role-neutral (see its own banner at the
//                      bottom of this file). The two roles emit under DIFFERENT log
//                      tags and categories — see the call site in
//                      SimulationManagerUImpl.cpp — because a line that cannot be
//                      told apart from the other role's by grep alone has already
//                      cost this initiative one mis-attributed analysis (item 49).
//
// They are SEPARATE OBJECTS with SEPARATE WINDOWS. A single shared window would
// have one thread reset counters the other thread is mid-increment on, corrupting
// a whole window's totals — strictly worse than the torn-SLOT debt
// RemoteInputCache.h documents and accepts, because a torn slot costs one input
// on one proxy for one tick while a torn window reset costs the entire
// measurement. Neither object is touched by the other's thread, so no atomics and
// no seam are needed; that is the whole reason for the split.
//
// NEITHER OBJECT LOGS. They accumulate and hand back a summary struct; the caller
// owns the logger and does the SIMLOG. Same convention `populateRemoteInputCache`
// and `RemoteMoveQueue` already follow (core containers do not log), and it is
// what lets the Low-Level-Tests assert on the numbers rather than on strings.
//
// ENGINE-AGNOSTIC. STL only — no UE types, no OGTypes, no other OGSimulation
// header. That is deliberate: it makes the whole of this file testable from
// og-simulation-tests without a simulatable, an owner-traits specialization or a
// logger.
//
// ONE ADAPTER'S BINDING FOR EVERY ENGINE NAME BELOW. The body names ROLES ONLY —
// the physics engine, the replication system, the relay-ring arrival callback, the
// server frame-rate cap, the global game-thread frame counter. For one adapter
// (Unreal/Chaos) those are Chaos, Iris, the component OnRep
// `USimmableUpdateComponent::OnRep_RelayedInputRing`, the `NetServerMaxTickRate`
// setting and `GFrameCounter`; in that one adapter the missing send-success signal
// is `FReplicationWriter::HandleDroppedRecord`, which recovers the changemask and
// not the values. Another adapter substitutes its own; nothing here depends on them.
//
// NAMESPACE NOTE: global namespace, matching the rest of the OGSim core.
// ---------------------------------------------------------------------------
```

**attached to** `enum class ScheduledRelayedReadOutcome : std::uint8_t`

```
// ---------------------------------------------------------------------------
// PROBE 1 — the four outcomes of the scheduled read.
//
// FOUR, NOT TWO, and the collapse to hit/miss is the mistake this enum exists to
// prevent (T19 description). `resolveScheduledRelayedInput`'s three-rung ladder
// serves `fallback()` from THREE different situations that mean completely
// different things while behaving identically:
//
//   NoProbe     rung 0 — `!findLatest().valid`, nothing has EVER arrived for this
//               character. The pre-registration / join window. NOT starvation:
//               there is no data yet because the channel has not started, and
//               counting it as one would make the join window look like a fault
//               on every single join.
//   Hit         the probe at `tick - dLatest` found a candidate AND that
//               candidate's own stamp equals `dLatest`. The proxy is consuming
//               the server's actual schedule — the whole claim of the scheduled
//               regime, and what T9 scenario 4 exists to confirm at floor > 0.
//   Miss        the probe found NOTHING resident at the scheduled capture tick.
//               STARVATION: the entry we should be running on has not arrived (or
//               has been evicted). Also covers the `tick < dA` early-session case,
//               where the probe tick would underflow — see the classification note
//               on `resolveScheduledRelayedInput`.
//   VerifyFail  a candidate WAS resident but its stamp differs from `dLatest`: the
//               delay REGIME shifted under the reader. TRANSITION, not starvation.
//               The data is arriving fine; the schedule it was stamped against is
//               no longer the current one, so replaying it would reproduce a
//               schedule the authority is not using.
//
// Miss and VerifyFail are the pair that must never be merged. A window that is all
// Miss says the wire is starving the proxy; a window that is all VerifyFail says
// the wire is healthy and the delay is thrashing. Same fallback behaviour, opposite
// diagnosis, and distinguishing them is precisely what T9 scenario 4 needs in order
// to tell "the schedule is working" from "the schedule is thrashing".
// ---------------------------------------------------------------------------
```

**attached to** `enum class ScheduledRelayedReadMissClass : std::uint8_t`

```
// ---------------------------------------------------------------------------
// [T20] PROBE B — WHY the miss happened. THE ENUM ABOVE IS DELIBERATELY UNCHANGED.
//
// T19's four outcomes record THAT a scheduled read missed. They cannot say why, and
// that is precisely the gap that left one measured client at 0.8% hit-rate
// unexplained while the coverage model predicted ~50% (RelayDepthCoverageHypothesis
// §3, architect response §8.4). The three whys have three DIFFERENT remedies, and
// picking the wrong one is the failure mode this enum exists to prevent:
//
//   InSpan       probeTick lies between the store's oldest and newest resident
//                capture ticks and is ABSENT. A COVERAGE HOLE: the sender produced
//                that tick and it was clobbered in the replace-latest relay ring
//                before replication published it. THIS is the class the hypothesis
//                predicts, and the only one raising the ring's retention depth
//                would move (item 63 / RN-13, 2026-08-16: that was a session-
//                configurable knob; it is retired — see RN-13, ReviewNotes.md).
//   AboveNewest  probeTick is NEWER than anything the store holds. The receiver is
//                asking for a capture that has not been produced or has not landed.
//                DEPTH IS IRRELEVANT here — no amount of ring redundancy delivers a
//                capture that does not exist yet. This is the design's own deficit
//                condition (D_A >= lead_B + downlink_B, spectrum §3.2) failing for
//                that particular sender/receiver pair.
//   BelowOldest  probeTick is OLDER than the store retains. Clock misalignment, or
//                the store's 64-tick capacity being outrun.
//   NoProbeTick  no probe tick could be formed at all (the `tick < dA` underflow
//                guard: a session younger than the delay). Kept SEPARATE rather than
//                folded into BelowOldest, which it superficially resembles — folding
//                an early-session artefact into a clock-misalignment bucket would
//                corrupt the exact discrimination this probe exists to provide.
//
// WHY A SECOND ENUM RATHER THAN SIX OUTCOMES. Splitting `Miss` into three
// enumerators would have rewritten every existing use of
// `ScheduledRelayedReadOutcome::Miss` — the T19 test suite, the stale-run rule
// (`isStaleFallbackOutcome`), and the two shipped call sites — to buy nothing the
// orthogonal field does not. `outcome` answers "which rung of the ladder answered",
// which is a property of the LADDER; `missClass` answers "where was the receiver
// asking relative to what it has", which is a property of the STORE. They are
// genuinely different questions, and the pair is strictly more informative than a
// flattened six-way enum: a window can report six classes AND still be compared
// against every T19-era measurement.
// ---------------------------------------------------------------------------
```

**attached to** `inline bool isStaleFallbackOutcome(ScheduledRelayedReadOutcome outcome)`

```
// True iff this outcome served `fallback()` from a store that HAS data — i.e. the
// D4 stale-hold situation probe 3 measures. Deliberately excludes NoProbe (see
// RelayReadProbe::notePredictionRead) and, obviously, Hit.
```

**attached to** `struct ScheduledRelayedReadReport`

```
// What `resolveScheduledRelayedInput` reports back to its CALLER, so the caller can
// count and log without the ladder itself gaining any state. Every field except
// `outcome` is diagnostic detail for the per-event Verbose line; `probeTick` and
// `candidateDA` are meaningless on the outcomes that never computed them, and the
// comments say which.
```

**attached to** `std::uint32_t probeTick = 0u`

```
// The capture tick the ladder probed, i.e. `tick - dLatest`. Meaningless on
// NoProbe (no probe was formed) and on the tick < dA underflow guard.
```

**attached to** `std::uint8_t dLatest = 0u`

```
// The stamp the verify step compared AGAINST — `findLatest().dA`. Meaningless
// on NoProbe.
```

**attached to** `std::uint8_t candidateDA = 0u`

```
// The resident candidate's OWN stamp. Meaningful on Hit and VerifyFail only;
// on VerifyFail this is the value that differed from `dLatest`, which is the
// single most useful number in a delay-transition trace.
```

**attached to** `// Why the miss happened. `NotAMiss` on every non-Miss outcome.`

```
// --- [T20] PROBE B ------------------------------------------------------
```

**attached to** `ScheduledRelayedReadMissClass missClass = ScheduledRelayedReadMissClass::NotAMiss`

```
// Why the miss happened. `NotAMiss` on every non-Miss outcome.
```

**attached to** `bool         deltaToNewestValid = false`

```
// THE SIGNED DISTANCE `probeTick - newestResident`, and at depth 1 this is the
// richer half of Probe B. The three miss classes are buckets over exactly this
// quantity, so the distribution separates them CONTINUOUSLY: a window whose
// deltas cluster at +2 is a receiver reading ahead of what it has been sent (no
// depth will help), and one whose deltas cluster at -3 with misses is a receiver
// reading inside a span full of holes (depth will).
//
// Set on every outcome that formed a probe tick — Hit and VerifyFail included,
// because "how far behind the newest is a HIT" is the calibration the miss
// deltas are read against. Costs nothing: `newestResident` on those arms is
// `findLatest().captureTick`, which the ladder has already computed.
```

**attached to** `bool          spanValid      = false`

```
// The store's resident span at the moment of the read. Filled ONLY on a miss
// (the classification needs it and nothing else does), so `spanValid` is false
// on Hit / VerifyFail / NoProbe even though a span exists there too.
```

**attached to** `inline constexpr std::int32_t kRelayDeltaHistogramRange = 64`

```
// ---------------------------------------------------------------------------
// [T20] The signed-delta histogram — `probeTick - newestResident`, per call site.
//
// Exact for |delta| <= kRelayDeltaHistogramRange, with one saturating bucket at each
// end; the exact min and max are tracked alongside, so a saturated percentile is
// always accompanied by a real number (the same contract RelayArrivalProbe's gap
// histogram uses, and for the same reason).
//
// THE RANGE IS THE STORE'S CAPACITY ON PURPOSE. Beyond +/-64 capture ticks the
// receiver is asking outside anything the store could ever have held, so the exact
// value stops carrying diagnostic information that the min/max does not already
// carry — it only says "far outside", which is what the saturating bucket says.
// ---------------------------------------------------------------------------
```

**attached to** `std::int32_t p10 = 0`

```
// Nearest-rank percentiles of the SIGNED delta. p10 and p90 are reported rather
// than a mean because the distribution is expected to be bimodal (hits clustered
// just below zero, above-newest misses clustered above it) and a mean of a
// bimodal distribution names a value that never occurs.
```

**attached to** `std::int32_t minDelta = 0`

```
// EXACT, never saturated.
```

**attached to** `std::uint32_t saturatedLow  = 0u`

```
// Samples that fell outside the exactly-tracked range in each direction.
```

**attached to** `static constexpr std::size_t kBucketCount =`

```
// Bucket 0 absorbs everything below -range; bucket kBucketCount-1 everything
// above +range; the middle 2*range+1 buckets are exact.
```

**attached to** `static std::int32_t valueForBucket(std::size_t bucket)`

```
// The delta a bucket index represents. The two saturating buckets report the
// first value OUTSIDE the exact range, which is a floor/ceiling on the truth and
// never a claim of precision it does not have.
```

**attached to** `std::int32_t percentile(std::uint32_t numeratorPercent) const`

```
// NEAREST-RANK over the buckets in ASCENDING delta order — the same definition
// RelayArrivalProbe::percentile uses.
```

**attached to** `struct RelayReadCounters`

```
// Per-window outcome tallies for ONE call site.
```

**attached to** `std::uint32_t miss       = 0u`

```
// [T20] The MISS TOTAL, unchanged in meaning. The four sub-counters below
// partition it and always sum to it exactly; `miss` is kept as the total rather
// than being replaced so every T19-era number stays directly comparable.
```

**attached to** `std::uint32_t missInSpan      = 0u`

```
// [T20] The miss partition — see ScheduledRelayedReadMissClass.
```

**attached to** `RelaySignedDeltaHistogram delta`

```
// [T20] `probeTick - newestResident` over every read that formed a probe tick.
```

**attached to** `std::uint32_t missClassTotal() const`

```
// Every miss is classified, so this must equal `miss`. Exposed so a test can
// assert the partition rather than trusting it.
```

**attached to** `void note(const ScheduledRelayedReadReport& report)`

```
// [T20] The full tally. Kept as an overload of the outcome-only form above so
// every T19 call site and test compiles and counts identically — the added
// fields are pure refinement, never a reinterpretation of the four totals.
```

**attached to** `RelayReadCounters prediction`

```
// THE TWO CALL SITES ARE COUNTED SEPARATELY, NEVER SUMMED (T19 description).
// Prediction and resim run the SAME ladder over the SAME store, so a divergence
// in their hit rates is a real signal about the frontier — the resim resolves
// ticks the prediction already ran, so entries that were missing then may have
// landed since. Summing them would average that signal away, and it is a signal
// nothing else in the system reports.
```

**attached to** `std::uint32_t maxConsecutiveFallbackRun = 0u`

```
// PROBE 3 — the D4 stale window. Longest consecutive run of fallback-serving
// reads on ONE character within this window, and which character owned it.
// This is what sets `K` for the deferred stale-hold rule (T5 / spectrum §8.7)
// from data instead of a guess.
```

**attached to** `inline constexpr std::uint32_t kRelayReadProbeWindowTicks = 120u`

```
// ~2 s at 60 Hz, matching the cadence ServerReceptionCoordinator::maybeEmitInputStats
// already emits `[InputStats]` on. Deliberately the same feel so an operator reading
// a PIE log sees the two channels tick at a comparable rate; it is NOT derived from
// TimeConfig because SimulationNetSync holds no TimeConfig, and threading one in for
// a log cadence would be a real dependency bought for a cosmetic gain.
```

**attached to** `class RelayReadProbe`

```
// ---------------------------------------------------------------------------
// RelayReadProbe — PHYSICS THREAD ONLY. Probes 1 and 3.
// ---------------------------------------------------------------------------
```

**attached to** `void notePredictionRead(unsigned int id, const ScheduledRelayedReadReport& report)`

```
// A scheduled read on the PREDICTION path (collectInputAll's proxy branch).
//
// THIS IS THE ONLY CALL SITE THAT FEEDS THE STALE RUN, and that is deliberate:
// the run is "consecutive ticks this character was served a fallback", which is
// only meaningful over a MONOTONIC per-tick stream. Resim replays ticks the
// prediction has already run, out of order and repeatedly, so interleaving it
// would produce a "consecutive run" that describes nothing.
//
// RUNG 0 IS EXCLUDED FROM THE RUN (T19 review F5). Rung 0 also serves
// `fallback()`, but it is the pre-registration / join window — "no data has
// ever arrived" — not staleness, which is "data arrived and then stopped
// scheduling". Counting it would inflate every session's maximum run by the
// whole length of its join window and bias `K` upward, and setting `K` from
// data is the entire reason this probe exists.
//
// It neither increments the run NOR resets it. Once anything has been pushed
// into a store, `findLatest().valid` stays true forever (slots are reclaimed by
// overwrite, never cleared), so NoProbe can only ever be a LEADING prefix and
// the two treatments are observationally identical. Not-incrementing is the
// load-bearing half; not-resetting is the literal reading of "exclude rung-0
// from the run" — the sample is excluded, rather than being treated as a break.
// [T20] THE SHIPPED OVERLOAD — takes the whole report, so the miss class and the
// signed delta are tallied alongside the outcome. The outcome-only overload
// below is kept because it is what the T19 tests drive and because a caller that
// has only an outcome must not be forced to fabricate a report.
```

**attached to** `void noteResimRead(ScheduledRelayedReadOutcome outcome)`

```
// A scheduled read on the RESIM path (collectResimInputAll's NoRef/remote row).
// Counted, but it drives neither the window nor the stale run — see above and
// maybeCloseWindow. Takes no id because nothing per-id is derived from it.
```

**attached to** `bool maybeCloseWindow(std::uint32_t predictionTick, RelayReadWindowSummary& out)`

```
// Close the window if `predictionTick` has advanced past it, filling `out` and
// resetting the counters. Returns true ONLY when a window closed AND carried at
// least one read, so an authority (which allocates no relay stores and therefore
// never reaches either call site) and an idle client never heartbeat a line.
//
// DRIVEN BY THE PREDICTION TICK, and only by it: it is the one monotonic
// per-frame clock either call site has. A HARD RESYNC can move it BACKWARDS,
// which is not an error here — the window simply restarts at the new tick
// rather than staying open for the ~4 billion ticks an unsigned subtraction
// would compute.
```

**attached to** `m_windowMaxRun    = 0u`

```
// The per-id CURRENT runs deliberately survive the window boundary — a
// starvation that straddles two windows is one run, not two. Only the
// window's MAXIMUM is reset, which is the value being reported.
```

**attached to** `void forgetOwner(unsigned int id)`

```
// Drop per-id state for a character that has unregistered. Without this the run
// map would grow with every character that has ever existed in the session.
```

**attached to** `const RelayReadCounters& predictionCounters() const { return m_prediction; }`

```
// --- introspection; tests and diagnostics only -------------------------
```

**attached to** `void noteRun(unsigned int id, ScheduledRelayedReadOutcome outcome)`

```
// [T20] The stale-run half of notePredictionRead, factored out so the two
// overloads cannot drift. The rule itself is T19's, unchanged: see the block
// above notePredictionRead for why rung 0 neither increments nor resets.
```

**attached to** `std::unordered_map<unsigned int, std::uint32_t> m_fallbackRuns`

```
// Live consecutive-fallback run per character. Bounded by live ids (forgetOwner).
```

**attached to** `// Gaps 1..64 are counted exactly; index 0 is unused (a zero gap is not a sample —`

```
// ---------------------------------------------------------------------------
// PROBE 2 — replication cadence, IN CAPTURE TICKS (T19 review F4).
//
// THIS IS A CORRECTNESS REQUIREMENT, NOT A UNIT PREFERENCE. The rule this feeds is
// `depth >= gap_p99 + margin`, and `depth` is denominated in CAPTURE ticks — a ring
// entry IS a capture tick. A gap measured in the RECEIVER's local ticks conflates
// the sender's cadence with the receiver's clock geometry (two different leads over
// the server, plus stalls and resyncs), so the resulting p99 would be in the wrong
// units and could not legitimately be compared against `depth` at all.
//
// The measurement is therefore: the newest `captureTick` in the ring on THIS
// arrival minus the newest on the PREVIOUS arrival, per component. Same units as
// depth, immune to local clock skew, and free — `populateRemoteInputCache` already
// walks every entry, so T19 added the newest-captureTick field to the report it
// already returned rather than adding a second walk.
//
// STRUCTURALLY LOCAL-TICK-PROOF: `noteArrival` is not given a local tick and there
// is no clock in this file, so a local-tick implementation cannot be written here
// by accident. The Catch2 case that pins this drives arrivals whose local spacing
// differs from their capture spacing and asserts the CAPTURE answer.
//
// A HISTOGRAM, NOT A MEAN — and not a mean plus a max either. A mean hides exactly
// the tail that sets `depth`: a channel that delivers every tick except for one
// 20-tick stall per second has a mean gap near 1 and a p99 near 20, and it is the
// 20 that the depth rule must clear. Buckets are exact for gaps 1..kMaxTrackedGap
// (the whole range that can ever matter, since the store holds only 64 capture
// ticks); anything larger lands in one saturating bucket AND is still reported
// exactly through `maxGap`, so a saturated p99 is always accompanied by a real
// number.
// ---------------------------------------------------------------------------
```

**attached to** `inline constexpr std::uint32_t kRelayArrivalMaxTrackedGap = 64u`

```
// Gaps 1..64 are counted exactly; index 0 is unused (a zero gap is not a sample —
// see noteArrival) and index kGapOverflowBucket absorbs everything larger.
```

**attached to** `inline constexpr std::uint32_t kRelayArrivalProbeWindowSamples = 120u`

```
// One summary per this many GAP SAMPLES. Sample-driven rather than tick-driven
// because the game thread has no simulation tick; at depth 1 and a healthy wire a
// component replicates about once per tick, so 120 samples is ~2 s per component —
// the same feel as the physics-side window without pretending to share its clock.
```

**attached to** `inline constexpr std::uint32_t kRelayArrivalDiscontinuityTicks = 16u`

```
// ---------------------------------------------------------------------------
// ⭐ [T34 rework] THE DISCONTINUITY GUARD — the same guard, for the same reason,
// that `FrameHealthProbe` (`kFrameHealthDiscontinuityTicks`) and `RelayWriteProbe`
// (`kRelayWriteDiscontinuityTicks`) already carry. A capture-tick jump larger than
// this is a SINGLE CORRELATED EVENT — a connection hiccup, a host stall, a
// relevancy pause, a re-join, the client's game thread blocking so OnReps queue —
// not `gap - 1` independently lost inputs. Charging it to the per-input loss rate
// mixes two distributions and reports catastrophic failure on a working relay.
//
// WHY THIS MATTERS MORE HERE THAN ON EITHER SIBLING: `lostCaptureTicksX1000` is the
// discriminating term of item 34's acceptance gate, whose pass condition is ~11 per
// mille. A single 47-tick gap in a 120-sample window computes to ~355 per mille —
// 30x the pass condition, on a window whose other 118 samples are healthy.
//
// ⚠ THE VALUE IS 16 BECAUSE THE ARCHIVES FORCE IT, not because it is round. Every
// `[RelayProbe.Arrival]` window in `runs/t39_runA_3char`, `runs/t39_runB_2char` and
// `runs/t33_depth1_control` was re-read; the observed gap distribution is BIMODAL
// with a completely empty band:
//
//   healthy   every window's `max=` is in 1..6, and p99 never exceeds 4
//   EMPTY     no sample anywhere in 7..17, across ~28,000 samples in three runs
//   outliers  18, 20, 20, 22, 46, 47 and four samples >= 64 (max 229)
//
// 16 sits at the top of that empty band: 2.6x the worst healthy `max` and 4x the
// worst healthy p99 below it, 2 ticks of margin below the smallest outlier above
// it. It is also exactly 2 * `relayedInputRing::kMaxDepth` (8) — one flush round
// can publish at most kMaxDepth capture ticks, so a gap of more than two full
// rounds cannot be produced by the flush path at all; something upstream stopped.
//
// The reviewer's suggested 15-30 range is only satisfiable at its very bottom: a
// threshold of 20 or 24 would let the 18/20/22 class through, and the window
// carrying 18 and 22 still computes to ~250 per mille. 16 is therefore not a taste
// call — anything above 17 fails to fix the defect.
//
// And it cannot be produced by ordinary wire loss: at the measured 1.122 % per
// capture tick, 16 consecutive losses has probability ~1e-31.
//
// A gap above this RE-SEEDS the watermark, is counted in `discontinuities`, and is
// a sample of nothing — it enters neither loss accumulator, neither the histogram
// nor `maxGap`. Its magnitude is preserved exactly in `maxDiscontinuityGap` so
// nothing is hidden, only re-classified.
// ---------------------------------------------------------------------------
```

**attached to** `std::uint32_t p50    = 0u`

```
// All in CAPTURE ticks. p50/p99 are nearest-rank over the histogram.
```

**attached to** `bool p99Saturated = false`

```
// True when p99 fell in the saturating bucket, i.e. the real p99 is
// ">= kRelayArrivalMaxTrackedGap". `maxGap` is still exact when this is set.
```

**attached to** `std::uint32_t noAdvance = 0u`

```
// Arrivals that replicated but advanced no capture tick (a dA re-stamp of an
// already-resident tick is the realistic cause). NOT cadence samples — they
// delivered no new capture tick, so counting them as a gap of 0 would drag the
// percentiles down and understate the depth requirement. Reported separately
// because a window that is mostly these is itself worth seeing.
```

**attached to** `std::uint32_t saturatedSamples = 0u`

```
// Samples that landed in the saturating bucket.
//
// ⚠ [T34 rework] STRUCTURALLY UNREACHABLE SINCE THE DISCONTINUITY GUARD, and
// this is deliberate rather than an oversight. The bucket sits at
// kRelayArrivalMaxTrackedGap = 64; the guard fires at 16. Every gap big enough
// to saturate is re-classified as a discontinuity before it is ever sampled, so
// `saturatedSamples` and `p99Saturated` now read 0/false forever.
//
// THEY ARE KEPT because every archived T22/T33/T39 window carries `saturated=`,
// and runB's two poisoned windows were found by it — deleting the field would
// break comparability with exactly the logs that motivated the guard. But an
// operator reading a NEW log must read `discontinuities` instead: `saturated=`
// never saw the 46/47 class at all, which is precisely why it was not enough.
```

**attached to** `std::uint32_t lostCaptureTicks      = 0u`

```
// -----------------------------------------------------------------------
// ⭐ [og-netcode-v2-input-relay T34] THE R = 0 LOSS COUNTER — MANDATORY, not
// diagnostic garnish, and the reason is structural: with bare C1 flush-on-poll
// at R = 0 an input is sent exactly ONCE, and the replication system exposes NO
// send-success signal that game code can read (T38 §4.3; the binding block at
// the top of this file names one adapter's near-miss and why it is not one). So
// permanent input loss is INVISIBLE on the server by construction.
//
// THE COUNTABLE END IS THE CLIENT. Capture ticks are per-character monotonic at
// ~60/s, so every permanently lost input is a visible ARITHMETIC GAP in the
// received stream, and nothing else can produce one: a gap of g means g-1
// capture ticks were produced by the sender and never arrived here.
//
// ⛔ [T34 loss-counter fix] THE NUMERATOR IS `gap - delivered`, NOT `gap - 1`,
// AND THE DIFFERENCE IS THE WHOLE INSTRUMENT. `gap - 1` measures ADVANCE OF THE
// NEWEST WATERMARK MINUS ONE, which equals the lost count only while an arrival
// carries exactly ONE new capture tick — the retired replace-latest regime.
// Under flush-on-poll one arrival publishes the whole staged burst, so a 2-entry
// burst advances the watermark by 2 and `gap - 1` charges 1 as lost WHILE BOTH
// ENTRIES ARRIVED. Measured on `runs/t34_run1_2char_2026-08-09_1938`, that read
// 122 / 129 per mille against a ~11 per mille pass condition — and it was
// measuring the burst rate: the run's `[RelayFlush] entriesPerRoundX100` was
// 112-113, and 1 - 1/1.13 = 115 per mille. The instrument was reporting, as
// loss, precisely the thing the flush now delivers instead of losing.
//
//   lostCaptureTicks     Sum(gap - delivered) over the window's accepted
//                        arrivals, where `delivered` is how many NEW capture
//                        ticks that arrival actually carried (the caller's
//                        count; see noteArrival). EXACT, not estimated: the
//                        watermark advanced by `gap`, `delivered` of those ticks
//                        arrived, so the remainder provably never did. Wire loss
//                        + scheduler-skip loss + everything, i.e. exactly the
//                        total the user's acceptance is priced on.
//                        ⭐ IT REDUCES TO Sum(gap - 1) AT DEPTH 1, so every
//                        archived comparison and the replace-latest
//                        counterfactual stay meaningful.
//   deliveredCaptureTicks
//                        Sum(delivered), clamped per sample to `gap`. Reported
//                        so the window is SELF-CHECKING: `lost + delivered ==
//                        expected` must hold exactly, on the log line as well as
//                        in a test. Without it a reader cannot tell a window
//                        that lost nothing from a window whose delivered count
//                        was never plumbed through.
//   expectedCaptureTicks Sum(gap) — the capture ticks the senders PRODUCED over
//                        the span this window covers. The only honest
//                        denominator: it is derived from the same samples, so
//                        it stays correct across window edges, across a varying
//                        number of remote characters, and on a client whose own
//                        frame rate has nothing to do with the senders'.
//   lostCaptureTicksX1000
//                        per mille of expected. STEADY-STATE EXPECTATION ~ 11
//                        (the measured 1.122 % wire loss). Sustained material
//                        excess over the server's `[RelayProbe.Budget] lost=`
//                        rate means scheduler SKIPS are happening on top of wire
//                        loss, which is the evidence-driven trigger to re-check
//                        the diet margins or put R = 1 back on the table
//                        (T38 §13.4). It must be reported for the join-settling
//                        window SEPARATELY from steady state (T43 finding 3).
//
// WHY IT RIDES THE EXISTING WINDOW RATHER THAN A NEW ONE: it is a ratio of two
// quantities this probe already computes per sample, and a second window over
// the same samples would be a second clock to keep honest for no new
// information. The window is shared across remote ids (as it always has been),
// and the ratio aggregates correctly across them because both numerator and
// denominator are sums over the same sample set.
//
// ⚠ A no-advance arrival is NOT a sample and contributes to NONE of the three
// terms — it advanced no capture tick, so it says nothing about loss.
// `noAdvance` above already reports those separately.
// -----------------------------------------------------------------------
```

**attached to** `std::uint32_t discontinuities      = 0u`

```
// ⭐ [T34 rework] Gaps this window RE-CLASSIFIED rather than charged, per
// kRelayArrivalDiscontinuityTicks. Non-zero means the window covers less
// continuous capture stream than its `samples` suggests.
//
// ⚠ THE OPERATOR RULE, and it is the reason this field exists at all: a window
// reporting `discont=` > 0 is DISCARDED, not averaged in — exactly as
// `RelayWriteProbe::discontinuities` is. `lostCaptureTicksX1000` is still
// honest for the samples it kept, but the excluded event was real and the
// window no longer covers a contiguous span.
//
// `maxDiscontinuityGap` is the largest EXCLUDED gap, exact. Without it the guard
// would silently swallow the one number that says how bad the interruption was —
// and before the guard existed, `max=` was the only tell those windows had.
```

**attached to** `class RelayArrivalProbe`

```
// ---------------------------------------------------------------------------
// RelayArrivalProbe — GAME THREAD ONLY. Probe 2.
// ---------------------------------------------------------------------------
```

**attached to** `bool noteArrival(unsigned int   id,`

```
// Record one relay-ring arrival for `id`, carrying the newest capture tick the
// ring held AND how many NEW capture ticks that ring actually delivered.
// Returns true — filling `outSummary` and resetting the histogram — when this
// sample completed a window.
//
// ⛔ [T34 loss-counter fix] `newCaptureTicksDelivered` IS REQUIRED AND HAS NO
// DEFAULT, DELIBERATELY. A default of 1 is exactly the retired replace-latest
// premise — "one arrival carries one entry" — and silently re-defaulting to it
// is how this instrument came to report ~120 per mille on a flush that lost
// nothing (see RelayArrivalWindowSummary's loss block). Making it a positional
// requirement turns every call site into a statement of what that arrival
// delivered, and makes a pre-fix call site a compile error rather than a
// silently-wrong number.
//
// WHERE THE COUNT COMES FROM at the shipped call site:
// `RelayedInputIngestReport::newCaptureTicksIngested`, which the ingest already
// computes while walking the ring. It counts entries whose capture tick was NOT
// already resident, because re-delivery of a tick this receiver already holds is
// not new coverage.
//
// IT IS CLAMPED TO `gap` before it is used. The exactness argument — "the
// watermark advanced by `gap`, `delivered` of those ticks arrived, so the rest
// never did" — is an argument about the half-open interval
// `(previousNewest, newestCaptureTick]`, which holds exactly `gap` tick slots.
// An arrival that additionally back-fills a hole BELOW the previous watermark
// (not reachable under R = 0's monotonic single-publish stream, but not
// excluded by any type) would otherwise subtract coverage that belongs to an
// earlier interval, and could underflow the unsigned difference. The clamp
// makes the counter unable to report negative loss as an enormous positive one.
//
// `outGapCaptureTicks` receives the gap this arrival contributed, or 0 when it
// contributed none (first arrival for this id, or no advance). It exists so the
// caller can emit its per-event Verbose line without re-deriving the gap.
//
// FOUR ARRIVALS ARE NOT SAMPLES, and each is a different thing:
//   * the FIRST arrival for an id — there is no previous newest to subtract, so
//     there is no gap. Counted nowhere; it only seeds the watermark.
//   * an arrival whose newest capture tick did NOT advance — see
//     RelayArrivalWindowSummary::noAdvance.
//   * an arrival whose newest capture tick went BACKWARDS. The relay stream is
//     monotonic in capture tick by construction (the server relays only on
//     `parked && acceptedNew`), so this should not happen; if it ever does, a
//     signed-negative "gap" would be nonsense, so it is treated as a no-advance
//     and the watermark is left alone rather than being rewound.
//   * [T34 rework] an arrival whose gap EXCEEDS
//     kRelayArrivalDiscontinuityTicks — a stall or an interruption, not
//     `gap - 1` lost inputs. Counted in `discontinuities`, magnitude kept in
//     `maxDiscontinuityGap`, and the watermark IS advanced so the next arrival
//     measures from the far side of it.
```

**attached to** `if (gap > kRelayArrivalDiscontinuityTicks)`

```
// ⭐ [T34 rework] THE DISCONTINUITY GUARD. See
// kRelayArrivalDiscontinuityTicks for why 16 and why this is not optional:
// without it a single stall reports ~355 per mille loss on a window whose
// other 118 samples are perfect, against a pass condition of ~11.
//
// The watermark has already been advanced above, so the NEXT arrival
// measures from the far side of the interruption rather than re-charging it.
// This is a sample of nothing: no histogram bucket, no `maxGap`, no
// `m_samples`, NONE of the three accumulators — and therefore it cannot
// close a window either, which is deliberate. A window must close on 120
// real samples.
//
// ⚠ [T34 loss-counter fix] It composes with the delivered count the only way
// that is coherent: `newCaptureTicksDelivered` is discarded here TOO. A
// discontinuous arrival may well carry a full burst, but the interval it
// spans is not a contiguous capture stream, so neither its loss nor its
// coverage is a statement about the wire. Crediting its delivered ticks
// while not charging its gap would let an interruption IMPROVE the reported
// rate — an instrument that reads healthier the worse the connection gets.
```

**attached to** `return false`

```
// `*outGapCaptureTicks` stays 0: its documented contract is "the gap
// this arrival CONTRIBUTED", and this one contributed none. The
// magnitude is not lost — it rides the window line as `discontMax=`.
```

**attached to** `const std::uint32_t delivered = (newCaptureTicksDelivered > gap)`

```
// [T34] THE R = 0 LOSS COUNTER. Accumulated on the same accepted-arrival
// path as the histogram, so the two can never disagree about which samples
// they cover. See RelayArrivalWindowSummary's block for why
// `gap - delivered` IS the permanently-lost count and why `gap` is the right
// denominator.
//
// ⛔ [T34 loss-counter fix] The clamp is not defensive tidiness: it is what
// keeps the subtraction below an honest statement about the interval
// (previousNewest, newestCaptureTick], which holds exactly `gap` ticks. See
// the block above noteArrival.
```

**attached to** `std::uint32_t sampleCount()     const { return m_samples; }`

```
// --- introspection; tests and diagnostics only -------------------------
```

**attached to** `void peekSummary(RelayArrivalWindowSummary& out) const { fillSummary(out); }`

```
// The percentile the window WOULD report right now. Exposed so a test can
// assert a distribution without having to fill an exact window.
```

**attached to** `std::size_t trackedOwnerCount() const { return m_lastNewestCaptureTick.size(); }`

```
// [og-netcode-v2-input-relay item 91 part C] Mirrors `RelayReadProbe::
// trackedOwnerCount()` above (this file, this class's PT sibling) — same
// reason: a direct, no-window-required proof that `forgetOwner` actually
// shrinks the id-keyed map, for a leak-freedom test that does not want to
// depend on `noteArrival`'s first-vs-subsequent-arrival semantics as an
// indirect signal.
```

**attached to** `std::uint32_t percentile(std::uint32_t numeratorPercent, bool& outSaturated) const`

```
// NEAREST-RANK. p_q is the smallest gap value whose cumulative count reaches
// ceil(q * n) — the standard definition, chosen over interpolation because the
// samples are integer tick counts and an interpolated 3.7-tick p99 would have
// to be rounded up to 4 before it could be compared against `depth` anyway.
```

**attached to** `const std::uint64_t scaled = static_cast<std::uint64_t>(numeratorPercent)`

```
// ceil(numeratorPercent * n / 100) without floating point.
```

**attached to** `outSaturated = false`

```
// Unreachable while `rank <= m_samples`, which the arithmetic above
// guarantees; falling through to the exact max is the harmless answer.
```

**attached to** `m_discontinuities     = 0u`

```
// [T34 rework] Per-window, like FrameHealthProbe's: the field's job is to
// tell an operator whether THIS window was interrupted, and a cumulative
// count would mark every window after the first as suspect forever.
```

**attached to** `}`

```
// The per-id watermark deliberately SURVIVES the window boundary: the gap
// across a window edge is a real gap, and dropping the watermark would
// silently discard one sample per component per window.
```

**attached to** `std::unordered_map<unsigned int, std::uint32_t> m_lastNewestCaptureTick`

```
// Newest capture tick seen per component. Bounded by live ids (forgetOwner).
```

**attached to** `std::uint32_t m_lostCaptureTicks      = 0u`

```
// [T34] The R = 0 loss counter's accumulators. All reset with the window; the
// per-id watermark that produces the gaps deliberately does not.
// [T34 loss-counter fix] `m_deliveredCaptureTicks` is the third term, and the
// invariant `lost + delivered == expected` is what makes the window checkable.
```

**attached to** `std::uint32_t m_discontinuities     = 0u`

```
// [T34 rework] The discontinuity guard's tally. See
// kRelayArrivalDiscontinuityTicks.
```

**attached to** `// Exact buckets for 0..64 sim ticks per frame; anything larger saturates. 0 is a`

```
// ---------------------------------------------------------------------------
// [T20; renamed + extended to both roles T49] PROBE 4 — SIM TICKS PER
// GAME-THREAD FRAME, i.e. FRAME HEALTH.
//
// ORIGIN, SERVER-ONLY. This probe was built to measure the CAUSE of the quantity
// probe 2 measures the EFFECT of. Probe 2 reports `gapCaptureTicks` — how many
// capture ticks a client's relay ring skipped between two arrivals. This probe
// reports how many sim ticks a game thread advanced between two frames. On the
// SERVER the two are predicted to be the same number, and that prediction was the
// whole T20 experiment:
//
//   * Property replication runs once per server GAME-THREAD FRAME, and the
//     server frame-rate setting is a CAP on that rate, never a floor.
//   * A 60 Hz async sim on a 30 fps server therefore advances TWO sim ticks per
//     replication, and a replace-latest ring at depth 1 can only carry the newer
//     one. `G ~ 60 / serverFrameRate` — every measured number in
//     RelayDepthCoverageHypothesis falls out of that one relation.
//   * So if ticks-per-frame p50 EQUALS the measured gap p50, G is a HOST
//     PERFORMANCE artefact (the observed sessions ran a dedicated server, two
//     clients and the editor on one machine) and there is no netcode defect. If
//     ticks-per-frame is 1 while the gap is still 2, replication is being skipped
//     for some other reason and the depth hypothesis is not the explanation.
//
// [T49] EXTENDED TO THE CLIENT. Nothing above is server-specific — it is a
// hook-independent ratio over two caller-supplied counters (see below) — and item
// 49 named the gap directly: `[RelayProbe.Frame]` was the ONLY wall-clock timing
// instrument anywhere in this netcode surface, and it was server-only by
// construction even though the server never resims and the client does. On a
// client this same ratio, plus its own `meanFrameMicros`/p99/max, is frame HEALTH
// under resim load — the quantity every client cost figure in
// `finding_task43_resim_gate_live.md` §4 had to be DERIVED from `ResimGateProbe`
// cadence instead of measured, because nothing sampled wall-clock time on that
// thread. The type was renamed from `ServerFrameProbe` (its T20 name) to
// `FrameHealthProbe` accordingly — nothing role-specific survives in its math, so
// nothing role-specific should survive in its name.
//
// TWO ROUTES TO A RATIO ABOVE 1, AND THEY ARE NOT THE SAME DEFECT. The physics
// engine may run several fixed sub-steps inside one game frame (`numSteps > 1`),
// or the game thread may simply be slow. Both show up as ticks-per-frame > 1 and
// they need different fixes, so this probe reports the sub-step count ALONGSIDE
// the ratio and never collapses them. ON A RESIMMING CLIENT THIS SEPARATION IS
// THE WHOLE POINT:
// a resim burst folds extra sim ticks into `numSteps` at the SAME hook, so a ratio
// above 1 with `numStepsAboveOne > 0` reads as "resim ran", not "the game thread
// hitched" — the two client-cost questions item 49 exists to keep apart.
//
// HOOK-INDEPENDENT BY CONSTRUCTION. The caller supplies a GLOBAL frame counter, not
// an invocation count, so the ratio is correct no matter which game-thread hook
// feeds it and no matter how often that hook fires — the same property that let
// this probe move to a second role without a second implementation. The probe
// additionally reports how its own invocations distributed over frames —
// `dFrame == 1` (once per frame, the assumed case), `dFrame == 0` (fired more than
// once in a frame, i.e. per sub-step) and `dFrame > 1` (frames it did not fire on).
// THAT IS THE VERIFICATION THE TASK ASKS FOR: the hook's cadence is measured, never
// assumed — on EITHER role, from its own counters, independent of which hook the
// caller chose.
//
// GAME THREAD ONLY, on WHICHEVER ROLE the owning object runs on. Like the other two
// objects here it holds no lock and needs none — it is touched from exactly one
// thread. Each role gets its OWN instance (SimulationManagerUImpl's
// `m_frameHealthProbe` is a plain member, constructed once per actor, and both a
// server-role and a client-role actor exist as separate objects whenever ONE
// PROCESS HOSTS BOTH ROLES) — never shared, so there is no cross-role
// synchronization question either.
//
// TICK SOURCE IS THE CALLER'S PROBLEM, and there is only one right answer on the
// game thread: the tick↔physics-frame mapper's atomic offset. The underlying clock
// (the server tick or the client's prediction tick) is written on the physics
// thread and must not be read here on either role. This probe takes a plain
// number and therefore cannot make that mistake on the caller's behalf — the call
// site carries the comment, including item 49's argument for why the SAME atomic
// read that was already proven safe for the server call site is equally safe for
// the client one.
// ---------------------------------------------------------------------------
```

**attached to** `inline constexpr std::uint32_t kFrameHealthMaxTrackedTicks = 64u`

```
// Exact buckets for 0..64 sim ticks per frame; anything larger saturates. 0 is a
// real observation here (a frame in which physics did not step), which is why the
// range starts at 0 rather than at 1 as the arrival histogram's does.
```

**attached to** `inline constexpr std::uint32_t kFrameHealthProbeWindowSamples = 120u`

```
// One summary per this many samples. At an intended 60 fps that is ~2 s, the same
// feel as the other two windows; on a 30 fps server it is ~4 s, which is itself
// informative — the summaries thin out exactly when the measured thing is bad.
```

**attached to** `inline constexpr std::uint32_t kFrameHealthDiscontinuityTicks = 600u`

```
// A sim-tick jump larger than this is a DISCONTINUITY (the mapper offset being
// established, a level transition, a multi-second editor stall), not a hitch.
// Sampling it would put a five-digit outlier in `max` and hide every real number
// behind it. Counted and reported, never silently dropped.
```

**attached to** `std::uint32_t samples          = 0u`

```
// --- the ratio, over samples where EXACTLY ONE frame elapsed -------------
```

**attached to** `std::uint32_t totalSimTicks         = 0u`

```
// --- the ratio, aggregated over the WHOLE window ------------------------
// Cadence-independent: the totals are differences of the CALLER's own counters,
// so this stays right even if the hook fired twice in a frame or skipped frames.
// x100 to keep the log line integer — 250 means 2.50 sim ticks per frame.
```

**attached to** `std::uint32_t oncePerFrameSamples = 0u;   // dFrame == 1 (the assumed case)`

```
// --- the HOOK CADENCE verification --------------------------------------
```

**attached to** `std::uint32_t totalNumSteps    = 0u`

```
// --- the SUB-STEP cross-check -------------------------------------------
// The physics engine's own count of fixed steps the frame will advance.
// numSteps > 1 is SUB-STEPPING; a ratio above 1 with numSteps == 1 throughout
// is FRAME-RATE SHORTFALL. Reported side by side so the two are never
// conflated.
```

**attached to** `bool noteFrame(std::uint64_t frameCounter,`

```
// Record one game-thread sample. Returns true — filling `out` and resetting the
// window — when this sample completed a window.
//
//   frameCounter  a GLOBAL, monotonic frame number — THE ENGINE'S OWN frame
//                 counter, NOT an invocation count. The whole
//                 hook-independence property rests on that.
//   simTick       the sim tick this frame is about to advance to, from a source
//                 that is safe to read on the GAME thread.
//   numSteps      the physics engine's sub-step count for this frame.
//   nowMicros     a monotonic wall-clock reading, for the mean frame time.
```

**attached to** `m_totalNumSteps += numSteps`

```
// The sub-step cross-check is per INVOCATION and is accumulated before any
// of the frame-delta reasoning, so it stays correct whatever the cadence
// turns out to be.
```

**attached to** `if (simTick < m_lastSimTick`

```
// A backwards or implausible jump is a discontinuity, not a measurement.
// Re-seed from it rather than recording it.
```

**attached to** `std::uint32_t ratioSampleCount() const { return m_ratioSamples; }`

```
// --- introspection; tests and diagnostics only -------------------------
```

**attached to** `void peekSummary(FrameHealthWindowSummary& out,`

```
// The summary the window WOULD report right now, given the caller's current
// counters. Exposed so a test can assert a distribution without filling a window.
```

**attached to** `std::uint32_t percentile(std::uint32_t numeratorPercent, bool& outSaturated) const`

```
// NEAREST-RANK, and the bucket index IS the tick count — same definition as
// RelayArrivalProbe::percentile, except that bucket 0 is a real observation.
```


### 16.2 `Network/RelayWritePathProbe.h` — as it stood before compression

**attached to** `#include <array>`

```
// SPDX-License-Identifier: MPL-2.0
```

**attached to** `// ---------------------------------------------------------------------------`

```
// ---------------------------------------------------------------------------
// RelayWriteProbe / ConnectionBudgetProbe — the SERVER-side relay telemetry
// T22 needs, and the two numbers nobody has ever measured.
// (og-netcode-v2-input-relay T22. Companion to Network/RelayReadProbe.h, which
// holds the CLIENT-side arrival/read probes plus FrameHealthProbe.)
//
// ---------------------------------------------------------------------------
// WHY A SEPARATE FILE. This is a DIAGNOSTIC UNIT with a stated end date. T22 is a
// measurement task, not a fix: if its result is "the mechanism is elsewhere",
// deleting this header, its two call sites and its test file removes the whole
// instrument in one reviewable change. RelayReadProbe.h is shipped telemetry that
// three later tasks read; mixing a disposable probe into it would make that
// separation a diff-archaeology exercise later. Same STL-only, no-logging,
// caller-owns-the-window conventions as that file — see its banner.
//
// ---------------------------------------------------------------------------
// ONE ADAPTER'S BINDING FOR THE REPLICATION NAMES. This header names ROLES only —
// the replication system, the replication poll, the replication writer, the
// game-thread frame identity. For one adapter (Unreal) those bind to Iris, its
// once-per-frame property poll, `FReplicationWriter` and `GFrameCounter`.
// Another adapter substitutes its own; nothing in this header depends on them.
// The transport-connection roles carry their own binding block at PROBE 6.
//
// ---------------------------------------------------------------------------
// ⭐ THE GAP IN THE ELIMINATION CHAIN THIS EXISTS TO CLOSE.
//
// `RelayDepthCoverageHypothesis.md` §9.11a eliminates every upstream stage and
// concludes "the loss is in the replication send path". Its elimination table has
// a hole, and it is a precise one:
//
//   | stage                        | evidence cited                     |
//   | server writes the ring       | writes on every accepted receipt,  |
//   |                              | and receipts are complete          |
//   | server frame rate            | 1.003 sim ticks/frame, p99 = 1     |
//
// Both statements are TRUE and neither covers this case. The ring is written from
// the RPC RECEIPT path (`ServerReceptionCoordinator::receiveRemoteInput`'s relay
// tap -> `relayRemoteInput` -> `writeLatest`), on the game thread, once per
// genuinely-new capture tick. It is NOT written from the sim tick. So:
//
//   * "receipts are complete" says no capture tick is LOST on the wire into the
//     server. It says nothing about how many of them land in the SAME game-thread
//     frame.
//   * "1.003 sim ticks per frame" measures the SIM's pacing. The relay writes are
//     paced by PACKET ARRIVAL, which is a different clock with different jitter.
//
// And the ring shipped, at the time this probe was built, at a compiled
// retention depth of 1 entry — REPLACE-LATEST (item 34 later replaced that
// write path; item 63 / RN-13 then retired the now-inert depth field itself —
// see RN-13, ReviewNotes.md). The replication
// system polls a replicated property ONCE PER SERVER GAME-THREAD FRAME and compares
// the live value against its shadow copy (T20 §4.4 for the cadence, §4.5 for the
// compare-against-latest semantics: "three writes produce one compare against the
// third"). Therefore:
//
//   ⇒ IF TWO RELAY WRITES LAND IN ONE GAME-THREAD FRAME, THE FIRST ONE IS
//     UNOBSERVABLE. It is not dropped by the network, not deferred by the
//     replication writer, and not visible to any client-side probe. It is
//     overwritten in server memory before replication ever looks at it.
//
// That loss is INDISTINGUISHABLE, from the client, from a send-path drop: the
// client sees a relay ring whose newest capture tick advanced by more than 1.
// `RelayArrivalProbe` reports it as `gapCaptureTicks > 1`. Everything §9.11
// measured is consistent with either cause.
//
// THE ARITHMETIC THAT MAKES THIS TESTABLE. Over a window, writes arrive at the
// capture rate (60/s, since client->server receipt is measured complete) and are
// observable at most once per WRITING FRAME. So
//
//     observable fraction  <=  writingFrames / totalWrites
//                           =  1 / (mean writes per writing frame)
//
// The §9.11 clean-run baseline measured a delivered fraction of 59.3 %, i.e. a
// mean arrival gap of 60/35.6 = 1.685 capture ticks. If write coalescing is the
// mechanism, THIS PROBE MUST REPORT A MEAN OF ~1.69 WRITES PER WRITING FRAME AND A
// RUN-LENGTH HISTOGRAM THAT MATCHES THE CLIENT'S GAP HISTOGRAM SHAPE FOR SHAPE
// (p50 = 1, p99 = 3-7, max = 8). If instead it reports ~1.00, coalescing
// contributes nothing and the loss really is downstream — which is the outcome
// that keeps the send-path candidates alive.
//
// BOTH OUTCOMES ARE INTERPRETABLE IN ADVANCE, WHICH IS THE POINT. The interpretation
// table lives in `impl/pie_script_t22.md`; it is written before the run, not after.
//
// ---------------------------------------------------------------------------
// ⭐ [og-netcode-v2-input-relay T34] THE ANSWER CAME BACK "COALESCING", AND THE
// MECHANISM ABOVE HAS SINCE BEEN REMOVED. Read everything above as the QUESTION
// this probe was built to settle, not as a description of the shipped write path.
//
// The measurement said the write path loses ~11.6 % of relayed inputs to
// same-frame coalescing (observability ~884 per mille). Item 34 replaced
// replace-latest with BARE C1 FLUSH-ON-POLL: arrivals are staged and the whole
// stage is published once per replication poll, so a second write in a frame no
// longer overwrites the first — it is published beside it (`relayedInputRing::
// stageArrival` / `flushStagedInto`). The paragraph beginning "IF TWO RELAY WRITES
// LAND IN ONE GAME-THREAD FRAME" is therefore HISTORICAL: it is true of the path
// this probe measured, and false of the path that ships.
//
// THE PROBE SURVIVES BECAUSE THE QUESTION DID NOT GO AWAY, IT SHRANK. A burst
// LONGER than the stage capacity still loses its oldest entries, and that ceiling
// is what `observableX1000` now reports (see its field comment). The probe is also
// item 34's own acceptance instrument: its pass condition is
// `observableX1000 >= 990`. It is no longer disposable in the sense the WHY A
// SEPARATE FILE note above intended.
//
// ---------------------------------------------------------------------------
// WHAT THIS DELIBERATELY DOES NOT CLAIM. A run length of N means N writes shared a
// frame. It does NOT prove the N-1 older ones would otherwise have arrived — a
// send-path drop could be stacked on top of coalescing, and the two are additive.
// The probe reports the CEILING on observability that the write pattern imposes;
// comparing that ceiling against the client's measured arrival rate is what
// separates "coalescing explains all of it" from "coalescing explains part of it".
//
// ---------------------------------------------------------------------------
// INSTRUMENTATION ONLY, and GAME THREAD ONLY (server). Neither object holds a
// lock, neither logs, neither feeds back into any simulated value. Removing this
// header changes no behaviour.
//
// NAMESPACE NOTE: global namespace, matching the rest of the OGSim core.
// ---------------------------------------------------------------------------
```

**attached to** `// Exact buckets for run lengths 1..16; anything longer saturates. A run length is`

```
// ---------------------------------------------------------------------------
// PROBE 5 — RELAY WRITES PER SERVER GAME-THREAD FRAME.
// ---------------------------------------------------------------------------
```

**attached to** `inline constexpr std::uint32_t kRelayWriteMaxTrackedRun = 16u`

```
// Exact buckets for run lengths 1..16; anything longer saturates. A run length is
// the number of relay writes for ONE owner inside ONE game-thread frame, so 0 is
// not a possible observation (a run only exists because a write created it) and
// the histogram starts at 1 — the same convention RelayArrivalProbe's gap
// histogram uses, deliberately, so the two are read side by side without an
// index shift. 16 is well past the §9.11 max gap of 8.
```

**attached to** `inline constexpr std::uint32_t kRelayWriteProbeWindowRuns = 120u`

```
// One summary per this many COMPLETED RUNS (writing frames), per owner. Driven by
// runs and not by writes so the sample count means the same thing as
// RelayArrivalProbe's does: 120 observable events. At the predicted ~35 writing
// frames/s that is ~3.4 s; at a healthy 60 it is ~2 s.
```

**attached to** `inline constexpr std::uint32_t kRelayWriteDiscontinuityTicks = 600u`

```
// A capture-tick jump larger than this is a DISCONTINUITY (registration, a client
// re-join, the tick domain being re-established), not a coverage hole. Counted
// separately and never allowed into `missedCaptureTicks`, where a five-digit
// outlier would swamp every real number — the same guard, for the same reason, as
// FrameHealthProbe's `kFrameHealthDiscontinuityTicks`.
```

**attached to** `unsigned int ownerId = 0u`

```
// --- identity -----------------------------------------------------------
```

**attached to** `std::uint32_t runs         = 0u`

```
// --- THE HEADLINE: writes per writing frame -----------------------------
// `runs` is the number of game-thread frames in which this owner's ring was
// written at least once. `writes` is the number of relay writes those frames
// contained. Their ratio is the coalescing factor.
```

**attached to** `std::uint32_t firstCaptureTick = 0u`

```
// --- THE CAPTURE-TICK DENOMINATOR ---------------------------------------
// The span of capture ticks this window covers, inclusive:
// `lastCaptureTick - firstCaptureTick + 1`. This is the number of capture
// ticks the sending client PRODUCED over the window, and it is the only
// denominator that makes the server's numbers directly comparable to the
// client's arrival rate — which is itself measured against 60 captures/s.
```

**attached to** `std::uint32_t receivedX1000    = 0u`

```
// --- THREE FRACTIONS, PARTS PER THOUSAND, DELIBERATELY NOT COLLAPSED -----
// Each names a different stage, and a single "loss" number would make the
// remedy decision on merged evidence.
//
//   receivedX1000    `writes * 1000 / captureSpan` — of the capture ticks the
//                    client produced, how many the server actually received.
//                    UPSTREAM completeness. Depth cannot improve this; only
//                    input redundancy can. §9.11 measured this as ~1000
//                    indirectly (`[InputStats] dropped 0/238`); this is the
//                    direct reading.
//   observableX1000  `observableWrites * 1000 / writes` — of what the server
//                    received, how much a once-per-poll publish could ever see.
//                    THE COALESCING CEILING. 1000 = no coalescing.
//                    ⭐ [T34] ITS DEFINITION IS UNCHANGED; ITS ARITHMETIC MOVED
//                    WITH THE MECHANISM. Under the retired replace-latest write
//                    path a writing frame published exactly ONE entry, so the
//                    ceiling was `runs / writes` (~884 measured). Under bare C1
//                    flush-on-poll the whole staged burst is published, so the
//                    ceiling is `Sum min(runLength, stageCapacity) / writes` —
//                    only a burst LONGER than the stage loses anything, which at
//                    a stage capacity of 8 against a measured max run of 8 is
//                    ~1000. Item 34's acceptance gate reads this field and its
//                    pass condition is `>= 990`; leaving the old arithmetic in
//                    place would have made that gate unpassable by construction
//                    while the mechanism worked perfectly.
//   replaceLatestObservableX1000
//                    `runs * 1000 / writes` — the SAME ceiling computed the way
//                    the replace-latest path imposed it. Kept ONLY so archived
//                    T22/T33/T39 windows stay directly comparable and so the
//                    improvement flush-on-poll bought is visible as two numbers
//                    on one line rather than as a claim. It measures nothing
//                    live: no code path has that ceiling any more.
//   deliverableX1000 `observableWrites * 1000 / captureSpan` — the product, and
//                    ⭐ THE NUMBER TO COMPARE AGAINST THE CLIENT. §9.11
//                    measured a delivered fraction of 593 (35.6 arrivals/s
//                    against 60 captures/s). If this reads ~593 the server's
//                    own write pattern already accounts for the entire loss
//                    and the send path is exonerated. If it reads ~1000 the
//                    loss is genuinely downstream.
```

**attached to** `std::uint32_t observableWrites = 0u`

```
// [T34] The numerator behind `observableX1000`: Sum min(runLength,
// stageCapacity) over the window's completed runs. Reported raw so a window's
// ceiling can be re-derived rather than trusted, and so a burst that genuinely
// overflowed the stage is visible as `writes - observableWrites` rather than
// only as a rounded per-mille.
```

**attached to** `std::uint32_t emptyFrames = 0u`

```
// --- the shape of the arrival pattern -----------------------------------
// Frames between writing frames that carried NO write for this owner. High
// `emptyFrames` with runs > 1 is CLUMPING (packets arriving in bursts); zero
// empty frames with runs == 1 everywhere is the healthy steady state.
```

**attached to** `std::uint32_t nonConsecutiveWrites = 0u`

```
// --- upstream loss, kept STRICTLY SEPARATE ------------------------------
// A write whose captureTick is not exactly prevCaptureTick + 1 means the
// server never received that capture tick at all. `missedCaptureTicks` is the
// count of those ticks and is the numerator behind `receivedX1000`.
```

**attached to** `std::uint32_t discontinuities = 0u`

```
// --- hygiene ------------------------------------------------------------
// Non-zero means the window was RESTARTED mid-flight by a capture-tick
// discontinuity (registration, re-join, tick domain re-established). A window
// reporting this covers less wall clock than it appears to. DISCARD IT rather
// than averaging it in — that is what this field is for.
```

**attached to** `struct RelayStageCapacity`

```
// [T34] How many of one frame's writes the publish step can carry —
// `relayedInputRing::kMaxDepth` under bare C1 flush-on-poll, 1 under the retired
// replace-latest path. INJECTED rather than included, so this header stays
// STL-only and so a test can drive both regimes without a build flag.
//
// A NAMED TYPE RATHER THAN A SECOND uint32, deliberately: the probe's other
// constructor argument is also a plain count, and `RelayWriteProbe probe(4u)`
// silently meaning something new is precisely the class of defect this initiative
// keeps paying for. With this wrapper every pre-T34 call site is a COMPILE error
// that has to be read, not a runtime surprise.
```

**attached to** `bool noteWrite(unsigned int ownerId,`

```
// Record one relay-ring write. Returns true — filling `out` and resetting THAT
// OWNER's window — when this write completed a window.
//
//   ownerId       the relayed character's id (the key the client-side
//                 RelayArrivalProbe also uses, so the two summaries line up).
//   frameCounter  a GLOBAL, monotonic game-thread frame number — the ENGINE'S
//                 OWN frame identity, not an invocation count. The whole claim
//                 rests on that, because the frame is the unit the replication
//                 system polls on.
//   captureTick   the capture tick being written. Monotone per owner by
//                 construction (the relay tap sits inside the coordinator's
//                 `acceptedNew` arm), so a regression here is a bug and is
//                 counted as a discontinuity rather than silently absorbed.
//
// A WINDOW CLOSES ON A RUN BOUNDARY, NOT ON A WRITE. The run that is still
// open cannot be counted — its length is not known until a later frame's first
// write proves it ended. Closing on writes would put a truncated run in the
// histogram once per window and bias `maxRun` and the mean downward.
```

**attached to** `const bool backwards   = (captureTick <= s.lastCaptureTick)`

```
// A DISCONTINUITY RESTARTS THE WINDOW RATHER THAN POISONING IT. A capture
// tick that jumped by more than a plausible stall — or went backwards,
// which the monotonic relay gate makes unreachable today — breaks the
// `captureSpan` denominator, and a span that no longer matches the writes
// it spans makes every fraction below meaningless. FrameHealthProbe
// re-seeds for the same reason. `discontinuities` survives the restart so
// the NEXT emitted line still says the window was interrupted.
```

**attached to** `if (advance > 1u)`

```
// Capture-tick accounting runs on EVERY write, before any frame reasoning,
// so it stays correct whatever the frame pattern turns out to be.
```

**attached to** `++s.currentRun`

```
// Coalesced: this write overwrites the last one in a depth-1 ring
// before the replication poll ever runs. The capture tick it carries
// still ends the run, so the watermark advances; the run length does
// not close yet.
```

**attached to** `s.emptyFrames += static_cast<std::uint32_t>(`

```
// A new frame started, so the previous run is now COMPLETE and countable —
// and its LAST write is the previous one, which is what closes the window's
// capture span. `captureTick` belongs to the run that is only now opening,
// and therefore to the NEXT window.
```

**attached to** `resetWindow(s, captureTick)`

```
// The next window opens on the run this write just started, so its capture
// span starts at THIS capture tick. Anchoring it anywhere else would leave
// a one-write hole between consecutive windows.
```

**attached to** `void forgetOwner(unsigned int ownerId) { m_owners.erase(ownerId); }`

```
// Drop an owner's state. Called from the same unregister contract that reaps
// the reception coordinator's claim map, so the map stays bounded by live ids.
```

**attached to** `std::size_t   trackedOwnerCount() const { return m_owners.size(); }`

```
// --- introspection; tests and diagnostics only -------------------------
```

**attached to** `bool peekSummary(unsigned int ownerId, RelayWriteWindowSummary& out) const`

```
// The summary this owner's window WOULD report right now. Exposed so a test can
// assert a distribution without having to fill a 120-run window.
```

**attached to** `std::uint32_t windowStartCapture = 0u`

```
// The window's capture-tick span. `windowStartCapture` is the first write
// of the window's first run; `windowEndCapture` is the LAST write of the
// most recently COMPLETED run — never the open one, whose extent is not
// known yet.
```

**attached to** `static void restart(OwnerState& s,`

```
// Anchor everything to this (frame, capture tick) and clear the window. Used
// for the first write and for a discontinuity — the two cases where no earlier
// state is usable.
```

**attached to** `void recordRun(OwnerState& s, std::uint32_t runLength) const`

```
// [T34] NOT static any more: the observable numerator needs the stage capacity,
// which is per-probe. Everything else is unchanged.
```

**attached to** `static std::uint32_t percentile(const OwnerState& s,`

```
// NEAREST-RANK over the run-length histogram — the same definition
// RelayArrivalProbe::percentile uses, so a p99 here and a p99 there are
// comparable without a footnote.
```

**attached to** `static void resetWindow(OwnerState& s, std::uint32_t startCaptureTick)`

```
// The OPEN run, the frame anchor and the capture-tick watermark survive a
// window reset — only the window's own accumulators are cleared. Resetting the
// run or the watermark would inject a false run boundary and a false capture
// gap at every window edge: one per 120 runs, which is exactly the size of
// effect this probe is trying to resolve.
```

**attached to** `std::uint32_t m_stageCapacity`

```
// [T34] How many of one frame's staged writes the publish step can carry.
```

**attached to** `// One summary per this many samples, per connection. At the intended once-per-`

```
// ---------------------------------------------------------------------------
// PROBE 6 — PER-CONNECTION SEND BUDGET AND THROUGHPUT.
//
// ONE ADAPTER'S BINDING FOR THE TRANSPORT NAMES BELOW. The roles are: the TRANSPORT
// CONNECTION, its NEGOTIATED RATE, its SEND-DEBT counter, its READINESS test and the
// CHANNEL that writes. For one adapter (Unreal) those are `UNetConnection`,
// `CurrentNetSpeed`, `QueuedBits`/`SendBuffer`, `UNetConnection::IsNetReady()` and
// `UDataStreamChannel::Tick`; in that one adapter the rate is negotiated inside
// `UWorld::NotifyControlMessage`. Another adapter substitutes its own — the probe
// itself takes plain numbers and never sees any of them.
//
// WHAT IT SETTLES, AND WHY THE ARITHMETIC ALONE WAS NOT ENOUGH. The task's budget
// model is: the server's per-tick send allowance is the NEGOTIATED RATE divided by
// the desired tick rate, in bytes, and a connection may bank at most two ticks of
// unused allowance. Both halves are read from engine source — for one adapter,
// `UNetConnection::Tick`: `DeltaBits = CurrentNetSpeed * clamp(DeltaTime, 0,
// 1/DesiredTickRate) * 8`, then `QueuedBits` is floored at `-2 * DeltaBits`. At a
// negotiated 250000 bit/s and 60 Hz that is 4,166.67 B/tick with 8,333 B of
// bankable credit. Against a modelled 1,413.75 B/round for three characters that
// is ~34 % occupancy — nowhere near saturation.
//
// EVERY NUMBER IN THAT PARAGRAPH IS DERIVED, NOT OBSERVED. The negotiated rate is
// what the SERVER clamped the client's request to, at runtime, on a rate-negotiation
// path nobody on this initiative has watched execute; and the modelled payload
// counts two properties out of an unknown total. This probe replaces every derived
// term with a measured one:
//
//   * `netSpeedBps` — the connection's ACTUAL negotiated rate. If this is not
//     250000, the whole 34 % reading is wrong and Protocol B's prediction inverts.
//   * `outBytes` / `outPackets` deltas — the REAL per-tick payload, all properties,
//     all framing, all headers. This is Protocol C's answer, at connection
//     resolution: the two properties T29 measured are a known 401 B/char/round, so
//     the residual is everything else.
//   * `queuedBits` min/max and `notReadyFrames` — the saturation state itself.
//     Send debt plus buffered bytes at or below zero IS the connection's READINESS
//     test, and the writing channel returns WITHOUT WRITING ANYTHING when it fails.
//     A non-zero `notReadyFrames` is the send-path deferral candidate's smoking
//     gun; a zero one with `queuedBits` pinned near its floor is that candidate's
//     refutation, measured rather than argued.
//   * `outPacketsLost` delta — ack-derived, so it is the emulation's REAL outgoing
//     loss rate rather than the configured `PktLoss` percentage. That is candidate
//     1's evidence without changing a single config value.
//
// TAKES PLAIN NUMBERS, exactly like FrameHealthProbe, so it is engine-free and
// unit-testable; reading the transport connection's fields is the CALLER's job,
// in the adapter.
// ---------------------------------------------------------------------------
```

**attached to** `inline constexpr std::uint32_t kConnectionBudgetWindowSamples = 120u`

```
// One summary per this many samples, per connection. At the intended once-per-
// server-frame cadence that is ~2 s — the same feel as every other window here.
```

**attached to** `std::uint32_t samples      = 0u`

```
// --- the window itself --------------------------------------------------
```

**attached to** `std::int32_t  netSpeedBps         = 0`

```
// --- the allowance, as the server actually negotiated it -----------------
```

**attached to** `std::uint32_t outBytes          = 0u;   // delta over the window`

```
// --- what was actually sent ---------------------------------------------
```

**attached to** `std::uint32_t occupancyPctX10   = 0u`

```
// Occupancy of the per-tick allowance, in tenths of a percent. 340 = 34.0 %.
```

**attached to** `std::int32_t  queuedBitsMin  = 0`

```
// --- the saturation state ------------------------------------------------
// The send-debt counter is NEGATIVE when there is headroom (the per-tick
// allowance pays it down), so `min` is the MOST headroom seen and `max` is the
// CLOSEST to saturation. `notReadySamples` counts samples at or above zero —
// the state in which the replication system writes nothing at all this frame.
```

**attached to** `bool noteSample(std::uint32_t connectionId,`

```
// One sample, once per server game-thread frame, per client connection.
//
//   outTotalBytes / outTotalPackets / outTotalPacketsLost
//       the connection's SESSION-CUMULATIVE byte / packet / loss counters, not
//       its periodic stat accumulators. Cumulative counters are used
//       deliberately: the periodic accumulators are reset by the engine on its
//       own schedule, so differencing them across our window would drop
//       whatever the engine zeroed mid-window.
//   tickRateHz
//       the rate the per-tick allowance is computed against — the server's
//       DesiredTickRate. Passed in rather than assumed so that a run on a
//       differently-configured server still reports a correct allowance.
```

**attached to** `if (outTotalBytes < s.startBytes || outTotalPackets < s.startPackets`

```
// A counter that went backwards means the connection was replaced under the
// same id (reconnect). Re-anchor rather than reporting a negative delta as
// a gigantic unsigned one.
```

