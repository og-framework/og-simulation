<!-- SPDX-License-Identifier: MPL-2.0 -->
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
`UPROPERTY`, `ASimulationTimingRelay`, the `AsyncFixedTimeStepSize` key, `Config/DefaultEngine.ini`
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
step is sustained and always does. About 30 samples is about 0.3 s at a 100 Hz timing relay — longer
than a hitch's inflated-acknowledgement backlog, short enough that a real step is absorbed in well
under a second.

**Why the multiplicative bound is loose and the additive one exists.** 4x is deliberately loose: this
gate is aimed at 100x frame-hitch artifacts, not at trimming ordinary jitter, and anything under 4x
the current estimate is inside what `jitterMultiplier * smoothedJitter` already covers. Without the
additive half the multiplicative term collapses on LAN — at a 0.5 ms smoothed RTT, 4x is 2 ms, which
would reject an entirely ordinary 10 ms reading. 30 ms sits above any plausible loopback/LAN
excursion and far below the ~1 s artifacts being filtered.

**Why the cold-start ceiling is absolute rather than relative.** The first sample seeds the smoothed
RTT VERBATIM, so the relative bound has nothing to compare against. That seed is the single most
damaging value in the estimator — it is latched, and every subsequent jitter delta is measured
against it — so it is gated by an absolute ceiling instead, chosen over accept-then-correct. A
genuinely slow link is not locked out: `rttOutlierConsecutiveLimit` re-seeds it after a short run of
consistent readings, so the ceiling costs a fraction of a second on such links and rejects a
hitch-inflated first reading outright.

**Why rejections are counted and summarised.** A silent reject hides a genuine RTT step change
exactly as well as it hides a hitch artifact, so rejections are counted per window and summarised in
ONE log line per window that contains any. The window is sized to bound log volume at roughly one
line per 6 s at a 100 Hz timing relay even if the ping source misbehaves continuously. Setting it to
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
replication callback — and once more on every read inside `applyRelayDelayFloor`. That
belt-and-braces shape is the same one `clampConnectionTierIndex` uses for the replicated tier.

**Source of truth and distribution.** The SERVER owns the value, optionally overridden from the
`RelayDelayFloorTicks` key under `[OGNetcode]` at the composition root, and replicates it to every
client as its own uint8 replicated property on the session-scoped TIMING RELAY — session-scoped state
on the session-scoped vehicle. (One adapter's binding: a `UPROPERTY` on `ASimulationTimingRelay`.)
Clients never derive it locally; a client's copy of the field is written from that property's
replication callback. The deferred dynamic-floor policy (design §11 Q6) needs no new mechanism: it
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
