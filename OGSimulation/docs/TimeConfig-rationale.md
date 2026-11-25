<!-- SPDX-License-Identifier: MPL-2.0 -->
<!-- lint-external-ref: stallDebt -- the phase bit's field name until og-input-latency P6-2 renamed it transitionDebt; kept in the history -->
<!-- lint-external-ref: ASimulationTimingRelay -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
# `TimeConfig.h` — rationale

This is the narrative, the derivations, the measurement records and the deferral history for
`TimeConfig` (`PCTimeManagement/TimeConfig.h`). The header keeps the orientation block, every
per-field contract and every fence; this file carries everything else. The header is the
operational reference — read it first. Come here when you need the *why*, not the *what*.

**If this file and `TimeConfig.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **This file is not the source of truth for any VALUE.** Compiled defaults live in the header's
initialisers; the shipped configuration lives in its configuration key. Where a number appears
below it is there to make an argument readable, and the argument is what this file owns.

**One adapter's bindings, declared once here so the sections below can stay engine-neutral.** The
adapter this library was first measured on is `OGSimulationUnreal` + `Source/OGBrawlerUnreal`, on
Unreal Engine. Where a section names one of that adapter's symbols — `FApp::GetCurrentTime`,
`FReplicationWriter::HandleObjectBatchFailure`, `SplitHugeObject`, `GetBitCountSplitThreshold`,
`UPROPERTY`, `ASimulationTimingRelay` (deleted by og-input-latency task P5-9, named as history), the
`AsyncFixedTimeStepSize` key, `Config/DefaultEngine.ini`
— it is **one adapter's binding, an example and not the definition**. Another adapter substitutes
its own; the ROLE is what the argument rests on.

Origin: `og-netcode-v2-input-relay` items 34/38/39/43/45/46/63, the design notes
`RelayDelaySpectrumDesign.md`, `design_task43_resim_gate_fix.md`,
`design_task38_input_first_replication.md`, the defect note `finding_task31_resim_rate.md`,
`ReviewNotes.md` RN-12 / RN-13, and
`OGBrawlerNetworkModelResearch/arch/proposal_ogbrawler_netcode.md` §4 / §11.

> ⚠ **Every document named in that Origin line is private working material from the
> `og-netcode-v2-input-relay` initiative archive and is NOT distributed with this submodule.**
> They are named as provenance, deliberately unlinked; every claim this file *asserts* is anchored
> to a file in this repository and only to those. The declarations below tell
> `tools/lint/doc_anchor_lint.ps1` that these names are intentionally unresolvable.

<!-- lint-external-ref: RelayDelaySpectrumDesign.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: design_task43_resim_gate_fix.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: design_task38_input_first_replication.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: finding_task31_resim_rate.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: ReviewNotes.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: proposal_ogbrawler_netcode.md -- OGBrawlerNetworkModelResearch/arch, initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: FApp::GetCurrentTime -- one adapter's engine symbol; owned by the host engine, not by this repository, and must not resolve here -->
<!-- lint-external-ref: FReplicationWriter::HandleObjectBatchFailure -- one adapter's engine symbol; owned by the host engine, not by this repository, and must not resolve here -->
<!-- lint-external-ref: SplitHugeObject -- one adapter's engine symbol; owned by the host engine, not by this repository, and must not resolve here -->
<!-- lint-external-ref: GetBitCountSplitThreshold -- one adapter's engine symbol; owned by the host engine, not by this repository, and must not resolve here -->
<!-- lint-external-ref: m_isResimulated -- RETIRED NAME (item 45): the per-slot provenance bitset, replaced by a single atomic pending-anchor word; it must not resolve -->
<!-- lint-external-ref: OGBrawlerNetworkModelResearch/arch/proposal_ogbrawler_netcode.md -- the archive PATH of the proposal named above; private working material, not distributed with this submodule -->
<!-- lint-external-ref: TimeConfigOrderingTest.cpp -- test translation unit in the og-simulation-tests submodule, not distributed with this submodule -->
<!-- lint-external-ref: TimeConfigTierArrayOrderingTest.cpp -- test translation unit in the og-simulation-tests submodule, not distributed with this submodule -->
<!-- lint-external-ref: TimeConfigDefaultsTest.cpp -- test translation unit in the og-simulation-tests submodule, not distributed with this submodule -->
<!-- lint-external-ref: StateCorrectionCache::getLastResimulationTick -- RETIRED NAME (item 45): the newest-first scan this section says NO LONGER EXISTS; CorrectionCache.h states its removal at the site, and it must not resolve -->
<!-- lint-external-ref: newestLandedCorrection -- prose shorthand inside a formula, not an identifier: the quantity is the tick of the newest landed correction -->
<!-- lint-external-ref: asyncDeltaTime -- prose shorthand for one adapter's physics-solver delta, not an identifier in this repository -->
<!-- lint-external-ref: resimCooldownTicks -- RETIRED NAME: a rate ceiling built during item 45 and removed on the 2026-08-11 ruling; there is no successor by design and it must not resolve -->
<!-- lint-external-ref: Abort -- one adapter's engine enumerator (the clean batch-abort result); owned by the host engine and must not resolve here -->
<!-- lint-external-ref: deepAnchorSkips -- the configuration file's own spelling of the counter, quoted verbatim in section 11; the member is deepAnchorExclusions and this name deliberately does not resolve -->

## 1. The four bounded-depth mechanisms, and the ordering invariant

Four mechanisms together bound how far client prediction may diverge from the authoritative server.
They are easy to conflate, so the 4-way interaction is spelled out (`proposal_ogbrawler_netcode.md`
§4.3):

* **Stall** — the client is too far AHEAD of the server tick, and pauses one sim sub-step to let the
  server catch up. Long-standing behaviour; bounded stalls are preferred over unbounded rollback
  under cellular packet-loss bursts.
* **Skip** — the client is BEHIND, and advances several ticks in one display frame to catch up.
  Long-standing behaviour, kept as a binary catch-up.
* **RollbackWindow** — the SOFT cap on client prediction/resim depth, `rollbackWindowTicks`,
  derived from the Quantum formula on OGBrawler's cellular profile. §2 and
  §3 are entirely about what this does and does not do. Degraded mobile may in principle raise it as
  far as `rollbackWindowHardCap`; §2 records that nothing does so today.
* **HardResync** — the ABSOLUTE FAILSAFE BACKSTOP, `hardResyncThresholdTicks`. The legacy drift
  threshold, repurposed: it fires only when the soft cap has failed and the client is further adrift
  than `rollbackWindowHardCap` — off-the-rails packet loss, a multi-second freeze, a dev pause. It
  snaps the clock and wipes the cache, and is expected very rarely.

**Ordering invariant:** `hardResyncThresholdTicks > rollbackWindowHardCap`, so the failsafe always
fires strictly LATER than the soft cap — clamp before snap. `TimeConfigOrderingTest.cpp` asserts the
strict inequality, and `TimeConfigTierArrayOrderingTest.cpp` separately asserts that the top tier
ceiling cannot escalate past `rollbackWindowHardCap` either.

## 2. The RollbackWindow CLAMP: intended, never implemented

The ADR sentence *"the client clamps to the window and accepts a partial resim"* describes an
INTENDED mechanism that does not exist in the shipped code. Established by an exhaustive code trace;
the ruling is recorded in `RelayDelaySpectrumDesign.md` §7. As of that ruling:

- `SimulationReconciliation` contained no window or clamp logic. The resim span is *"newest landed
  correction -> prediction frontier"*, and depth was bounded only implicitly, by correction-ring
  capacity.
- The only production consumer of `rollbackWindowTicks` was the SERVER-side late-input future-guard
  context (`SimulationManager.h`) — a different mechanism from the client clamp.
- Client-side, `rollbackWindowHardCap` appeared solely as a log gate (`CorrectionCache.h`). It gated
  a diagnostic; it clamped nothing.

The Stall / Skip / HardResync mechanisms DO describe shipped behaviour; only the RollbackWindow
clamp was aspirational. ⚠ **The middle bullet is now out of date and §3 supersedes it** — that is
where the change is recorded, and these bullets are kept as the derivation the deferral was ruled
on rather than rewritten under it.

Building the clamp is genuine DESIGN work, not wiring: the site (naturally the resim-anchor
selection) and the partial-resim semantics both have to be specified. It was deliberately DEFERRED
to the sparse-state increment, where corrections stop landing every tick and deep resims become
real. While state replicated every frame the need was theoretical — resim depth stayed at about
client lead + downlink, comfortably under every configured ceiling.

### 2.1 The deferral's trigger fired, and the trigger was re-scoped rather than re-deferred

The sparse-state increment shipped: `SimulationNetSync::sendCorrectionAll` now writes
`correctionRotationK` characters' state buffers per tick, round-robin, so corrections stopped
landing every tick (§8). The condition the deferral named is therefore satisfied.

⚠ **The arithmetic in this subsection is the K = 2 derivation as it was recorded.** K has since
been lowered (§8), and the re-derivation at the lower value has NOT been done here and is not
assumed.

**The ruling: the clamp stays deferred, and the trigger is re-scoped from a binary condition to a
measured bound.** The deferral named TWO clauses and only the first fired:

* The resim span really is anchored on the newest LANDED correction, so rotation really does
  lengthen it. ⚠ The mechanism by which that was originally derived — a newest-first walk back from
  the frontier over per-slot `m_isResimulated` / `m_containsCorrectTick` flags in
  `StateCorrectionCache::getLastResimulationTick`, waking at exactly
  `predictionTick - lastCorrectTick`, with the frontier's flag propagated forward by
  `pushPredictionTick` — **no longer exists.** There is no walk and no
  inheritance; the anchor is an explicitly SET pending tick (§3, and
  `docs/ResimGatePolicy-rationale.md` §1). The DEPTH CONCLUSION is unchanged, because the anchor
  still coalesces to the newest landed correction, so the span is still
  `frontier - newestLandedCorrection`. Kept and annotated rather than deleted, because it is the
  derivation the ruling was made on.
* That distance grows by the ROTATION AGE of the newest correction, bounded by construction at
  `ceil(N/K) - 1` ticks — 0 at two characters (K >= N is every-frame), 1 at three or four, 2 at six.
  Add the replication system's skip-and-accumulate tail, itself bounded by the 4.0-vs-1.0
  static-priority ratio to roughly 4 frames, and one lost correction at the measured ~1.1 % wire
  loss, and the worst modelled addition is ~6-9 ticks.
* Against what: `rollbackWindowTicks` and `rollbackWindowHardCap` both remained UNIMPLEMENTED for
  this purpose, while the bound that actually binds is `StateCorrectionCache::StateBufferSize` — 60
  slots. Typical depth is client lead + transit, about 5 ticks; the worst case is a high-RTT client
  (prediction offset ~13 at 150 ms) at six characters, about 20-23 ticks. That crosses both
  configured ceilings, which was NEW — but it is still under a third of the ring, so nothing is
  dropped, nothing is unbounded, and no new diagnostic fires. (`isAnomalousMiss` gates on a MISSED
  tick's distance from the frontier, a clock-offset quantity, not on resim depth.)

So the first clause fired and the second did not: corrections are sparse, deep resims are not yet
real. Re-scoped rather than re-deferred on the same words, because *"the sparse-state increment"* is
now a condition that has already happened and cannot fire again.

**THE RE-SCOPED TRIGGER — build the clamp when any of these becomes true:**

1. modelled worst-case resim depth exceeds about half of `StateCorrectionCache::StateBufferSize` —
   reached by raising the character count well above 8 at K = 2, or by driving K to 1;
2. a MEASURED resim-depth distribution (not a model) shows a p99 above `rollbackWindowHardCap`,
   which would make the existing log gate's companion assumption stale as well;
3. `rollbackWindowTicks` acquires a real client-side consumer for any other reason, at which point
   the clamp is wiring rather than design. ⚠⚠ **CONDITION 3 HAS FIRED — see §3.**

⚠ The direction of travel is AWAY from this, not toward it: the shipping plan's K = N step puts
state back at every-frame cadence and un-fires the trigger entirely, and the wire diet in between
does not touch cadence. The quantity to re-derive if that plan changes is `ceil(N/K)`, and nothing
else here depends on the design of those steps.

## 3. THE DEPTH-SKIP AMENDMENT — condition 3 fired, and what shipped is a SKIP, not a CLAMP

Full derivation: `design_task43_resim_gate_fix.md` §3 candidate D (b).

**`rollbackWindowTicks` now has a real client-side consumer.** The edge-triggered resim gate reads
it as the maximum admissible resim DEPTH: `SimulationReconciliation::checkDivergenceAll` excludes any
character whose pending resim anchor sits more than `rollbackWindowTicks` below that character's
prediction frontier, via `resimGate::isAnchorWithinDepthPolicy`. §2's third bullet — *"client-side
this value clamps nothing"* — is therefore FALSE for this field. It remains true for
`rollbackWindowHardCap` as a RESIM bound; §11 records that the surrounding sentence about that field
was nevertheless wrong for a different reason.

**It SKIPS, it does not CLAMP, and the clamp is still unbuilt.** The ADR bullet's *"clamps to the
window and accepts a partial resim"* is NOT what shipped, and the difference is deliberate rather
than partial. Restoring at `frontier - rollbackWindowTicks` would restore a mid-window slot that no
correction ever landed in, so the replay would re-integrate identical inputs from identical state and
reproduce the same prediction — **a guaranteed no-op costing a full physics-engine rewind.** A
too-deep anchor is therefore SKIPPED AND COUNTED (`ResimGateWindowSummary::deepAnchorExclusions`),
leaving recovery to a newer correction — which raises the anchor — or to the HardResync failsafe.

If the partial-resim clamp is ever genuinely wanted, its semantics still have to be specified. This
consumer does not pre-empt that design; it only removes the *"no client-side consumer"* half of the
deferral. Conditions 1 and 2 of §2.1 still read as they did.

**The consumer is dormant only at the compiled default.** It is consulted only under
`resimTriggerPolicy == OnDisagreement` (§6 for why the ceilings are policy-scoped, and for the
compiled-default-versus-shipped-configuration pair). On the shipped configuration the depth policy is
ACTIVE, so this consumer runs today; flipping the compiled default would activate it for a build with
no override too.

**Why the ceiling is scoped to one policy at all.** `resimGate::policyEnforcesDepthCeiling` returns
true only for `OnDisagreement`. The ceiling exists to bound the disagreement trigger's worst-case
depth. Under the legacy policy the trigger rate is already bounded by frontier-exact coincidence, and
applying the ceiling there would cost the behaviour-neutrality the edge-triggered gate was defined by
— it would DROP an anchor the legacy gate retries. The full argument lives at that predicate.

## 4. The RTT outlier gate — why there are five fields

The host application measures RTT at FRAME-START time: the sample is stamped with the time the frame
began, not the time the acknowledgement landed. (One adapter's binding: the sample is
`CurrentTime - OutLagTime[Index]` with `CurrentTime = FApp::GetCurrentTime()`.) A frame hitch delays
acknowledgement processing and that delay lands straight in the sample, so a loopback session with
5-10 ms emulated lag can report a ~1-SECOND RTT.

Before this gate, `updateRTT` believed such a sample unconditionally: `jitterMultiplier` doubled the
excursion into the prediction offset and `jitterSmoothingAlpha` made recovery take seconds. The host
hands the library an obviously-bad number; the estimator's job is to not believe it. The full
rationale, including why the gate is one-sided and what it costs, lives at the gate itself in
`NetworkTimeEstimator.cpp`.

The mechanism is NOT a clamp: an implausible sample is REJECTED — it moves neither EMA — matching
the validity gate's doctrine in the same function. `rttOutlierConsecutiveLimit` is what stops that
being a bug: a filter that never lets the estimate move cannot follow a GENUINE step change in
network conditions, so after a run of consecutive implausible samples the estimator concludes the
level really has moved and re-seeds. A single hitch is isolated and never reaches the limit; a real
step is sustained and always does. About 30 samples is about 0.5 s at og-sim-net's clock cadence (at
most one `ClockPublish` per server frame, `SimNetServer::flushClock`; 60 Hz in the shipped game; the
deleted timing relay's was given as 100 Hz) — longer than a hitch's inflated-acknowledgement backlog, short enough that a real step is absorbed in well
under a second.

**Why the multiplicative bound is loose and the additive one exists.** 4x is deliberately loose: this
gate is aimed at 100x frame-hitch artifacts, not at trimming ordinary jitter, and anything under 4x
the current estimate is inside what `jitterMultiplier * smoothedJitter` already covers. Without the
additive half the multiplicative term collapses on LAN — at a 0.5 ms smoothed RTT, 4x is 2 ms, which
would reject an entirely ordinary 10 ms reading. 30 ms sits above any plausible loopback/LAN
excursion and far below the ~1 s artifacts being filtered.

**Why the cold-start ceiling is absolute rather than relative.** The first sample above 0 seeds the
smoothed RTT VERBATIM (a 0 does not seed, og-input-latency P5-28: §14's "Why a 0 reading does not
count"), so the relative bound has nothing to compare against. That seed is the single most
damaging value in the estimator — it is latched, and every subsequent jitter delta is measured
against it — so it is gated by an absolute ceiling instead, chosen over accept-then-correct. A
genuinely slow link is not locked out: `rttOutlierConsecutiveLimit` re-seeds it after a short run of
consistent readings, so the ceiling costs a fraction of a second on such links and rejects a
hitch-inflated first reading outright.

**Why rejections are counted and summarised.** A silent reject hides a genuine RTT step change
exactly as well as it hides a hitch artifact, so rejections are counted per window and summarised in
ONE log line per window that contains any. The window is sized to bound log volume at roughly one
line per 10 s at that 60 Hz clock cadence (600 samples, `rttOutlierLogWindowSamples`) even if the ping
source misbehaves continuously. Setting it to
0 disables the window summary; the running totals stay available through the estimator's accessors
either way.

## 5. The prediction-offset floor — the LAN corner case it closes

`predOffsetFloorTicks` guarantees the client's prediction target sits at least that far ahead of the
last known authority tick, keeping the dead-band lower bound at or above `authorityTick + 1` and
preserving the *"client predicts forward"* invariant on LAN.

Without the floor, sub-millisecond RTT rounds the prediction offset to 0-1 ticks and the softDrift
dead band locks the client at or behind authority in perpetuity. That is the LAN late-connect corner
case documented in the hit-resolution initiative's prediction-offset-floor finding note.

On WAN and cellular with real RTT the estimator's natural `rawOffset` already exceeds the floor and
the floor is a no-op. Verified at the time: at 50 ms RTT + 5 ms jitter `rawOffset` is 3.6, `ceil` 4,
equal to the floor; at 150 ms cellular it is 12.6, `ceil` 13, and the floor is irrelevant.

The floor is ALSO what the no-RTT-sample path of `NetworkTimeEstimator::getPredictionOffsetTicks()`
returns. It is a structural invariant guard, not an estimate, so it holds whether or not an RTT
estimate exists yet.

## 6. The resim trigger policy — the dormant ship, the measurement, and the sequencing

`resimTriggerPolicy` is the whole configurable surface of the edge-triggered resim gate. The gate
itself — a landed correction sets a per-character pending resim anchor tick, and the
resim-completion edge consumes it with a compare-and-swap — is derived in
`docs/ResimGatePolicy-rationale.md`; this section covers only the knob.

⛔ **The compiled default and the shipped configuration differ, and the header states that pair in
exactly ONE place: at the declaration of `TimeConfig::resimTriggerPolicy`.** Seven sites elsewhere in
the tree point at that declaration rather than restate the pair. **This file deliberately does not
author a second wording of it** — read the header, and then read the configuration key.

The gate shipped DORMANT at the compiled-default level, reproducing the gate it replaces, so a build
with no override observes no behaviour change. Under today's degenerately always-false divergence
verdict, *"disagrees"* fires on nearly every landing — measured at 86-87 % of physics frames, at
6.35x / 4.18x integration cost, superseding an earlier 3-6x model.

That is a VERDICT defect, not a gate defect. The verdict work is sequenced as a COST REDUCTION for a
trigger that ALREADY SHIPS, not as a blocker preventing it from shipping, and the compiled default's
own flip is SEQUENCED AFTER that work rather than blocked by it (ruled 2026-08-12) — a sequencing
that governs only what a build with no override runs.

⛔ **Do not flip the compiled default early "to see what happens."** The fix-preview measurement arm
(`design_task43_resim_gate_fix.md` §2.3) is the safe way to ask that question. The full argument,
with the sequencing record and its dates, is in `docs/ResimGatePolicy-rationale.md` §3 point 3, §5
and §10.

**Client-side only in effect.** The gate is consulted from
`SimulationReconciliation::checkDivergenceAll` on a predicting client, and an authority allocates no
correction caches at all. The value is still read and applied on both roles so the two `TimeConfig`
instances stay identical — the composition root's intake is role-agnostic — and on a server it simply
has no reader.

**There is no `resimCooldownTicks`, and its absence is a ruling.** That fence is kept VERBATIM in the
header, immediately below the declaration, because a reader who is about to re-introduce a cooldown
has no symbol to grep for and no document to be sent to. It is not summarised here; read it there.

## 7. The relay delay floor — spectrum, sizing, composition, and the derived clamp

`relayDelayFloorTicks` is the session-scoped minimum effective Layer-1 input delay, in ticks.
(`RelayDelaySpectrumDesign.md` §3, §6, §10, §11 Q1/Q2/Q5.)

**What it buys.** A peer can only simulate a relayed input AT its scheduled tick if the input reached
that peer before the peer's frontier got there. The trip left to cover is a property of the RECEIVER
(§3.2 of that design note):

    D >= lead_B + downlink_B  ~=  RTT_B (+ jitter/wobble margin)

The per-connection tier derives its delay from the SENDER's wire, which is the wrong variable for
receiver coverage — hence this separate, session-wide quantity. Raising it moves the whole session
along the spectrum from *"extrapolate + correct"* toward *"everyone applies the same input on the
same tick"*; the price is that EVERY player's own felt input lag rises to at least this many ticks.
That is why the knob defaults to 0 and is tuned by playtest, not by intuition.

**Sizing guide** at 60 Hz (design §3.3; m = jitter + frontier-wobble margin, about 2-3):

    cover-to-RTT:  30 ms -> >= 2+m    80 ms -> >= 5+m    150 ms -> >= 9+m

so *"the floor covers decent connections"* lands around 7-8. **No preset ships:** exact values wait
on the relay-cadence probe and playtest (design §11 Q2).

**How it composes.** It is a MAX, never a sum, and it is applied at every site that derives an
effective input delay, through the ONE shared helper `applyRelayDelayFloor`
(`Network/ConnectionTierTable.h`):

    effective = max(relayDelayFloorTicks, <tier-or-fallback value>)

A nonzero floor therefore DOMINATES `lanZeroDelayOverride` as well — see §9 and that field.

**Uniform-D fairness mode** (design §11 Q5) is a config VALUE, not a feature: once
`relayDelayFloorTicks >= max(rttTierInputDelays)` every derivation path — tier, LAN override, and the
no-tier fallback `rttTierInputDelays[kMaxConnectionTierIndex]` — collapses to exactly this value, so
every sender is scheduled with the same D and no player is advantaged by their connection. The
shipped configuration reaches that condition. It is otherwise invisible in the logs, which is why
`classifyRelayDelayFloor` exists: it surfaces the regime at startup.

**The clamp, and why the ceiling is DERIVED rather than a literal.** Effective values are capped at
`relayDelayFloorHardCapTicks`, which is `kLocalInputCacheCapacityTicks - rollbackWindowHardCap`
(computed by `relayDelayFloorHardCapForCapacities`, so a test can prove the cap follows the store
capacity down). Beyond that cap the client's own capture for the scheduled tick has already been
evicted from `LocalInputCache` before it can be consumed, and the whole scheduled regime silently
degenerates into permanent fallback reads instead of failing loudly. The clamp is enforced at BOTH
intake points — the configuration override at the composition root and the client's floor
receipt — and once more on every read inside `applyRelayDelayFloor`. That
belt-and-braces shape is the same one `clampConnectionTierIndex` uses for the replicated tier.

**Source of truth and distribution.** The SERVER owns the value, optionally overridden from the
`RelayDelayFloorTicks` key under `[OGNetcode]` at the composition root, and sends it to every client
as a `RelayFloor` message on og-net's Control lane (`SimNetServer::publishFloor`, and again to a client
that attaches later) — session-scoped state on the session's control channel. (Until og-input-latency
task P5-7 it was a uint8 replicated property, a `UPROPERTY`, on the session-scoped timing relay,
`ASimulationTimingRelay`, deleted by P5-9.) Clients never derive it locally; a client's copy of the
field is written from that message's receipt (`SimNetClient::onFloor`, then
`UESimulationSession::onRelayDelayFloorReceived`). The deferred dynamic-floor policy (design §11 Q6) needs no new mechanism: it
writes this field and the property again.

**This field owns the hiccup-absorption rationale** that the retired no-tier-baseline field used to
carry: deliberately trading a small constant input lag for the ability to absorb short network
hiccups without a visible re-simulation pop. That rationale describes THIS field, not the no-tier
fallback — the fallback is a policy choice about which tier to assume before one is known, not a
hiccup-absorption mechanism in its own right.

**The retired ring-retention knob** (design §11 Q3) was deliberately independent of this one: the
floor sets how far ahead inputs are scheduled, that depth set how much history survived a replication
gap. Its retirement fence is kept VERBATIM in the header for the same reason as §6's — a reader about
to re-introduce it has no symbol to grep.

## 8. The state cadence lever K

`correctionRotationK` is how many characters' correction-state buffers
`SimulationNetSync::sendCorrectionAll` writes per tick, round-robin.
(`design_task38_input_first_replication.md` §5.4, §6 candidate A, §13.2.)

**What it buys.** The correction state is the LARGE payload — 311 B on the wire per character per
tick — and the SELF-HEALING one: a missed snapshot costs correction latency only, because every
snapshot is a complete anchor and the client always reconciles against the newest landed one (design
§2.1). The relay ring is the small, IRREPLACEABLE payload — a dropped relayed input has no recovery
path anywhere (design §2.2). Before the split, both shared one atomic replication batch, so packet
overflow killed them together. The split ranks the ring above the state; THIS knob is the other half:
instead of writing every character's state every tick and letting the packet decide who loses, the
write site rotates through the characters at a decided cadence.

**The arithmetic.** With N registered authority writers, each character's state replicates at
`tickFrequency * K / N` Hz. At K = 1 that is 30/20/15 Hz at N = 2/3/4; at K = 2 it would be 60/40/30
Hz and 20 Hz at N = 6. A character not written is not dirty and costs ZERO bytes — the replication
system rolls back headers of clean objects — so the saving is real wire bytes, not a deferred write.

**Why the compiled default was lowered, and why a port must re-check it.** The reason is a specific
property of ONE replication implementation, not a byte budget. On the adapter this was measured on,
an object batch that fails to fit is routed into a chunked, reliable-attachment-backed delivery path
that BLOCKS that object's newer snapshots until acknowledged — but only when the failure leaves MORE
than a split threshold (1,536 bits, about 192 B) of space free; a failure below that threshold is
cleanly aborted and the state simply ships in packet 2 of the same tick. (One adapter's binding:
`FReplicationWriter::HandleObjectBatchFailure` -> `SplitHugeObject`, threshold
`GetBitCountSplitThreshold`, clean-abort result `Abort`.)

With un-dieted 316 B states and bare C1's VARIABLE-length rings, K = 2 at four characters puts the
second state's failure at about 270-312 B remaining — INSIDE that blocking window, on roughly a third
of frames. At K = 1 and N <= 4 the round fits outright on average and on correlated-p99 frames, and
the residual failures land BELOW the window. Cadence at K = 1 is the floor of the accepted 15-20 Hz
band, deliberately. The lowering is for the duration of the pre-diet window only: the wire diet
restores 2 in the same change that deletes `kPreDietCharacterCap` (design §16.2).

**The cost, stated.** At TWO characters K = 2 was every-frame, i.e. bit-identical to the pre-rotation
cadence, which is what kept the archived 2-character baselines comparable. At K = 1 a two-character
session corrects at half that, so a correction-cadence comparison against those baselines must expect
exactly half — designed, not a regression. Do NOT special-case K by character count.

**It interacts with a documented premise.** The retirement block at
`SimulationNetSync::sendCorrectionAll` records that the own-character input-echo drop is safe BECAUSE
corrections ship every frame, and names sparse state as the one thing that would re-open it. This
knob is that thing, deliberately and with the trade priced (design §2.3, §13.1): input delivery rises
far more than repair latency lengthens. Read that block before tightening K further.

**Session-scoped, server-only, not replicated** — like the retired ring depth and unlike the delay
floor. Only the authority runs `sendCorrectionAll`, so a client copy would have no reader, and
nothing about the cadence needs to ride the wire because a receiver just reconciles against whatever
corrections arrive. The composition root reads the `CorrectionRotationK` key under `[OGNetcode]` on
the SERVER and pushes it through `SimulationManager::setCorrectionRotationK`.

**The clamp.** [1, 16], by ONE shared idempotent guard `correctionRotation::clampK`, called at the
configuration intake, at the setter, and once more inside the selection predicate. 0 and negatives
clamp UP to 1: a K of 0 is not *"off"*, it is a correction channel that never publishes, i.e. a
permanent desync. The ceiling only has to stop a typo becoming nonsense — K >= N is the legitimate
every-frame setting.

## 9. The tier table, and the runtime tick rate

**The RTT tier is SERVER-AUTHORITATIVE** (decided 2026-07-19). The server derives each connection's
tier from its own per-connection RTT sample and replicates the tier index to the owning client. The
client does NOT compute its own tier — it consumes the replicated value. This preserves the
codebase's existing single-source-of-truth pattern (the server owns the authority tick and the client
derives from it) rather than introducing a second, independently-drifting estimator on the client.

All four tier arrays are indexed by the SAME tier index 0..3, so entry N of every array describes the
same connection-quality bucket. Keeping them as parallel arrays rather than an array-of-struct
matches the `proposal_ogbrawler_netcode.md` §11 appendix layout and keeps each row independently
tunable from configuration.

**Amended 2026-08-03** (`RelayDelaySpectrumDesign.md` §6/§9): every effective input delay derived
from the tier table is FLOORED by `relayDelayFloorTicks` — `max(floor, tier-or-fallback)` — through
the single shared helper `applyRelayDelayFloor`. The tier remains the per-wire quantity; the floor is
the session-wide receiver-coverage minimum. At a floor of 0 the max is the identity and everything in
this section is unchanged — but 0 is the COMPILED DEFAULT, not the shipped configuration (§7).

**The boundaries.** `rttTierBoundariesMs` holds inclusive UPPER bounds, so entry N is the highest
smoothed RTT that still counts as tier N. The final entry is a SENTINEL chosen far above any playable
RTT: a connection worse than the top tier has no worse tier to escalate into, so the top tier must be
an open-ended catch-all. A non-monotonic table would make the tier lookup order-dependent and could
strand a connection in a tier it can never leave, which is why
`TimeConfigTierArrayOrderingTest.cpp` asserts strict increase.

**The delays, and the no-tier fallback.** `rttTierInputDelays` IS the effective input delay once a
tier is known, subject to the session floor, applied inside `tierInputDelayTicks`. Worse tiers buy
more delay, which hides more of the network round-trip behind local input latency. Its LAST entry is
also read by `ServerInputDelayQueue::effectiveDelay` and
`ReplicatedTierConsumer::effectiveInputDelayTicks` whenever no per-connection tier is available yet.
That is exactly why monotonic non-decrease matters beyond the escalation argument: it is what
guarantees the last entry IS the worst, pessimistic delay rather than an arbitrary one. The full
argument for choosing the worst tier over the best tier or a bare floor is kept VERBATIM in the
header's no-tier retirement fence.

**Tier-flap mitigation.** `tierHysteresisMs` is a directional dead-band around each boundary: a
connection promotes only when smoothed RTT exceeds boundary + the band, and demotes only when it
falls below boundary - the band. Without it, an RTT hovering exactly on a boundary would flap between
two tiers every sample, and each flip changes the effective input delay — which the player feels
directly as stuttering control latency. `tierMinDwellTicks` is the companion: the hysteresis band
alone stops boundary-noise flapping but not a genuinely oscillating connection, so a dwell floor
bounds how often the player-visible input delay is allowed to change at all.

**The LAN override.** `lanZeroDelayOverride` gives a top-of-table connection ZERO input delay instead
of its tier's: on a sub-millisecond local link there is no round-trip to hide, so any forced delay is
pure added input lag with no benefit. Only tier 0 is affected — a bad connection on a LAN session
still gets its tier's delay. It is dominated by a nonzero floor because the floor is applied AFTER
this branch (design §6), and that is the correct precedence, not a conflict: on a pure-LAN session
the floor is configured 0 and the override behaves exactly as before, while on a MIXED session a LAN
sender must still be schedulable by WAN receivers — and a sender applying its own input at capture+0
while its peers schedule it at capture+floor is precisely the two-ends-disagree bug the floor exists
to prevent.

**The runtime tick rate is not set here.** `redundancyDepthTicks` tracks it — 3 at 60 Hz, 5 at 100 Hz
— but the tick rate itself is the host application's fixed physics timestep, configured outside this
library. (One adapter's binding: the `AsyncFixedTimeStepSize` key in `Config/DefaultEngine.ini`.)
`tickFrequency` is set at construction from that same timestep, as
`config.tickFrequency = 1.0 / asyncDeltaTime`. (One adapter's binding: that delta comes from the
physics solver's `GetAsyncDeltaTime()`.)

## 10. Fields with no runtime consumer — the configurability rule

Every constant the design names must exist as a `TimeConfig` field even before its consumer ships. If
the field did not exist here, the consuming work would inevitably hardcode the literal at its use
site and the configurability lint could not catch it — which is exactly the second-source-of-truth
failure the rule exists to prevent. It is also why an apparently unread field is **not dead code to
delete**.

There are three distinct shapes of "no consumer" in this header, and the distinction matters because
only one of them is completely inert:

| shape | what is true | fields |
|---|---|---|
| **no reader at all** | nothing in this repository names the field outside the defaults test | `harnessMode`, `sn1BroadcastPolicy`, `sn1IdleBroadcastIntervalTicks`, `hashBroadcastPolicy`, `hashBroadcastIntervalTicks`, `hashMismatchReaction`, `sparseSaveMode`, the four `record*` toggles, `aggregateSiblingInputBundles`, `hashLogRingCapacity` |
| **read by a library helper that has no production caller** | the field is genuinely read, but only from a function the Catch2 suite alone calls | `rttTierRollbackCeilings` (via `tierRollbackCeiling` / `lookupRollbackCeiling` in `Network/ConnectionTierTable.h` and
`effectiveRollbackCeiling` in `Network/ReplicatedTierConsumer.h`), `muteEchoOnDegradedTier` (via `tierShouldMuteEcho` and the two `shouldMuteEcho` members), `hashMismatchTickThreshold` (via `shouldEscalateToLayer2`) |
| **read in production** | everything else in the header | — |

`redundancyFrames` and `relayResendEntries` (§12) are in the last row, not the first. The client send reads
`redundancyFrames`, as the third argument of
`SimulationNetSync::sendLocalInputToAuthorityAll`. `relayResendEntries` has no reader in this repository, only its
setter: the host that owns the relay flush reads it and passes the count to `relayedInputRing::flushStagedInto`
(og-input-latency task 8). `backDateM0P50ThresholdTicks` (§13) is in the last row too, since og-input-latency P3-6:
`ServerReceptionCoordinator`'s constructor passes it to its `InputArrivalMargin`.

⛔ **The middle row is the one that has caused trouble.** A field in it looks alive to a grep and dead
to a debugger, and the header used to describe two of them wrongly in opposite directions — see §11.
State which of the three shapes applies; do not write "no consumer" unqualified.

For the whole observability section the consumer arrives with a separate observability work package
tracked outside this repository. `hashMismatchTickThreshold` and `hashMismatchReaction` are the two
that are pre-allowed by name in `tools/lint/configurability_lint.ps1` for their sibling-header layout,
and `DesyncDiagnosticSink.h` — whose sink boundary is `IDesyncDiagnosticSink`, today implemented
only by `LogOnlyDesyncDiagnosticSink` — states at the helper itself that `shouldEscalateToLayer2`
deliberately
does not read the reaction: that helper answers WHETHER to escalate, while the reaction selects WHAT
to do about a confirmed divergence, which is the consumer's policy call.

## 11. Corrections this extraction made, and what was verified before compressing

Rule R0 of the extraction initiative: **do not compress a claim you have not checked** — compression
makes a false statement shorter, denser and more authoritative, and every existence check passes it.
Every factual claim in the header was verified against the tree before being rewritten. Six were
wrong; all six are corrected in the header, and recorded here so the corrections are not silent.

| # | the claim, as it stood | what the tree says |
|---|---|---|
| **1** | `rollbackWindowHardCap`: *"Client-side this value is read only as a LOG GATE (`CorrectionCache.h`) and as the deliberate flat release gate beside it"* | **FALSE, and the same file contradicted it.** `relayDelayFloorHardCapTicks` (`Network/ConnectionTierTable.h`) reads it to derive the relay-delay-floor ceiling, and `clampRelayDelayFloorTicks` calls that at both intakes — the client's floor replication callback included — and again inside `applyRelayDelayFloor`. So it IS read client-side and it IS a clamp. `relayDelayFloorTicks`' own comment, further down the same header, already documented that read. `Network/ServerReceptionCoordinator.h` reads it at three more, server-side, sites. |
| **2** | `hashMismatchReaction`: *"Consumed by the desync sink boundary."* | **FALSE.** Nothing outside `TimeConfigDefaultsTest.cpp` names the field, and `DesyncDiagnosticSink.h` states in terms that `shouldEscalateToLayer2` deliberately does not read it and that it *"has no consumer"* yet. Two files in one library flatly disagreed, and this header's was the wrong one. |
| **3** | `muteEchoOnDegradedTier`: *"NO CONSUMER YET … Nothing reads this value today"* | **FALSE as written.** `tierShouldMuteEcho`, `ConnectionTierTable::shouldMuteEcho` and `ReplicatedTierConsumer::shouldMuteEcho` all read it. What is true is that none of those has a production caller — the accurate phrasing is the one `ConnectionTierTable.h` already uses for the rollback ceiling, **NO PRODUCTION CALLER**. |
| **4** | `hardResyncThresholdTicks`: *"This backstop IS live and, today, the only shipped bound of the three."* | **STALE since the depth-skip amendment.** `rollbackWindowTicks` bounds resim depth today, by SKIPPING (§3). What is unbuilt is the partial-resim CLAMP, not every bound. Third self-contradiction found in this file across the initiative. |
| **5** | `harnessMode`: no consumer statement at all | **It has no reader anywhere in this tree** and never said so, while eight neighbours that also have none did. Stated now. |
| **6** | the observability banner: *"No field below repeats that; they inherit it from here."* | **The banner was false about its own section** — eight fields below it repeated the sentence verbatim. Resolved in the direction the banner asks for: the repetitions are gone, so the banner is now true. |

⚠ **Two further claims are true but were checked and are worth recording as checked**, because they
are the kind that rot silently: `StateCorrectionCache::StateBufferSize` is 60 and
`kLocalInputCacheCapacityTicks` is 64, so §7's derived cap is 44 at current defaults; and the ordering
invariants named in §1 and §9 are asserted by `TimeConfigOrderingTest.cpp` and
`TimeConfigTierArrayOrderingTest.cpp` respectively, both of which exist and assert exactly what the header
now says they do.

⚠ **One routed observation, not fixed here because it is not this header's file.** The
`[OGNetcode]` section's own commentary names the deep-anchor counter `deepAnchorSkips`, while the
member is `ResimGateWindowSummary::deepAnchorExclusions`. That is the wire-name-versus-member class
and was first routed by an earlier task; it is repeated here because §3 depends on the member name.

⛔ **What did NOT move, and why.** Three fences in the header are ABSENCE fences — *there is no
`resimCooldownTicks`*, *there is deliberately no relay-ring retention-depth field*, *there is
deliberately no dedicated no-tier-baseline field*. Their entire load-bearing power is positional: a
reader about to re-introduce one of those knobs has no symbol to grep for and no document to be sent
to, because they are about to *add* the line, not read it. They stay verbatim at the site, and this
file deliberately does not paraphrase them.

## 12. The latency knobs: redundancy in frames, and the relay re-send

Two fields added by og-input-latency task 4, 2026-11-11. **Both compile to today's behaviour.** `redundancyFrames`
is read by the client send since og-input-latency task 7 (the third argument of
`SimulationNetSync::sendLocalInputToAuthorityAll`; the arithmetic is in `SimulationNetSync-rationale.md` §17).
`relayResendEntries` is read by the server's relay flush since og-input-latency task 8: the host that owns the
flush reads it, so neither field is in §10's "no reader at all" row.

**`redundancyFrames`: the redundancy window counted in frames.** `redundancyDepthTicks` counts the window in
*ticks*. A client that steps several ticks per frame then re-sends fewer of its frames than a client that steps one,
so a single lost send can lose more than one tick of input. The consumer sizes the bundle as about
`redundancyFrames` frames' worth of ticks, never fewer than `redundancyDepthTicks` slots (or `kMaxSlots`, when the
depth is larger) and never more than the bundle's wire bound `inputRedundancyBundle::kMaxSlots`. **0 is "today", not "off":** at 0 the bundle keeps exactly
`redundancyDepthTicks` slots, so the compiled default changes nothing.

**`relayResendEntries`: already-flushed relay entries re-included in each flush.** This is the successor the relay
ring's retirement block (in the header, above the field) names: "already-sent ticks re-included in a flush", a
different quantity from "entries retained by a write path", under a different name. ⛔ **It is not a depth.** The
flush stage's capacity stays `relayedInputRing::kMaxDepth`, and `relayedInputRing::stageArrival` takes no depth
parameter. Wiring this field (or any knob) into that capacity is the trap the retirement block records. At 0 the
flush is today's: nothing already flushed is sent again.

**The clamps.** Each is clamped by ONE shared idempotent function, `latencyKnobs::clampRedundancyFrames` and
`latencyKnobs::clampRelayResendEntries`, called at the configuration intake and again at the manager's setter
(`SimulationManager::setRedundancyFrames`, `SimulationManager::setRelayResendEntries`): the shape
`correctionRotation::clampK` set for `correctionRotationK` (§8). Negatives clamp up to 0, which is today in both.
* `redundancyFrames` is clamped to [0, `latencyKnobs::kMaxRedundancyFrames`], and that ceiling equals
  `inputRedundancyBundle::kMaxSlots`. The consumer counts every frame as at least one tick, so a frame count
  above the slot bound can only ask for more slots than the bundle holds, and the consumer's own slot ceiling would
  cut it back. The ceiling therefore loses nothing. `TimeConfigDefaultsTest.cpp` asserts the equality at compile
  time, so a change to the bundle's bound fails the test build until the ceiling is revisited.
* `relayResendEntries` is clamped to [0, `latencyKnobs::kMaxRelayResendEntries`], which is 2: og-input-latency's
  design (§6.10) states that range, and its task 8 keeps a "last flushed" tail of at most two entries.

Two `static_assert`s below the struct check that each compiled default lies inside its own clamp, so the setter can
never move a value nobody configured.

**The configuration intake.** The composition root reads `RedundancyFrames` and `RelayResendEntries` under
`[OGNetcode]` **on both roles**, once per session, and pushes them through the two setters. That follows the
orientation rule that both roles carry the same struct: `redundancyFrames` is read only by a client's send and
`relayResendEntries` only by the authority's flush, and each simply has no reader on the other role. Its start-up
proof line is `[LatencyKnobs] redundancyFrames=… relayResendEntries=… (ini|compiled)`, at `Warning`, with the values
read back from the session's `TimeConfig`. The trailing token is `ini` when either key was present in the ini and
`compiled` when neither was. (One adapter's binding: og-sim-session's `readSessionKnobs` and `applyLatencyKnobs`
(`SessionKnobs.h`, since og-session-runtime task 23; `UESimulationSession`'s members before), which the Unreal session calls,
and the keys in the brawler's `Config/DefaultEngine.ini`.)

## 13. Back-dating detection: the m0 p50 threshold

One field added by og-input-latency P3-1, 2026-11-12: `backDateM0P50ThresholdTicks`. Its reader is the back-dating
detector, og-input-latency P3-6: `ServerReceptionCoordinator`'s constructor passes it to its `InputArrivalMargin`,
which compares each wire's window m0 p50 with it at each `[InputArrival.Window]` close (`InputArrivalMargin-rationale.md`
§14). It changes only what is logged and counted. It has no configuration key: the composition root does not
override it.

**What it detects.** A client could stamp its captures older than they are, so they seem to arrive earlier and dodge
the input delay. Rewriting a tick it has already sent gains nothing, because the server's queue keeps the first copy of
a tick (`ServerInputDelayQueue::enqueue` returns false on a duplicate capture tick). A gain therefore needs a constant
shift, holding every input back k ticks. That lowers m0, the arrival margin without the delay
(`ArrivalMarginSignal::m0P50Ticks`; `InputArrivalMargin-rationale.md` §9 defines it), by k.

**The honest band.** All measured on one PC, with in-order emulation and D = 6:
- an own-input-margin measurement taken before m0 was logged gave m0 p5 of 0 to 2;
- the first real-host run of the m0 line (2026-11-12, a 2-minute headless session, two clients at a time, one replaced mid-session) gave m0 p05 −1
  and p50 0 in steady state, and p50 values of −3 and −2 in a wire's first windows after a join;
- the clock's dead band (`softDriftThresholdTicks`) lets a client's lead sit up to 3 ticks below its target with no
  correction, so an honest m0 can reach about −3. This is inferred, not measured;
- with a static delay floor, the only delay change in a session is the join's stall from the no-tier fallback up to
  the floor, and it falls inside the join grace below.

**The rule P3-6 built.** Per wire, from its 6th window on (the first five, 10 s at the 2 s window, are join
grace): a window with at least 60 samples and an m0 p50 below the threshold counts. Five such windows in a row open an
episode, logged once as `[InputArrival.BackDate]` (the `Net` route; `SimulationLogRoutes-rationale.md` §5) and counted.
The first window with samples at or above the threshold closes it. Nothing is refused, delayed or flagged to the
client.

**The trade.** −5 sits 2 ticks below the lowest honest value above (−3), so an honest session should not open an
episode. The cost is that a shift of 5 ticks or less goes unseen: an honest p50 near 0, shifted by 5, is still not
below −5. Because the detector only reports, a false positive costs one log line and a count. The threshold can be
tightened only once the clock holds m0 close to its target, which is time-dilation work; that needs a new decision.

**The `static_assert` below the struct** checks that the compiled default is below 0. An honest m0 p50 sits at about 0
in steady state, and the dead band lets it fall a few ticks below, so a threshold at or above 0 would flag honest
sessions. The check replaces a comment; no tag is needed.

## 14. The one-shot re-centre: `recentreSettleSamples`

One field added by og-input-latency task 27, 2026-11-18: `recentreSettleSamples`. Its reader is `ClientPredictionClock`.
Since og-input-latency P3-11 it counts the clock's own advances on which the estimator had a reading
(`NetworkTimeEstimator::hasFirstRTTSample`), a window of ticks; until then it counted samples with
`NetworkTimeEstimator::getMeasuredRTTSampleCount`, the running total of samples that passed `updateRTT`'s validity gate
(plausible or not) **and read above 0**, which the decision's line still reports. See "The window is time" below. The
composition root may override it with the `RecentreSettleSamples`
key under `[OGNetcode]`, read on both roles (one adapter's binding: og-sim-session's `readSessionKnobs` and
`applyLatencyKnobs`, `UESimulationSession`'s members until og-session-runtime task 23, with the start-up line `[ClockRecentre.Config] recentreSettleSamples=… (ini|compiled)`). The key
exists so the A arm (0) runs on the same binary as the B arm.

**The problem it fixes: the dead band keeps a join's landing error for the session.** The clock corrects only a drift
larger than `softDriftThresholdTicks` (3). Whatever lead a join leaves inside that band stays until the next hard resync.
Two joins measured by og-input-latency left one:
- **The Ph3 landing** (task P5-25, the Ph3 gate, and its review). A joining client's first frame runs a catch-up batch of
  10 to 17 steps. The first step is the hard resync onto the target; the batch's remaining steps then run past it, with one
  graduated Stall per `gradualCorrectionRate` steps, and end 7 to 10 ticks above the target. Graduated Stalls walk the lead
  back down, and the walk stops at the dead band's edge, so the clients settled 2 to 3 ticks of drift above the target, a
  lead of 6.3 to 7.1 ticks, about +58 ms of input latency against the Ph2 reference.
- **An inflated first RTT** (task P4-12's F10, fixed at its trigger by P4-13). A raised first target makes the client skip
  up to it; when the RTT falls back, the client is left above the new target, inside the band.

**The rule.** The re-centre is armed when the clock is constructed and again by every hard resync. Once the clock has made
`recentreSettleSamples` more advances with a reading (P3-11; measured samples before), that advance, if it is past the
warm-up guard, decides **once** how many ticks the lead is off, and logs one line:

`[Warning][Clock.Recentre] fromLead=<x> toLead=<y> owed=<n> target=<t> rttMs=<r> samples=<s> cause=<c>`, with
` fromTarget=<a> toTarget=<b>` appended when `cause` is `rearm`, then ` skipped=within1` when `owed` is 0. `cause` names
the arm the decision closes (since rework 2): `join` for the clock's first decision (the arm at construction, and a hard
resync that lands before any decision, as every measured join does), `resync` for a hard resync after a decision, and
`rearm` for the re-arm below.

**The decision reads a median, not one frame** (og-input-latency task 27, rework 1, after its review's finding F3). Every
advance of an armed re-centre past the warm-up guard stores one reading: the lead over the last authority tick that the
clock would have had if every advance since the arm had been Normal. The decision takes the median of the last
`ClientPredictionClock::kRecentreMedianAdvances` (15) readings, an odd count so that a full run's median is one reading
(a `static_assert` checks it). It moves the median to the current frontier by taking off the Skips and Stalls the clock
has made since the arm, and subtracts the current prediction offset; `owed` is the result, rounded half away from zero
(only a run shorter than 15 with an even count has a half). The readings are stored uncorrected so that the graduated
Stalls of a landing's walk down, which run inside the window, do not move the median. The median is of the lead over the
authority tick, not of the drift, and the offset is the decision's own: the median filters the authority tick's arrival
phase, which flickers frame to frame, and an RTT step inside the run counts in full. That mattered because the outlier
gate re-seeds on its `rttOutlierConsecutiveLimit`-th consecutive outlier, which with the default window is the sample that
fills it, and until og-input-latency P5-28 every join on a link above 30 ms re-seeded there (the estimator seeded on a 0,
see "Why a 0 reading does not count" below): a median of the drift owed 0 there and left the client two ticks low
(`PCTM.ClientPredictionClock.Recentre.ZeroReadingsDoNotFillTheWindow`). A real RTT step inside the run still does.

`fromLead` is `toLead + owed`, `toLead` the target minus the last authority tick (the prediction offset), `target` the
target tick, `rttMs` the smoothed RTT and `samples` the samples since the arm. A non-zero `owed` is a debt of exactly that
many graduated corrections, one per `gradualCorrectionRate` advances, **inside the dead band too**, in the debt's
direction (ahead: Stall; behind: Skip). Outside the dead band the drift sets the direction, as before. Every graduated
correction fired while a debt is owed pays one tick of it, so the debt ends after exactly `|owed|` corrections; an
opposite one can only come from outside the band, where the drift disagrees with the debt by more than the band, and
counting it too keeps the episode bounded. No later reading ends the debt; a hard resync or a re-arm clears it and
re-arms. Otherwise it does not fire again. A hard resync or an input-delay transition debt (Stalls, or Skips since og-input-latency P6-2) keeps its priority over it (hard
resync, then debt, then drift correction), and `evaluateDrift` reports the correction it owes.

**The re-arm: a target that moves after the decision** (og-input-latency task 27, rework 2, from the Ph3 gate's finding
R10). After a decision the clock compares the **RTT-only offset**, `max(predOffsetFloorTicks, ceil(smoothedRTT ×
tickFrequency))`, with its value at that decision, on every advance that is not a hard resync. That is the prediction
offset (`getPredictionOffsetTicks`, the quantity the target adds to the authority tick) without its jitter term, floored
as the prediction offset is; the clock computes it itself (`ClientPredictionClock::recentreRttOffsetTicks`, since rework
3; floored since rework 4), from its own `TimeConfig`, which is the estimator's. When the two differ by
`ClientPredictionClock::kRecentreRearmTicks` (2) or more on every one of the last `kRecentreMedianAdvances` (15) advances,
the re-centre re-arms: any outstanding debt is cleared, the settle window counts `recentreSettleSamples` advances again
from the next advance, the median run restarts, and the next decision is made as above, from the state at that moment.
The decision itself still aims at the full prediction offset; only the trigger leaves the jitter out. Both constants are
`constexpr` in the clock; there is no new `TimeConfig` field and no new ini key. The decision's line then carries
`cause=rearm fromTarget=<a> toTarget=<b>`: `a` is the RTT-only offset at the previous decision and `b` the RTT-only offset
on the advance that re-armed, both floored (the full offset is the line's `toLead`). There is no cap on re-arms. A hard
resync keeps its own re-arm unchanged. `evaluateDrift` reports no correction on the advance that re-arms, because the
re-arm clears the debt before the band is read.

The case it was built for: in the Ph3 gate's shaped rendered shape (b) (P5-25 rework 1), both client1s decided on the
start-up RTT spike. They logged `rttMs` 131.6 and 139.9 ms, offsets 8 and 9, about 1.2 s after the join. Their
averaged ping then fell one bucket per second, to 106–113 ms at 1.5–3 s and 78–87 ms from 5 s on, and the offset fell to
5–6. The dead band held the difference at +2 for the session. A replay of both RTT timelines
(`PCTM.ClientPredictionClock.Recentre.ThePh3bJoinsReArmWhenTheirStartUpRttSpikeSettles`) re-arms one client once
(RTT-only offset 8 to 6) and the other twice (9 to 7, then 7 to 5). It settles both with a median lead error of 0 and a
mean of +0.15 and −0.25 over the 10–30 s window. Before the re-arm the same replay settled at +2 and +4. On the RTT-only
offset each re-arm comes 12 to 14 samples sooner than on the full offset (rework 2's trigger), whose jitter term the
RTT's own fall raised; the decisions' `owed`, the Skips and Stalls and the settled readings are the same.

**Why the trigger is floored** (rework 4, from the Ph3 gate's finding R13). Unfloored (rework 3), an RTT that moves
entirely under the floor (`predOffsetFloorTicks`, 4, so a smoothed RTT under about 67 ms at 60 Hz) moved the trigger and
re-armed although the target did not move. The re-decision then measured the same lead again. On the Ph3 gate's loopback
shape (c) (P5-25 rework 2) six of the eight re-arms were of this kind (`toLead=4`, the RTT-only offset 4 to 2 or 3 to 1).
Four decided `skipped=within1`. Two, the main clients of one pair, decided `owed=-1` on a lead that flickered between 0
and −1 in the 15 readings, took one Skip, and sat at +1 for the session (p50 +1, means +0.61 and +0.75 over 10–30 s;
p50 0 in the 3 s before).
Floored as the target is, the trigger moves only when the target's RTT part does.
`PCTM.ClientPredictionClock.Recentre.ThePh3cMainClientDoesNotReArmUnderTheOffsetFloor` replays one of those two clients,
its first 900 samples as its log has them (the RTT the smoothed RTT implies, the authority ticks and the advances per
sample): on the unfloored trigger it reproduces all three logged decisions, the third `owed=-1`; floored, it decides
twice (at 89 and 52 ms) and settles at a median lead error of 0. The same replay of all ten (b) and (c) clients of that
capture (60 s each) removes the six under-floor re-arms and the two Skips they took, and changes no other decision or
correction; every (b) client's decisions, corrections and lead are identical. `PCTM.ClientPredictionClock.Recentre.AnRttFallUnderTheOffsetFloorReArmsOnlyOnTheFlooredOffset` pins the
rule on a fall to 15 ms: from 90 ms (offset 6) it re-arms and logs `toTarget=4`; from 75 and 55 ms (5 and 4) it does not.

**Why it cannot oscillate on jitter alone** (rewritten in rework 3, after its review's finding R1). The trigger does not
read the prediction offset. That offset adds `jitterMultiplier` (2; the engine source's, §17) times the smoothed jitter, an exponential average
(`jitterSmoothingAlpha`, 0.15) of each sample's distance from the smoothed RTT, and on a heavy-tailed steady link that
term alone moves it by several ticks. Ten minutes at 70 ms + Exp(30 ms), or on a bursty Wi-Fi profile (75 ms + Exp(5 ms),
with an Exp(80 ms) burst on 5 % of the samples), one sample and one advance per frame, three seeds each: the offset ranged
over 5 to 21 ticks, and a trigger on it (rework 2's) re-armed 62 to 108 times a session and took 3.2 to 4.4 times the
Skips and Stalls of the clock without the re-arm. The trigger reads the RTT-only offset instead, so jitter reaches it only
through the smoothed RTT, an exponential average (`rttSmoothingAlpha`, 0.15) of the samples themselves. Two guards
filter what is left: the 2-tick threshold (a `static_assert` keeps it at 2 or more), which the one-tick wobble of the
`ceil` near a tick boundary never reaches, and the 15-advance run, which a short excursion of the RTT average does not
complete. On the same links and seeds the RTT-only trigger re-armed at most once in ten minutes, and the session took at
most 4 Skips and Stalls more than the clock before the re-arm (rework 1).
`PCTM.ClientPredictionClock.Recentre.AJitterySteadyLinkDoesNotReArmIt` pins it: at most 2 re-arms, at most 5 Skips and
Stalls more, and a full offset that does swing by 10 ticks or more. The one-tick wobble around a decision is
`PCTM.ClientPredictionClock.Recentre.AOneTickOffsetWobbleNeverReArmsIt` (offsets 5–6 around a decision at 5, and 5–7
around a decision at 6). Only a sustained move of the RTT average re-arms, which is an RTT step. Each re-arm then needs a
full settle window and the next decision before another can start, and that decision's RTT-only offset becomes the new
reference.

A client hitch that inflates the averaged ping for a few seconds is such a step, and it re-arms twice, once into the
spike and once out of it. In rework 2's shaped rendered smoke (2026-11-18), both clients stalled 1.5 s at the same
instant and hard-resynced. The averaged ping then read about 154 and 188 ms for about 4 s where the link gave 80, and the
full offset went from 5 to 12 and 15. Each client re-armed into the spike (`owed=0`: the ordinary graduated Skips outside
the dead band had already followed it) and out of it (`owed` 3 and 2). Both were on target again about 6 s after the
resync. That smoke ran rework 2's trigger. Re-read from its logged smoothed RTT, the RTT-only offset went from 5 to 10
and 12, stood there for about 180 samples and fell back to 5, so the RTT-only trigger re-arms into and out of the same
spike: its 15-sample run completes 1 sample later going in and 10 to 11 samples sooner coming out (the jitter term no
longer leads the run in, nor holds it back on the way out).

**The zero point is drift = 1, not 0.** The clock's drift is the target minus the tick it last stepped, so a Normal advance
at drift 1 steps the target tick itself, and at drift 0 it steps one tick past the target. A hard resync lands the same way:
it steps the target tick, and the next frame's drift is 1. og-input-latency P4-12's estimator trace tool reads the
lead the same way, as the stepped tick minus the server's tick against the target minus the server's tick, so a re-centre
onto drift 1 is what that tool reads as "on target". The ordinary dead band is unchanged and stays centred on drift 0.

**Why one shot and not a controller.** The user's ruling for this fix (2026-11-18, "one-shot now, dilation later"): the
continuous controller, a drift law with a band of about one tick, is og-time-dilation's work, and this step must stay
compatible with it. A one-shot re-centre adds no state a controller would have to undo, and its knob has an off value a
controller can set. The re-arm (rework 2) keeps that shape: each decision is still one discrete step, and a re-arm only
schedules another one when the target's RTT part (floored, as the target is) has moved by 2 or more ticks. It does not
follow the lead continuously.

**The window: 30, the size of `rttOutlierConsecutiveLimit`.** A `static_assert` below the struct checks that the
compiled default spans a whole outlier run at one reading per tick, so an inflated reading that the outlier gate is about to
re-seed from has either been rejected or re-seeded inside the window before the clock re-centres on it. The window counts
from the hard resync, so the resync frame's catch-up batch and the walk down from it are inside it. With the og-sim-net
client's clock coalesce (`SimNetClient-rationale.md` §8) an engine reading is one frame, so 30 samples were about half a
second at 60 frames per second; since P3-11 the window is 30 advances, half a second at 60 Hz, whatever the source's rate.

**The window is time, not samples** (og-input-latency P3-11, from P3-10 part B's F3). P3-10 flipped the clock's RTT source to
SimClock (§15), whose pongs arrive at 20 Hz against the engine's one reading per frame, so a window of 30 samples took 1.5 s
where it had taken about half a second, and the estimate kept moving during it: in P3-10's flipped smoke two of three joins
decided `owed=-1` on a smoothed RTT 67 to 69 ms that had started at a first reading of 76 to 87 ms. The window now counts
`ClientPredictionClock`'s own advances since the arm on which `hasFirstRTTSample()` read true (the advance's one read of the
flag, §16). An advance is one tick, so 30 is half a second at 60 Hz on either source, and at the engine's one reading per
authority tick it is the span the sample count gave. Details:
- **What counts.** An advance with a reading: a Normal advance, a graduated Skip or Stall, an input-delay debt Stall, and the
  catch-up batch's steps after a resync, all of which are ticks of time. The arm's own advance does not count: a hard resync
  returns before the count, and a re-arm counts the window before it re-arms, so either starts the window at the next advance.
  An advance before the estimator's first reading does not count either, so "Why a 0 reading does not count" below still
  holds: the window starts at the first real reading.
- **What it spans in samples.** About 10 SimClock samples, under `rttOutlierConsecutiveLimit`: on SimClock the first pong seeds
  the estimate (it arrives before the world, so before the join's resync), and a step up after it that the gate keeps
  rejecting re-seeds after 1.5 s, after the decision. The re-arm covers it when the RTT-only offset moves by two ticks or
  more; a one-tick move stays in the dead band, as before.
- **The decision reads the median of the window's last 15 advances**, as before; with a window of 30 advances the run is
  full by then if the advances are past the warm-up guard.
- **`samples=`** on the decision's line still reports the measured samples since the arm (10 on a 20 Hz SimClock join, 30 on
  an engine join; at least 8 since P3-12, below), so the line says how many readings the decision rests on.
- `PCTM.ClientPredictionClock.Recentre.TheSettleWindowIsTimeNotSamples` pins it: one sample every third frame on SimClock and
  one per frame on the engine source, an 80 ms link, both decide on the 30th frame after the resync with the same line but for
  `samples=10` and `samples=30`; counted in samples the SimClock join decided on the 90th (seen failing: `90 == 30`).
- **Two replays moved, each re-pinned with this reason.** `AFlickeringLeadIsDecidedOnItsMedianAndPaidInFull`'s +4 landing
  runs three catch-up advances in its join frame after the resync, which now count, so it decides three frames sooner
  (`target=1533 … samples=27`, was `target=1536 … samples=30`; the same `owed=4` and Stalls). `ThePh3cMainClientDoesNotReArmUnderTheOffsetFloor`'s
  samples carry 0 to 2 advances each, so its re-arm's window fills on its 29th sample (`target=1664 rttMs=52.082 samples=29`,
  was `target=1665 rttMs=52.080 samples=30`; the same `owed=2`, Stalls and settled reading). Every other case, the two
  batch-1 flickering joins and every one-sample-per-advance replay, decides on the same advance as before, and no golden in
  either `[@og]` suite moved.

**The window floors its samples** (og-input-latency P3-12, from the Ph4 gate's finding R3). Counted in advances, the
window is time only while the clock steps about once per tick of wall time, and a catch-up batch does not. In the Ph4
gate's shaped rendered shape (b) the join's hard resync landed in a rendered start-up frame whose batch ran 26 more
advances in 16 ms, every one with a reading, so the 30-advance window filled 0.14 s after the resync, on 1 SimClock
sample; every shaped (b) and loopback (c) join of that gate decided on 1 to 6 samples, and the (a) joins on 9 to 10.
The decision measured the lead mid-transient: the start-up frames took 16 to 164 ms, so the client was still stepping
off its backlog and the authority ticks were still arriving in bursts. One client decided `fromLead=19 toLead=5
owed=14 target=1891 rttMs=66.706 samples=1 cause=join` and paid its 14 Stalls in 0.9 s, but its 56 advances in that
time outran the authority's 52 ticks by 4, so the lead fell 10, and with the offset up 2 (5 to 7) it was left at +3.
The dead band then held it: it acts on drift beyond `softDriftThresholdTicks` (3) from 0, while the re-centre's zero
is drift 1 (below), so it holds a lead error of −2 to +4. One graduated Stall at +5 took the client to +2 for the
session (median +2, mean +2.32), and the re-arm never ran (the RTT-only offset never stood 2 ticks off its 5). Three of the four (b)
clients of the gate's candidate arm sat at +2.
- **The rule.** The window is also full only once the estimator has taken
  `ClientPredictionClock::kRecentreMinSamples` (8) measured samples since the arm (`getMeasuredRTTSampleCount`, the
  count the line's `samples=` reports). Samples arrive in wall time (SimClock's pongs at 20 Hz, the engine source's
  one reading per frame), so the floor binds only where advances outrun time; in steady state 30 advances hold about
  10 SimClock samples or 30 engine ones, and the advances still bind. A `static_assert` keeps the floor at or under
  the default window's advances, so one reading per advance never waits on it. It is a `constexpr` in the clock, as
  the re-arm's constants are: no new `TimeConfig` field and no new ini key. It applies to every arm (the join, a hard
  resync, a re-arm), and `evaluateDrift`'s mirror reads the same count.
- **Why 8.** The gate's 51 clients on the candidate tree, their logged inputs (the samples the smoothed RTT implies,
  the authority ticks and the advances per tick) replayed through the clock, reproduce every logged decision's advance,
  target and `owed` (on 6 the line's `samples=` and `rttMs` differ by where one sample landed between two ticks), and every
  logged target and advance on 45 of them. With a floor of 8 every one of the 36 shape (a) join decisions is
  byte-identical in the replay (they rested on 9 to 10 samples); every (b) and (c) decision moves to `samples=8`, 0.41 to 0.68 s after its resync; the three +2
  clients settle at a median of 0 (means +0.32, +0.04, −0.37 from 5 s on), and no client is 2 or more off (3 before).
  A floor of 6 or 7 left one (b) client at −1, and 9 to 15 one or two at +1. The two mid-session hard resyncs of the
  gate's warm-up run, whose decisions rested on 1 sample each, decide on 8 too.
- **Not adopted: a second decision on a lasting residual.** A follow-up decision after a residual of 2 or more ticks
  held for 1 to 2 s, replayed on the same clients without the floor, re-decided four of them, one by 4 ticks, which
  took that (b) client from +2 to −1. It would also follow the lead, which is a controller's work (og-time-dilation's
  src-29), and the lead error it would read swings with the jitter term on a heavy-tailed link (below).
- `PCTM.ClientPredictionClock.Recentre.TheWindowFloorsItsSamplesSoACatchUpBatchCannotFillIt` replays that client's first
  600 authority ticks (10 s) from its log. Without the floor it reproduces the logged decision and holds a median of +2
  (mean +2.31) from 5 s on (seen failing); with it, it decides `fromLead=14 toLead=6 owed=8 target=1925 rttMs=73.472
  samples=8 cause=join` and holds a median of 0 (mean +0.31). No other case or golden in either `[@og]` suite moved.

⚠ **What it does not cover.** An RTT step that arrives after the window (for example a re-seed long after the join, or
the start-up spike settling) is covered since rework 2 when it moves the RTT-only offset by 2 or more ticks: the re-arm
above (`PCTM.ClientPredictionClock.Recentre.AMidSessionRttStepReArmsItOnce`: 80 to 120 ms, offset 5 to 8, one re-arm,
lands on 0). A move of one tick, and a continuous drift that stays under 2 ticks from the last decision, are not covered:
the dead band holds them, as before. Since rework 4 an RTT change under the floor is invisible to the re-arm, as it is
to the target: it moves neither. That includes a fall that takes the target one tick down onto the floor (75 to 15 ms,
offset 5 to 4: the lead stays +1, as after any one-tick fall above the floor), which the unfloored trigger re-armed on
and corrected only because it read the fall as four ticks. Since rework 3 neither is a move of the jitter term alone: on the jittery links
above the clock holds a mean lead error of +1.8 to +2.8 ticks with or without the re-centre, which is the dead band
against a fast-moving jitter term. That is og-time-dilation's controller (src-29). A lead error that flickers across a tick boundary frame to frame is covered by
the median and the fixed debt since rework 1. Before it, one frame decided and the first frame that read on target ended
the correction, so a client at p50 +1 kept its +1 (one B client of nine in the task's headless smokes) and a +4 landing
stopped a tick short (`PCTM.ClientPredictionClock.Recentre.AFlickeringLeadIsDecidedOnItsMedianAndPaidInFull` pins the fix for
both). What remains is a share that moves after the decision. The decision sees only the run that the window ends in,
and the authority tick's arrival phase can drift over seconds. In the rework's headless smoke (2026-11-18) one client
read +1 on 6 of the 15 readings before its decision, which correctly owed 0, and then read +1 on 55 to 72 % of the
frames in the following seconds: its integer median over 10 to 30 s was +1, though its mean lead error stayed within
half a tick (+0.35 by the estimator trace tool). Following such a drift is a controller's work, not a one-shot's.

⚠ **The landing's source is not fixed here.** The catch-up batch runs in the tick driver's batch loop
(`SimulationTickDriver::runBatch`, called from `pumpAndRunSteps`), which steps every scheduled step of the frame whatever
the clock decided on the first one. Dropping the batch's remaining steps on a hard resync, or resyncing to the target minus
the remaining batch, needs the driver to learn of the clock's resync: a change across the tick driver's interface, not one
in the clock. The re-centre corrects the landing either way.

**Why a 0 reading does not count** (found by this task's headless smoke, 2026-11-18). One adapter's client RTT is
an averaged RoundTrip ping (one adapter's binding: `FNetPing` with `RoundTrip=PlayerStateAvg`, read in
`UESimNetLinkServices::serverRttSeconds`), which reads exactly 0 until its first one-second bucket fills. The estimator
takes the 0 as a real first reading and seeds from it. In the smoke every client's window filled on 0 readings
(`rttMs=0.000 samples=30`, about 0.5 s after the join) while the first real reading arrived 0.9 s after it. On that
loopback the offset stayed at its floor, so nothing moved; on a 100 ms link the target rises by two ticks after the
decision and the dead band keeps the client two ticks low (the float64 model and
`PCTM.ClientPredictionClock.Recentre.ZeroReadingsDoNotFillTheWindow`). Counting only readings above 0 starts the window at
the first real reading; a real round trip, even on loopback, reads above 0 (the smoke's first real readings were 21 to 31 ms: the smoothed RTT one sample after the 0 seed was 3.2 to 4.6 ms,
which is 0.15 of the reading).

**Nor does a 0 seed the estimator** (og-input-latency P5-28). The source's 0 is its first average, taken on its first ack
while every bucket is still empty, and it is held until the next bucket closes about a second later (one adapter's
binding: the engine's player-state ping average closes a bucket once a second has passed since the last close, whose
time starts at 0, so the first ack closes one, and an average over no pings is 0). Seeded on it, the estimate was 0 and the outlier bound
`rttOutlierMultiplier * smoothedRTT + rttOutlierMarginSeconds` was 30 ms, so on a link above 30 ms every real reading was
rejected until the escape hatch re-seeded on the 30th: Ph3's shaped (a) captures log `filtered 29 implausible RTT sample(s)
and force-re-seeded 1 time(s)` at every join, with the target on its floor until then. Since P5-28 a 0 before the first
reading above 0 latches nothing (`PCTM.NetworkTimeEstimator.StartUp.TheFirstRealReadingAfterAZeroSeeds`), so the first
real reading seeds the estimate and the target follows it. The window and the decision are unchanged: the window counted
the same readings, so the decision lands on the same sample, now about 30 samples after the seed instead of on it.
⚠ **It does not change the join's `owed`.** The join's hard resync lands on the floor target when the first authority tick
arrives, before the source has any reading, and a target one or two ticks above the floor is inside the dead band, so the
clock does not move before the decision, which still owes those ticks (`ZeroReadingsDoNotFillTheWindow`: `owed=-2` on a
100 ms link, the same line before and after P5-28). The decision's `rttMs` is not smoothed over the run either: the source
holds each one-second average, so the 30 readings before the decision are usually one value. Since og-input-latency
P3-10 the join resyncs once more when the first reading moves the offset (§16), and that decision owes 0.

**0 disables it exactly.** With `recentreSettleSamples = 0` the window never fills, so the decision never runs, nothing is
owed or logged and the dead band is the only correction: the clock is the one before this field (an armed clock still
stores its readings, and nothing reads them). The re-arm only runs after a decision, so at 0 it never runs either. `PCTM.ClientPredictionClock.Recentre.ZeroDisablesItExactly` compares every
result, tick and log line of a join, a hard resync and a steady stretch at 0 with a window that can never fill
(`UINT32_MAX`), not with the code before this field, so it shows only that 0 takes the never-fills path. The proof that 0
is the old clock is the goldens: with `recentreWindowFull()` poisoned to false (0 for every config), every golden in both
`[@og]` suites passes and only the re-centre's own cases fail (seven of them at rework 1, ten at rework 2, eleven at
rework 3, thirteen at rework 4, 2026-11-18). At the default no golden re-arms: with a failing check poisoned into the
re-arm, only the cases built to re-arm fail (two at rework 2; three at rework 3: the Ph3 (b) replay, the RTT step, and the
jittery-link case, two of whose six sessions re-arm once; five at rework 4: those three, the Ph3 (c) replay and the fall
under the floor).

⚠ **0 is not the binary before task 27 at a join.** The og-sim-net client's clock coalesce (`SimNetClient-rationale.md`
§8) is not gated by this field, so an arm at 0 runs the old clock fed one sample per drain, where the binary before task 27
fed one sample per queued clock message. At the Ph3 join every first-frame sample carried a smoothed RTT of 0 and the
floor offset, so the newest gives the same target as the last of them and the landing does not move; an attribution
arm should still say so.

## 15. The clock RTT source: `clockRttSource`, the switch and the SimClock shadow

One field added by og-input-latency P3-10 (part A), 2026-11-19: `clockRttSource`, `Engine` or `SimClock`, compiled
default `SimClock` since part B's flip (`Engine` before it; see "The flip" below). Its reader is `NetworkTimeEstimator`. Its one door is `SimulationManager::setClockRttSource`
(`SimulationManager-rationale.md` §16). P3-10's part B added the Unreal host's key, `[OGNetcode] ClockRttSource`, beside
`RecentreSettleSamples`, and wired both feeds and the shadow line (in the Unreal host's `UESimulationSession`).

**Why a second source.** The engine ping includes the engine lane's acknowledgement turnaround. og-input-latency P5-27
measured the client's RTT about 4 ms higher in Ph3 than in Ph2 on every shape. Ph3 took the inputs and corrections off the
engine lane, so the client's engine packets are mostly acknowledgement replies sent in step with the server's frames, and the
engine's round trip (from the send to the start of the client frame that processes the acknowledgement) now includes about one
server frame minus the client's turnaround, against about half a client frame before: 11.4 to 15.4 ms on loopback (one
adapter's binding: UE's `FNetPing` RoundTrip, read by `UESimNetLinkServices::serverRttSeconds`). og-sim-net's SimClock ping/pong
(`SimNetClock.h`) takes the server's turnaround out: `RTT = (t3 − t0) − (t2 − t1)`. The target offset is
`ceil((smoothedRTT + m × smoothedJitter) × tickFrequency)`, m the active source's multiplier (§17), so at 60 Hz 4 ms is 0.24 tick, and a whole tick
on a link whose RTT sits near a tick boundary.

**The routing.** The host hands every engine reading to `noteEngineRttSample` and every SimClock sample to
`noteSimClockSample`. The active source starts as `Engine`. An engine reading reaches `updateRTT` only while `Engine` is
active. A valid SimClock sample always feeds the shadow (below). With `clockRttSource == SimClock`, the first valid one
switches the active source to `SimClock`, calls `noteRttSourceChanged()` and logs `[Warning][ClockSource.Switch]` once;
from then on every valid SimClock sample reaches `updateRTT`, and engine readings reach nothing. `updateRTT` stays the
estimate's one door, so the validity gate, the "a 0 does not seed" rule (§14) and the outlier gate (§4) apply to either
source unchanged. The switch waits for a sample, not for the configuration, because the SimClock source has no reading
until its first pong returns. Until then the engine ping is the only source. If no pong ever comes (a server without the
clock), the engine keeps feeding the estimate.

**`noteRttSourceChanged()` never resets the estimator** (P3 design §7.1). The smoothed RTT and jitter,
`hasFirstRTTSample()` and every count stay. Three things change for the new source's first sample:
- It moves the RTT EMA but not the jitter EMA. Its distance from the estimate is the step between the two sources' biases,
  not jitter; read as jitter, `jitterMultiplier` would double it into the offset for tens of samples.
- If it reads above the estimate and at or under `rttOutlierColdStartCeilingSeconds`, the plausibility gate is **re-based**
  on it (below).
- The outlier run restarts: a run of the old source's outliers does not count toward the new source's escape.
Before the first seed it does nothing: there is no estimate to keep, and the first sample seeds as always (P5-28's 0 makes
this the usual case: the engine's averaged ping reads 0 for its first second, and a pong arrives sooner).

**A step down needs nothing.** The gate is one-sided (§4), so a lower reading is accepted and the RTT EMA slews to it. At
`rttSmoothingAlpha` 0.15 the remaining step after k samples is 0.85^k of it, and the 7th sample is the first to have covered
1 − 1/e of the step (1/α = 6.7): the design's "slews in ~7 samples"
(`PCTM.NetworkTimeEstimator.ClockSource.AStepDownSlewsInAboutSevenSamples`, 84 to 80 ms).

**The re-base.** A step up beyond `rttOutlierMultiplier × smoothedRTT + rttOutlierMarginSeconds` would be rejected
`rttOutlierConsecutiveLimit − 1` times and re-seeded on the next (§4): a reset, with 29 samples run on the old level's target.
Re-based, the gate measures against the larger of the smoothed RTT and the new source's first reading, so that reading and
the new level after it are plausible and the EMA slews up to it. The re-base lapses once the plain bound admits the reading
it was re-based on, which takes one sample for 20 to 200 ms and two for 10 to 400 ms, or after `rttOutlierConsecutiveLimit`
samples, the horizon of the re-seed it stands in for, so a spike on the switch sample cannot leave the gate loose. An
outlier escape's re-seed ends it too. A first reading above the cold-start ceiling earns no re-base, as it would earn no seed:
a frame hitch on the switch sample is filtered like any other, and the next accepted reading is the one that skips the jitter
update (`PCTM.NetworkTimeEstimator.ClockSource.AStepUpBeyondTheBoundDoesNotReSeedAfterTheReBase`,
`PCTM.NetworkTimeEstimator.ClockSource.AHitchOnTheFirstNewSourceReadingIsNotReBasedOn`).

**At `Engine` nothing changes.** `noteEngineRttSample` is `updateRTT`, a SimClock sample feeds only the shadow, and with no
switch the gate's reference is the smoothed RTT itself. `PCTM.NetworkTimeEstimator.ClockSource.AtEngineTheShadowNeverMovesTheEstimate`
compares every output, after every sample, with a twin fed only `updateRTT`.

**The shadow** (`NetworkTimeEstimator::getClockSourceShadow`), for the Unreal host's `[ClockSource.Window]` line (P3 §7.1
names its fields: the engine RTT, the SimClock RTT, og-net's acknowledgement SRTT, the offset and the one-way delay
variation):
- `simClockRttSeconds`: the SimClock RTT's EMA at `rttSmoothingAlpha`, seeded by the first valid sample, with no gate: the
  shadow is compared, not believed.
- `offsetSeconds`: theta of the minimum-RTT sample among the last `NetworkTimeEstimator::kClockOffsetSamples` (8, NTP's
  clock-filter register, RFC 5905 §10); a tie goes to the newest. Theta assumes symmetric one-way delays: on an asymmetric
  link it is off by (up − down)/2 (og-input-latency P2-20's F3), so the line names it the minimum-RTT sample's offset.
- `owdVarUpSeconds` and `owdVarDownSeconds`: RFC 3550's interarrival jitter (§6.4.1), `J += (|D| − J) / 16`, with D the
  change of one direction's transit time (`t1 − t0` up, `t3 − t2` down) between consecutive samples. A transit carries the
  clock offset, which D cancels. The line logs both, as `owdVarUs=<up>/<down>`.
- `samples` and `invalidSamples`: a sample with a non-finite stamp or an RTT not above 0 is counted and ignored, and never
  switches the source.
The shadow and the switch's bookkeeping are plain members on the estimator's writer thread, like the gate's counters. Read
the shadow on that thread (the drain that hands the samples over is where the line belongs).

**The re-centre (§14) reads the RTT through the same estimator.** Its re-arm compares the floored RTT-only offset with its
value at the last decision and re-arms at a move of `ClientPredictionClock::kRecentreRearmTicks` (2). P5-27's ~4 ms is 0.24
tick at 60 Hz, so at any RTT the floored RTT-only offset moves by at most one tick, and the switch never re-arms it
(`PCTM.ClientPredictionClock.Recentre.AFourMillisecondSourceStepNeverReArmsIt`: 61 to 120 ms, both directions; a 40 ms
control re-arms). A one-tick target move stays inside the dead band, as after any one-tick RTT move (§14 "What it does not
cover"). ⚠ So a switch that lands **after** a decision leaves a client whose target fell one tick one tick above it, and the
flip's gain does not reach that client. The switch lands at the first pong, which should precede the join's decision (about
half a second after the first real reading) and, if the pings start with the session, the first authority tick too; then the
join and its decision see the SimClock RTT from the start. Measured (P3-10 part B, one shaped run, three joins): the switch
lands 4 to 43 ms after the join's first hard resync and 1.5 s before its decision. The decision counted samples, and SimClock
delivers 20 a second against the engine's one per client frame, so after the switch the join's decision landed about 1.5 s
after the first reading rather than about 0.6 s. Since og-input-latency P3-11 both are fixed: the window counts ticks (§14,
"The window is time"), and one adapter's host holds the newest sample that arrives before its world session binds and feeds
it ahead of the first authority tick (one adapter's binding: the Unreal client host), so the join's own resync sees the
SimClock reading and §16's second resync has nothing to wait for.

**The flip** (P3-10 part B, 2026-11-19). Before the default changed, a shadow smoke on Ph3's shape (a) (og-net's shaper, lag
15-50 ms each way, loss 2/1 %; three joins) logged the SimClock RTT below the engine's in 24 of 25 ten-second windows, by a
median of 13.1 ms (−19.0 to +1.4): about one server frame, the turnaround the engine's ack round trip carries. In the
flipped run's three joins the switch came before the engine's first reading above 0 (`hasFirstRTTSample=0` on each
`[ClockSource.Switch]` line), so the first SimClock sample seeded the estimate and `noteRttSourceChanged()` had nothing to
keep; the step and the re-base cover a switch after a seed. So the compiled default
is `SimClock`; `Engine` is the configuration key's other arm, and a host that delivers no SimClock sample stays on the engine
ping. Whether the flip holds the gate's counts (0 extra Skip, Stall or HardResync, the join's ` cause=firstRtt` resync counted
apart; (a)/(b) lead and late share at or below Ph2's) is Ph4's gate's measurement, over more runs than one smoke.

**The server-tick estimator** (og-time-dilation task 3, 2026-11-20). `SimClockRttSample` also carries the pong's
`serverTick` and `serverTickAgeSeconds` (appended after `t3Seconds`, so a positional copy of the first six is unchanged), and
every valid sample, at either source and before and after the switch, ends `noteSimClockSample` in the estimator's
`ServerTickEstimator`, after `updateRTT`, so its RTT gate reads the smoothed RTT that sample produced. A sample whose tick is
`SimClockRttSample::kNoServerTick` (what a host that copies no tick delivers) is ignored there, and the estimator's other
outputs never read it (`PCTM.NetworkTimeEstimator.ServerTick.AValidSampleReachesTheServerTickEstimatorAndMovesNothingElse`).
`activeJitterMultiplier()` exposes the multiplier the offset uses (§17). The sensor, its formula and its limits:
`ServerTickEstimator-rationale.md`.

## 16. The join's second hard resync, on the first RTT reading

Added by og-input-latency P3-10 (part A, the lead's 07:20 line from P5-28's smoke), 2026-11-19. Its reader is
`ClientPredictionClock`; it has no field of its own and runs only while `recentreSettleSamples` is not 0.

**The problem.** The join's hard resync fires on the first authority tick, which arrives before the engine's averaged ping has
any reading (§14: it reads 0 for about a second). So it lands on the floor target, `predOffsetFloorTicks` (4) above the
authority tick. When the first reading lands, the target rises by one or two ticks on a WAN link, which is inside the dead
band, so nothing moves the lead until the re-centre's join decision about half a second later, which then owes the gap. P5-28's
shaped smoke: `owed` −1 on 3 of 6 joins; `PCTM.ClientPredictionClock.Recentre.ZeroReadingsDoNotFillTheWindow`: −2 on a
100 ms link.

**The rule.** A reading is *settled* on an advance when `NetworkTimeEstimator::hasFirstRTTSample()` read true on that advance
and on the one before it. A hard resync that lands on the floor offset (`predOffsetFloorTicks`) with no settled reading records
that offset and waits; with no reading at all the offset is the floor, so every resync before the first reading waits. On the
first advance past the warm-up guard whose reading is settled, if that reading has moved the offset and the clock is not
already on the target (drift 1, §14's zero point), the clock hard-resyncs once more, onto the target, and logs the resync line
with ` cause=firstRtt offset=<landed>-><now>` appended. Either way the wait ends on that advance. Once the estimator has a
settled reading the clock never waits again, so every later hard resync is unchanged. `evaluateDrift` reports `HardResync` on
that advance.

**Across threads (rework 1).** When `updateRTT` runs on another thread than the clock's advance (it could until
og-session-runtime task 13), the clock reads the estimator through relaxed atomics, which are not a snapshot (`ThreadingCrossings.md` row 10). Every other reader of the
estimate is re-read on the next advance, so a mixed read costs it one frame. This one acts once, so a mixed read on the advance
that first sees the reading would end the wait for good. The review (R1) found two: the flag changing between two loads in one
advance (the due check read it false, then the clear read it true), and the flag visible before its seed, which reads the floor
offset, equal to the one the resync landed on, so the check read "not moved" and the wait ended. So the clock:
- reads the flag once per advance, **before** the offset, and computes the target from that one offset read (`readTarget`,
  the same `authority + offset` sum as `getTargetPredictionTick`), so the due check, the target a resync lands on and the offset
  it records are one value;
- decides only on a settled reading: the advance that first sees the flag does neither the second resync nor the clear. The
  decision lands one advance later than it would on a reading seen at once.
- treats a resync on the floor offset with an unsettled flag as one with no reading. A flag read before its seed is visible
  reads the floor, so that resync waits too; when the reading really is at the floor, the next settled advance finds the offset
  unmoved and the wait ends with no second resync.

The alternative was an acquire/release pair on the flag (release on the seed's store, acquire on the clock's load), which would
make a flag read true imply a visible seed. It was not taken: it would make this the one acquire/release pair in a class whose
contract is relaxed on every access, for one reader. The flag changing between two loads needs the single read either way. And
a test cannot build a read that the memory order rules out, so that fix could not be seen to fail. The settled rule needs only what row 10
already assumes, that the next read supersedes a mixed one. It is tested through
`PCTM.ClientPredictionClock.Recentre.TheFirstRttResyncSurvivesAMixedReadOfTheSeed`, with a read probe that is a friend of the
estimator and is defined only in og-simulation-tests. The probe hides the seed for one advance, or lands the seed between two
loads. Immediate mode has no mixed read (the writer and the reader are both on the step's thread) and takes the same delay:
a reading is settled on the second advance that reads it, as in the single-threaded tests.

**Why a second resync and not a held one.** Holding the join's resync until the first reading would leave the client stepping
its own pre-join ticks for that long (about 0.6 s on P5-28's shaped smoke: the first real reading came 34 to 41 samples after the
join), sending inputs for ticks the server refuses as late. The second resync keeps the join frame as it is and moves the
frontier once more, at join time, where a resync's cache wipe costs least. A re-centre debt instead (one graduated correction
per `gradualCorrectionRate` advances) would leave the decision's window reading a lead that is still being paid.

**With the re-centre.** The second resync is a hard resync: it re-arms the re-centre, still as the join's arm (no decision has
been made), and the settle window counts from it. The first two readings were counted before the arm (the resync waits for a
settled reading, rework 1), so the decision lands two advances later (P3-11; two samples when the window counted samples) than with no second resync, on a frontier that is on the target, and owes 0 in every case of
`PCTM.ClientPredictionClock.Recentre.AJoinBeforeTheFirstRttReadingResyncsOnceMoreWhenItLands`. It also lands on the target
whatever the first resync's catch-up batch left (the Ph3 landing above), which the re-centre otherwise paid in graduated
Stalls. It is gated with the re-centre so that `recentreSettleSamples = 0` stays the clock before both, for an A arm.
Three of §14's join replays changed with it, each re-pinned with this reason: `ZeroReadingsDoNotFillTheWindow` (the
decision's `owed` −2 to 0, no Skips), the Ph3 (b) replay's second client (`owed` −2 to 0, two Skips fewer; its re-arms are
unchanged) and the Ph3 (c) replay (`owed` 2 to 0, two Stalls fewer; its re-arm unchanged). The (b) replay's first client
landed on its first reading's target already (drift 1) and is unchanged. Rework 1 moved each second resync one advance
later and each join decision's target one tick later in those replays and in the case above; nothing else in them moved.

**With the SimClock source** (§15) the first pongs arrive before the world, so before the first authority tick. P3-10 part B
measured the estimator's first sample 4 to 43 ms after that tick anyway, because one adapter's host dropped a sample while no
world session was bound; two of three joins then took the second resync. Since og-input-latency P3-11 that host holds the
newest such sample and feeds it ahead of the first authority tick (one adapter's binding: the Unreal client host), so the
join's own resync has the reading. On shape (a) (og-net's shaper, lag 15-50 ms each way, loss 2/1 %) the nine joins of three
runs on that build all switched to SimClock in the drain of their first authority tick and took no second resync; the same shape on the build
before P3-11, three joins, took two. The second resync is for a source whose first reading comes late, as the engine's does.
⚠ The resync's advance still reads the flag for the first time, so it is not settled: a join whose first reading puts the
offset on the floor (a round trip under about 67 ms with no jitter) still waits, and takes the second resync if the next
settled advance finds the offset moved. None of those nine did.

⚠ **It is one more `HardResync` per affected join.** A gate reading "0 extra HardResync" (P3-10's flip) counts the
` cause=firstRtt` lines apart.

## 17. The jitter multipliers: `jitterMultiplier` and `simClockJitterMultiplier`

Added by og-input-latency P3-13, 2026-11-20 (Ph4 gate R2/R15, the user's ruling "tune it in Ph4"). The prediction offset is
`ceil((smoothedRTT + m × smoothedJitter) × tickFrequency)`, floored at `predOffsetFloorTicks` (§5). Since this task m is the
**active RTT source's** multiplier (§15): `jitterMultiplier` while the engine ping feeds the estimate (every client before
its first SimClock sample, and every client at `clockRttSource = Engine`), `simClockJitterMultiplier` once the SimClock ping
does. Their reader is `NetworkTimeEstimator::getPredictionOffsetTicks`; their one door is
`SimulationManager::setJitterMultiplier` (`SimulationManager-rationale.md` §17), which sets both to one value (one adapter's
binding: the Unreal host's `[OGNetcode] JitterMultiplier`, reported on its `[ClockSource.Config]` line). The door clamps to
0..4 (`clockJitter::clampJitterMultiplier`); both compiled defaults lie inside the clamp (a `static_assert`).

**Why the sources need their own.** The engine ping read almost no jitter (Ph4's gate: `smoothedRTT=84.714ms
smoothedJitter=0.003ms` on shape (a)), so its offset was in effect `ceil(RTT × 60)`. The SimClock ping reads the link's
real per-sample variation (`smoothedRTT=66.947ms smoothedJitter=9.221ms` on the same shape), so with the one multiplier of 2
its offset stayed at 6 ticks: 2 × 9.2 ms took back the 18 ms the RTT gained, while the authority-tick anchor arrived fresher.
The gate measured (a)'s lead at 3.370 ticks (de-lagged) against Ph2's 2.453, with the late share at 2.6 pp against Ph2's 9.2.

**Across threads.** The offset is read on the client's stepping thread, and the switch happens on the estimator's writer
thread, which may be another (it could until og-session-runtime task 13). So the active source is a relaxed atomic with the estimate's one writer, like the RTT
and jitter EMAs: a read that combines a new source with an old EMA is one frame's offset, superseded by the next read. At
the switch the first SimClock sample does not move the jitter EMA, or seeds it at 0 (§15), so the new multiplier first
applies to the engine era's jitter, which is about 0.

**The measurement** (P3-13, the Editor Development build with P3-12's sample floor; Ph4's F arm: immediate client traffic,
the authority loop on; og-net's shaper b0: lag 15-50 ms each way, loss 2/1 %; one value per run through the door, two runs
per value and shape, interleaved, after a discarded warm-up). Lead = the mean over runs of each run's mean over clients of
the lead p50, with the loop-on server's log-stamp lag removed; late share = the mean over runs of the pooled relay late
share; E2E-a (capture to the server's release) de-lagged; E2E-b (capture to its use on the receiver); counts summed over
runs and clients. Ph2's figures are its gate references.

| shape | multiplier | offset mean (ticks) | lead p50 (ticks) | late share (pp) | E2E-a p50 / p95 (ms) | E2E-b p50 / p95 (ms) | Skip / Stall / HardResync |
|---|---|---|---|---|---|---|---|
| (a) | Ph2 | – | 2.453 | 9.21 | 142.0 / 154.0 | 103.5 / 185.0 | 3 / 0 / 6 |
| (a) | 2.0 | 6.01 | 3.283 | 3.88 | 158.7 / 165.8 | 101.5 / 115.5 | 4 / 5 / 6 |
| (a) | 1.5 | 5.83 | 2.960 | 6.53 | 149.2 / 159.3 | 104.5 / 185.5 | 3 / 4 / 6 |
| (a) | 1.0 | 5.38 | 2.760 | 4.37 | 149.7 / 157.0 | 101.0 / 146.5 | 1 / 4 / 6 |
| (a) | **0.5** | 5.04 | **2.300** | 2.90 | 140.7 / 146.8 | 101.0 / 112.5 | 0 / 5 / 7 |
| (b) | Ph2 | – | 4.037 | 10.55 | 167.0 / 178.5 | 104.0 / 212.0 | 4 / 14 / 4 |
| (b) | 2.0 | 6.02 | 3.396 | 1.48 | 156.5 / 164.5 | 102.0 / 108.5 | 1 / 49 / 4 |
| (b) | 1.5 | 5.83 | 2.960 | 2.59 | 149.3 / 156.8 | 104.5 / 106.5 | 2 / 39 / 4 |
| (b) | 1.0 | 5.38 | 2.745 | 4.24 | 145.8 / 154.3 | 102.0 / 142.0 | 3 / 58 / 4 |
| (b) | **0.5** | 5.04 | **2.565** | 18.97 | 144.0 / 156.8 | 121.5 / 153.5 | 1 / 57 / 4 |

- **The lead follows the offset**, and only 0.5 brings (a)'s to Ph2's (2.300 ≤ 2.453; 1.0 reads 2.760). Every arm's clocks
  sat on their targets (every client's lead − target p50 within ±1 and every arm's pooled mean within ±0.5, from 10 s after
  the first step); what moved is the target, about 2.9 (0.5) to 3.9 (2.0) ticks ahead of the server (its p50, on server
  stamps not de-lagged). The extra HardResync at 0.5 on (a) is one join's ` cause=firstRtt` resync (§16).
- **The late share does not follow the multiplier.** It follows how far apart the clients' leads sit: a relay input is late
  when its sender runs behind its receiver, and a change of the offset moves both leads alike. Over the 16 runs the late
  share rises with the spread of the clients' lead − target means (0.06 to 0.26 tick: 0.8 to 1.9 pp; 0.34 to 0.76 tick: 2.0
  to 6.7 pp), and the one
  outlier is (b)'s second run at 0.5: its two clients sat 2.0 ticks apart (+1.35 and −0.66 over target), 36.0 pp. Its other
  run read 1.9 pp, and (a)'s two runs at 0.5 read the lowest late share of any value (3.8 and 2.0 pp). That spread is the
  clock's landing after a join (the dead band holds a residual, §14), not the margin.
- **The server's margin holds.** The server-slack p5 stayed at 4 to 6 ticks on every run (Ph2 5 to 6). Resimulations:
  0.33 to 0.99 a minute on (a) (Ph2 0.31 to 0.94); on (b) none on six runs and 0.72 on two (one at 1.0, one at 0.5; Ph2 none).
- **So no value meets "lead and late share at or below Ph2's on both shapes" on these means**: 0.5 misses (b)'s late share
  by its one split run, and every other value misses (a)'s lead. The compiled default is **0.5**, the only value that meets
  the lead on both shapes, with the late share left to the clock's landing (above). `jitterMultiplier` (the engine source)
  keeps 2.0: the sweep measured only the SimClock source.
- ⚠ 0.5 × 9 ms is a 4.5 ms margin over the RTT. On a link whose jitter is heavier than the shaper's, the margin shrinks with
  the multiplier; the door exists to re-run this sweep there.

`PCTM.NetworkTimeEstimator.ClockSource.EachSourceHasItsOwnJitterMultiplier` pins the offset at 2.0, 1.5, 1.0, 0.5 and 0 for
each source, on an exact RTT and jitter (1/16 s and 1/32 s): 8, 7, 6, 5 and 4 ticks.

## 18. The clock's published correction phase

Added by og-time-dilation task 5, 2026-11-21. Its writer is `ClientPredictionClock`; it has no field of its own. Its reader is
the dilation controller (og-time-dilation task 6), which holds its rate scale at 1.0 while the clock's own corrections run:
the re-centre armed or owing ticks (§14), an input-delay transition debt owed (Stalls or, since og-input-latency P6-2, Skips), the wait for the first RTT reading (§16), and a while after any
hard resync. That reader runs once per frame on the scheduler's owning thread (the client's frame thread), and the clock
advances on the step's thread, so the clock publishes what the reader needs as one atomic word.

**The word.** `ClientPredictionClock::publishedCorrectionPhase()` returns a `ClockCorrectionPhase`, decoded from one relaxed
load of a `std::atomic<uint32_t>` (lock-free, a `static_assert`):

| bits | field | what the clock reads |
|---|---|---|
| 0 | `recentreArmed` | `isRecentreArmed()` |
| 1 | `recentring` | `isRecentring()` |
| 2 | `transitionDebt` | `getPendingInputDelayTransitionTicks() != 0`: Stalls or Skips owed. Named `stallDebt` and read as `getRequiredInputDelayIncreaseStallTicks() > 0` until og-input-latency P6-2 made the debt signed; a Skip debt must freeze the controller as a Stall debt does |
| 3 | `awaitsFirstRtt` | the hard resync's wait for the first settled reading (§16) |
| 8–31 | `hardResyncs` | the clock's hard resyncs, modulo 2²⁴ |

Bits 4–7 are 0. The count is the clock's own production counter, not the event seam's `hardResyncCount` (the seam is
diagnostic only and no production code may read it). It wraps after 16.7 million resyncs; the reader only compares it for a
change, so the wrap costs nothing.

**When it is written.** One relaxed store, built from the accessors in the table, as the last statement of
`advancePrediction()`, of `requestInputDelayTransition()` (its no-op path for a delta of 0 too; `requestInputDelayIncreaseStall()` forwards to it, with a non-positive delta as 0, since og-input-latency P6-2) and of the
constructor (the re-centre is armed from construction, so a reader before the first advance sees the arm). The store is not
"remembered" at each return: `advancePrediction()` calls the advance's body, `advanceAndCorrect()`, and publishes after it,
so every return of the body (the hard resync, the transition debt's Stall and Skip, the warm-up guard, the dead band, the graduated Skip,
the graduated Stall, the graduated zone's normal advance) and any return added later reach the one store. A hard resync's
callbacks run inside the body, so they run before its store: a reader on the callbacks' thread sees the phase from before
the resync until the advance returns.

**The contract: one writer, any reader, write-only for the clock.**
- **One writer**, the clock's thread. Every write is a whole word, so a reader sees one store's value and never a mix of
  two. ⚠ The stall request is called by the tier receipt. A receipt in the step's drain, on the clock's thread, keeps the
  contract as stated. A receipt on another thread while the clock advances would be a second writer, as it would be of
  the transition debt itself (a lock-free atomic since og-input-latency P6-2, consumed by compare-exchange: `ThreadingCrossings.md` row 12). Its store would encode the re-centre's flags as that thread reads
  them, and might land after a newer advance's store; the clock's next advance stores the word again, so it would be wrong
  for at most one advance.
- **Any reader**, from any thread. The load is relaxed and the word is ordered with nothing else: a reader that also reads
  the estimator or the prediction tick may combine values one advance apart. The freeze is re-evaluated every frame, so that
  mix costs at most one frame's freeze decision, the same posture as the estimator's relaxed fields (`ThreadingCrossings.md`
  row 10).
- **Write-only for the clock.** No clock decision reads the word: every decision reads the clock's own members, and the word
  is an output, not state. A decision that read it would make the clock depend on a value another thread can see stale (or a
  test can overwrite), and the clean and the overwritten clock would diverge. `ClientPredictionClockPhaseProbe`, a friend
  declared in the header and defined only in og-simulation-tests (the pattern of §16's estimator probe), overwrites the word
  for the fence below. ⚠ The crossing is not yet in `ThreadingCrossings.md`: it has no off-thread reader until the controller
  is wired in, and that task adds the row.

**Tests.** `PCTM.ClientPredictionClock.Phase.ThePublishedWordFollowsEveryAdvanceThroughAJoinADebtAndAResync` drives a join
whose first frame has no RTT reading (a hard resync onto the floor target that waits for the first reading, then a
four-advance catch-up batch), the settle window, a decision that owes 3 and their payment, a 2-tick stall debt and its
payment, and a second hard resync with its re-armed decision; after every advance and every stall request the word equals the
accessors and an independently counted resync total, and every flag is seen both ways. It also poisons the event seam's
resync count and checks the published count does not follow. Seen failing with the store skipped on the stall-debt path:
the word still read `debt=1` after the debt's last Stall.
`PCTM.ClientPredictionClock.Phase.ThePublishedWordCannotReachAnyClockDecision` is the fence, in the event seam's pattern:
the same lifecycle with the word overwritten before every advance and every request (every field wrong, all ones, zero)
leaves every `AdvanceResult`, both cursors, `evaluateDrift`, the stall debt, the re-centre's accessors, the resync
callbacks' ticks and every log line identical to the clean run, and one more tick of real stall debt moves the trace. Seen
failing with the debt branch also requiring `publishedCorrectionPhase().stallDebt`.

### 18.1 The published target read (og-time-dilation task 10c)

The dilation controller's band guard bounds `P − (A + O)`: the stage's deadline tick against the target tick the clock
reads, `A + O`, at its next advance (`ClockDilationController-rationale.md` §5.4). The stage cannot read that target itself:
an immediate client drains its authority ticks at each step's start, after the stage, so the stage's own `A` predates every
arrival since the clock's last read. So the clock publishes the target it read.

**The word.** `ClientPredictionClock::publishedTargetRead()` returns a `ClockTargetRead {advances, targetTick}`, decoded from
one relaxed load of a `std::atomic<uint64_t>` (lock-free, a `static_assert`): `advances` in the high 32 bits, `targetTick`
in the low 32.
- `targetTick` is `readTarget()`'s `targetTick`, `getLastAuthorityTick() + getPredictionOffsetTicks()`, the one value the
  advance's drift, its hard-resync landing and its re-centre reading are computed from. `advanceAndCorrect()` copies it into
  `m_lastReadTargetTick` right after its `readTarget()`.
- `advances` counts `advancePrediction()` calls, modulo 2³². A reader compares it only for a change: a new count is a new
  read, the same count is the read it already saw. Before the first advance the word is `{0, 0}`.
- One 64-bit word, so a reader never pairs one advance's count with another advance's target.

**When it is written.** `advancePrediction()` stores it with one relaxed store after the phase word's, as its last statement:
`publishTargetRead()` increments the count and stores `(advances << 32) | m_lastReadTargetTick`. Every path of the body (the
hard resync, the first-RTT resync, the transition debt's Stall and Skip, the warm-up guard, the dead band, the graduated Skip and Stall, the re-centre's
payment) returns into it, as for the phase word. `requestInputDelayTransition()` (and `requestInputDelayIncreaseStall()`, which forwards to it) does **not** store it:
it reads no target. The constructor does not either: `{0, 0}` already says "no read".

**The contract** is the phase word's: one writer, any reader, write-only for the clock.
- **One writer**: `advancePrediction()`'s caller, the step's thread. Unlike the phase word there is no second writer: a
  tier receipt on another thread would call the transition request, which does not store this word.
- **Any reader**, relaxed, ordered with nothing else. The stage reads it once per frame on the scheduler's owning thread. A
  read that lands inside a batch running on another thread sees the batch's latest advance so far; the controller pairs it
  with its previous call, a one-sample approximation `ClockDilationController-rationale.md` §5.4 accepts.
- **Write-only for the clock.** No clock decision reads the word; it is an output. `ClientPredictionClockPhaseProbe` (the
  friend of task 5, defined only in og-simulation-tests) also overwrites and reads this word for the fence below.

⚠ Neither word has a `ThreadingCrossings.md` row yet. Task 5 left the row to the task that wired the reader; og-time-dilation
task 8 wired the stage into the Unreal host without adding it. Recorded for the initiative's lead (task 10c's notes).

**Tests.** `PCTM.ClientPredictionClock.Read.ThePublishedReadIsEveryAdvancesOwnTarget` drives the phase cases' lifecycle (the
join's hard resync onto the floor target, the settle window, a decision paying 3, a 2-tick stall debt, a second hard resync
with its re-armed decision) plus a graduated Skip and a graduated Stall: after each of its 406 advances the word equals the
advance's own count and the `A + O` the estimator held when it ran, and both stall requests leave the word unchanged. Seen
failing with the store moved before the body (the previous advance's target): 391 of 406 advances mismatched, the first
`published {1, 0}, read {1, 1506}`.
`PCTM.ClientPredictionClock.Read.ThePublishedReadCannotReachAnyClockDecision` is the fence: the same lifecycle with the word
overwritten before every advance and every request (both fields wrong, all ones, zero) leaves every `AdvanceResult`, both
cursors, `evaluateDrift`, the stall debt, the re-centre's accessors, both published words after each advance, the resync
callbacks' ticks and every log line identical to the clean run; one more tick of real stall debt moves the trace. Seen
failing with the advance taking its target from the word whenever the word's count differs from its own: all three
overwrites moved the trace.
