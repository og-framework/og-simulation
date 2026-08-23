<!-- SPDX-License-Identifier: MPL-2.0 -->
# `ResimGateProbe.h` — rationale

The derivations, the archived measurement record and the reachability arguments behind
`ResimGateProbe` and `CorrectionLandingProbe`. The header keeps every fence, the per-declaration
contracts and a short orientation block; this file carries everything else. The header is the
operational reference — read it first, and come here when you need the *why*, not the *what*.

**If this file and `ResimGateProbe.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

**This is not the lineage document.** What the gate used to be, why it was rebuilt, which item
changed what, and every retired symbol the story touches, are `docs/History-ResimGate.md`, written
in closed tense. Nothing here repeats it. The two split on tense: that file records what *happened*,
this file records what a number *means today*.

⚠ **EVERY ARCHIVED FIGURE BELOW WAS MEASURED UNDER THE COMPILED DEFAULT TRIGGER POLICY**, which is
not the policy the shipped configuration selects. §20 is the correction record for the three places
the header had that backwards. Before reading any number here against a live log, establish which
policy the run used — the pair is stated once, with both halves anchored, at
`TimeConfig::resimTriggerPolicy`, and a run announces its own value on a `[ResimGate]` startup line.

<!-- lint-external-ref: FSimulationManagerAsyncCallback::TriggerRewindIfNeeded_Internal -- ENGINE-SIDE ADAPTER SYMBOL of ONE adapter (Unreal/Chaos), named in §9 as that adapter's binding for the rewind-request role; another adapter substitutes its own and this token means nothing there -->
<!-- lint-external-ref: deepAnchorSkips -- THE CONFIGURATION FILE'S AND CALL SITE'S OWN SPELLING of the counter, quoted in Sec 7 precisely because it differs from the member; the member is deepAnchorExclusions and this name deliberately does not resolve -->
<!-- lint-external-ref: GetResimFrame -- ENGINE SYMBOL of ONE adapter's physics replication, named in Sec 9 as a feeder of that adapter's depth merge -->
<!-- lint-external-ref: CompareTargetsToLastFrame -- ENGINE SYMBOL of ONE adapter's physics replication, the alternative feeder named beside it -->
<!-- lint-external-ref: FRewindData::FindValidResimFrame -- ENGINE SYMBOL, not a name in this repository: the rewind-frame search of ONE adapter's physics engine, quoted in §9 so a reader who has that engine can re-check rung 3 at source -->
<!-- lint-external-ref: FMath::Min -- ENGINE SYMBOL of ONE adapter, the depth merge named in §9's rung 2; another adapter substitutes its own -->
<!-- lint-external-ref: BlockResimFrame -- ENGINE SYMBOL of ONE adapter, the refusal floor a retried request hammers in §8; it has no counterpart in this repository -->
<!-- lint-external-ref: DEBUG_REWIND_DATA -- ENGINE BUILD MACRO of ONE adapter, named in §8 because it is what makes a refused rewind silent in a normal build -->
<!-- lint-external-ref: DEBUG_NETWORK_PHYSICS -- ENGINE BUILD MACRO of ONE adapter, named beside the one above for the same reason -->
<!-- lint-external-ref: finding_task31_resim_rate.md -- PRIVATE-ARCHIVE DOCUMENT: the investigation these counters were commissioned from lives in the initiative workspace, outside this repository, and is cited in §5 and §8 as the provenance of the archived figures -->
<!-- lint-external-ref: FNetworkPhysicsCallback::TriggerRewindIfNeeded_Internal -- ENGINE SYMBOL of ONE adapter, the physics callback that hosts rung 2's depth merge; quoted in Sec 9 so a reader with that engine can find the merge -->
<!-- lint-external-ref: PhysicsStep -- ENGINE-LOOP VARIABLE of ONE adapter, quoted in Sec 9 rung 3 as the quantity that always equals the resim step there -->
<!-- lint-external-ref: ResimStep -- ENGINE-LOOP VARIABLE of ONE adapter, the counterpart of the above; same reason -->
<!-- lint-external-ref: design_task43_resim_gate_fix.md -- PRIVATE-ARCHIVE DOCUMENT: the design that produced the edge-triggered gate, cited in §9 and §13 as the provenance of the engine-source rulings -->

---

## 1. The one include, and the STL boundary it does and does not preserve

`ResimGateProbe.h` includes `OGSimulation/Network/CorrectionVerdictProbe.h` for one reason:
`PredictedCharacterClass`. The class split in `CorrectionLandingClassSummary` and the class split in
`CorrectionVerdictWindowSummary` must be the *same* split, and the only way to guarantee that is to
share the enum rather than derive a second one. Two independently derived notions of "remote" are
what would let the two halves of a summary describe different populations while both look
authoritative — and a divergence like that is silent, because each half is internally consistent.

**What that include costs, stated precisely because the header's own claim used to overstate it.**
`RelayReadProbe.h` states the family's testability property as *engine-agnostic, STL only — no UE
types, no OGTypes, no other OGSimulation header, so the file is testable without a simulatable or a
logger*. `ResimGateProbe.h` satisfies every clause of that except the third: it *does* include
another `OGSimulation` header. The consequence is nil, because the included header is itself
`<cstdint>`-only, so the transitive include set is unchanged and the testability the clause exists
to protect is intact. But the property is not preserved *verbatim*, and the header now says so.

## 2. Two objects because there are two threads

`ResimGateProbe` is written only from the physics thread. `CorrectionLandingProbe` is written only
from the game thread. They are separate objects with separate windows, never shared and never
atomic, and that is a correctness property rather than a layout preference.

A single shared window would have one thread call `resetWindow` while the other is mid-increment.
The loss is not one sample — it is **a whole window's totals**, because the reset zeroes every
counter at once. Two objects make the race impossible rather than unlikely.

**The cost, stated so nobody looks for a line that cannot exist.** The under-resimulation reading
wants `landedBehind` (game thread) next to `requested` / `grants` / `finishes` (physics thread), and
those cannot share one log line without sharing state across the two threads. So the reading is two
adjacent Warning lines, each self-contained. `RelayReadProbe`, `RelayArrivalProbe` and
`FrameHealthProbe` state the same one-object-per-thread rule for the same reason, so the shape is
the family's, not this file's. `[ResimProbe.Landing]` alone already answers the
question — is there a large behind-frontier population that produced no trigger? — because
`atFrontierRatePerMille` is the frontier-touch rate, and `[ResimProbe.Gate]`'s `requested` is what
confirms the trigger rate tracks it.

## 3. One window length, two units, and no floating point

`kResimGateProbeWindowSamples` is an alias of `kCorrectionVerdictProbeWindowSamples`, not a second
literal. The commissioning instruction was to *mirror the divergence probe's window mechanism and
window length exactly (same constant source, do not invent a second window size)*, and the reason
survives the instruction: a `[ResimProbe.Landing]` line and a `[DivergenceProbe.Window]` line
closing on the same denominator describe the same interval, which is what makes them comparable
window-for-window in one log. A second `= 120u` here would be a second source of truth that drifts
silently the day the first one moves.

**The two units genuinely differ, and the header says so rather than implying they match:**

| object | 120 means | per window |
|---|---|---|
| `ResimGateProbe` | 120 divergence checks, i.e. 120 non-resim physics frames — `onCheckIsSimilar` runs once per such frame, so one window per 2 s at 60 Hz | 3 Warning lines |
| `CorrectionLandingProbe` | 120 correction events across both classes and both landed/discarded outcomes | 2 Warning lines |

The landing window is one step wider than the verdict probe's drive, because a DISCARD is an
observation here (§19) where for the verdict probe it was a non-event. So this window closes
slightly sooner than its divergence-probe neighbour on the same run. **The two are not required to
align — only to be the same size.**

Five Warning lines per window per category against a commissioned budget of six. Verbose adds a
`[ResimProbe.Request]` line per trigger, a `[ResimProbe.Stranded]` line per episode, a
per-correction `[ResimProbe.Landing]` line, and — from the reconciliation peer rather than from this
file — `[ResimProbe.SlotMap]`.

**The two policy spellings, so a reader can grep the configuration**: the enumerators are
`FrontierExact` and `OnDisagreement`. Which of the two the compiled default is, and which the
shipped configuration selects, is stated once at `TimeConfig::resimTriggerPolicy` and is
deliberately not repeated here — see the note at the head of this file.

**Integer parts-per-thousand, never floats and never accumulating averages.** Every derived figure
on both summaries is `numerator * 1000 + denominator/2` over `denominator`, so a Catch2 assertion
and a log parser reproduce the same digits. This is the `CorrectionVerdictProbe` rule adopted
verbatim, and it is why no ratio here is a `float`.

## 4. I1 — the denominator, and the hole it fills

The counters this file exists to make readable were being read against **no denominator at all**.
The pre-existing `[ResimCheck.IsSimilar]` line is literally the "no resim needed" branch — the
denominator — and it emits at `Log` under a category that ships at `Warning`. It therefore has zero
occurrences in every log on disk. Trigger counts were being compared against nothing.

`checks` always equals the window length, because the window is driven by it. It is reported anyway,
exactly as `CorrectionVerdictWindowSummary::samples` is, so the line is self-describing.

⚠ **Its failure mode is therefore not `checks=0`.** A broken instrument shows up as *no
`[ResimProbe.Gate]` line existing at all* — which is precisely how the old dead line failed, and
which is also the **correct and expected** state on the authority, where no prediction runs and no
correction cache is allocated.

## 5. I1 — the trigger rate, and the ratio that started this

`requestRatePerMille` is `requested / checks`. The archived baseline against which every later
reading is compared:

| quantity | archived value |
|---|---|
| corrections per client run | **4,552**, every one judged `correct=0` |
| resim triggers in the same run | **59** |
| ticks per run | ~2,700 |
| source | the private-archive investigation `finding_task31_resim_rate.md` |
| trigger rate | **~22 ‰**, i.e. 1–3 % |

Two prior explanations for that ratio — arrival-driven triggering, and rollback-window coalescing —
miss it by 4–20×. Coalescing is real and insufficient: replays are ~1–2 ticks deep (§11) and account
for about **1.5 %** of the gap. What the ratio actually measured is in `docs/History-ResimGate.md`;
what matters *here* is that 22 ‰ is the number a new reading is compared against, and that it was
taken under the compiled default policy.

## 6. The surviving-anchor count

A correction that lands on the game thread **mid-replay** raises the pending anchor past the value
`prepareResimAll` captured. The completion edge then tries to consume the anchor with a
compare-and-swap against the captured value, the compare fails, and the anchor **survives**. That is
the mechanism working, not a defect: the gate stays open and re-requests next frame with a
*different* anchor. `SimulationReconciliation::consumeResimAnchorsAll` carries the full argument.

**Three properties that are easy to get wrong when reading it:**

1. **It is fed from the apply edge**, beside `noteFinish` in `SimulationManager::onPostGameSimulation`
   — *not* from `noteCheck`'s call site like `checks` / `declined` / `requested`. It still lands in
   the right window because `noteCheck` alone drives the window, and every other apply-edge counter
   (`prepares`, `finishes`, `replayTicks`, `replayOverruns`, `freshClobbersAvoided`) already
   accumulates the same way.
2. **It is surfaced on `[ResimProbe.Gate]`, by explicit ruling**, not on `[ResimProbe.Apply]` where
   its call site would otherwise put it. A surviving anchor is a property of the *gate* — it stays
   open — not of the apply edge that happened to observe it.
3. **A nonzero reading should be visible in `requests` without a matching `repeatRequests`**,
   because the next request's anchor differs from this one's. A nonzero here *with* a matching
   `repeatRequests` is a different story and worth investigating.

⚠ **Expect nonzero, possibly large, on a shipped run.** It was structurally near-zero only under the
compiled default. See §20 for why the header used to say the opposite.

## 7. The depth-policy exclusion count

When a character reports `needsResimulation()` but its pending anchor sits more than
`TimeConfig::rollbackWindowTicks` below that character's own prediction frontier,
`checkDivergenceAll` **excludes it from the min fold** rather than clamping it, and counts the
exclusion.

**Why skip and not clamp.** Clamping would restore at an uncorrected mid-window slot and replay the
identical prediction from it — a no-op that costs a full physics rewind. The anchor is out of reach;
pretending otherwise buys nothing and spends a rewind. The soft cap is a *circuit breaker*, and a
breaker that trips into a no-op is worse than one that refuses.

**Character-frames, not distinct anchors.** A stranded deep anchor is re-examined on every frame it
stays stranded, so the counter climbs while one anchor sits out of reach. That is the intended
reading — *an anchor is stuck out of reach* — and it is the same convention `refusedFrames` uses.

⚠ **Three spellings of one quantity, and a reader has to grep all three:** the summary field is
`deepAnchorExclusions`, the setter is `noteDeepAnchorSkips`, and the call site's local plus the
shipped configuration's own commentary use `deepAnchorSkips`. The header names all three. Unifying
them is a code change and is out of scope for a comment pass; it is routed rather than done.

⛔ **This counter is live on every run of this project.** The depth policy is consulted only under
the disagreement trigger policy — which the *compiled default* does not select and the *shipped
configuration* does. See §20.

## 8. I3 — the physics-rewind request / refusal ledger

**One adapter's binding, and why the header states it once.** The request and grant hooks, the
rewind-frame search, the depth merge and the refusal floor are all engine-side. For one adapter —
Unreal/Chaos — they are `FSimulationManagerAsyncCallback::TriggerRewindIfNeeded_Internal`,
`::FirstPreResimStep_Internal`, `FRewindData::FindValidResimFrame`, `FMath::Min` and
`BlockResimFrame`. Another adapter substitutes its own, and the engine-free Catch2 suite drives the
same door with no physics engine at all (`ResimGatePolicyTest.cpp`). The header declares that pair
once at the head of I3 and names roles everywhere below it.

**Why we count refusals ourselves.** One adapter compiles its own refusal diagnostics out of any
normal build (behind `DEBUG_REWIND_DATA` / `DEBUG_NETWORK_PHYSICS`), so a refused rewind is
**completely silent** and our side simply retries next frame, because nothing cleared
`needsResimulation()`. Four fields end that silence: `requests`, `grants`, `refusedFrames`,
`repeatRequests`.

**`requests` versus `requested` is a free wiring self-check.** The two differ only by the adapter's
own short-circuits — no manager, or a role that does not run prediction — so on a predicting client
they must agree. A persistent gap means one of the two hooks is mis-placed, and the check costs
nothing because both numbers are already being counted on opposite sides of the boundary.

**`refusedFrames` is valid as a refusal count**, and under the edge-triggered gate by a stronger
argument than it originally had. A refusal used to leave the gate open because no flag had moved; it
now leaves it open because nothing but the completion edge can consume an anchor. Same number, no
longer contingent on a flag discipline.

**The archived refusal band, with its population stated.** The four runs tabulated in the source
investigation read:

| run / client | trigger frames | rewinds granted | apply edge reached | refused (%) | granted-but-never-applied (%) |
|---|---:|---:|---:|---:|---:|
| t37 A | 59 | 41 | 32 | 18 (31 %) | 9 (22 %) |
| t37 B | 59 | 46 | 36 | 13 (22 %) | 10 (22 %) |
| t39_runB A | 146 | 121 | 98 | 25 (17 %) | 23 (19 %) |
| t39_runB B | 134 | 114 | 92 | 20 (15 %) | 22 (19 %) |

⚠ **15–31 % is the band of these four rows. Over all 18 archived client logs the investigation's own
figure is 10–31 %** — the header used to attribute the narrower band to the wider population, which
is corrected in §20. A drop toward 0 after a future fix is that fix's proof; a rise is a regression
alarm.

**`repeatRequests` is the refusal-run signature.** Repeated `[ResimCheck.Divergence]` lines carrying
the *same* `correctionTick` with no `[Resim.Prepare]` between them: the retry keeps hammering a
frame at or below the engine's refusal floor until a newer correction moves the anchor past the
block. Every refusal episode in the sampled log follows a prepare that produced no finish — the two
anomalies in §10 are one episode class, not two independent ones.

## 9. I4 — the domain-conversion pin

`clampedGrants` counts grants whose granted physics step differs from the frame we last requested. A
mismatch means the grant was **deepened**: an engine-side requester merged a deeper request in under
the depth merge, or the rewind-frame search's own validation walked down.

**A shallow clamp — a grant *later* than requested — is structurally impossible on this wiring, and
the argument is a property of the roles, not of any one engine:**

1. the rewind-frame search walks **downward**; its return set is
   `{no-frame} ∪ [earliest-retained + 1, requested]`. Never later.
2. the depth merge is a **MIN**: it can only deepen the frame or leave it.
3. the replay always starts at the granted frame, so the physics step this probe observes always
   equals the solver's resim step, and is `≤ requested`.

⚠ **Only rung 3 is checkable inside an engine, and it was checked in ONE adapter's source.** For
that adapter the frame search returns `{INDEX_NONE} ∪ [EarliestFrame+1, RequestedFrame]`; the depth
merge (`FMath::Min`) sits in that adapter's `FNetworkPhysicsCallback::TriggerRewindIfNeeded_Internal`,
fed by physics replication's `GetResimFrame` or `CompareTargetsToLastFrame`; and the replay-loop
push-data skip that could start a replay late is dead code there, so `PhysicsStep` always equals
`ResimStep`. **Another adapter must re-check
all three rungs.**

**A constant 0 here is a verified assertion, not an unexercised counter.** The detector is
direction-agnostic, but this project drives no engine-side requester today, so 0 is the correct live
reading. A nonzero is an engine-behaviour-change alarm — an engine upgrade, or a replicated physics
body starting to move our rewind depth — and never a tuning signal.

**The depth pair and the ±1 skew.** `minRequestDepth` / `maxRequestDepth` are
`lastCompletedStep - requestedChaosFrame` over the window's requests; healthy is 1–2, matching the
~1.7-tick mean replay span of §11. A **negative** depth is clamped to 0 rather than stored: it would
mean we asked to rewind to a frame at or ahead of the last completed one, which the ±1 skew in the
tick↔physics-frame mapper produces transiently. It is visible as `depthMin=0` beside a nonzero
`refusedFrames` — the skewed request lands on the engine's refusal paths, not on a moved grant.

⚠ **The two domains are separate parameters on purpose.** `lastCompletedStep` and
`requestedChaosFrame` are physics-domain frames and are signed, because the engine's are;
`anchorTick` is a simulation tick. Mixing the two is the exact mistake the parameter names exist to
prevent, and it is the mistake the ±1 skew makes cheap to commit.

## 10. I5 — the apply edge, and the defect it pins

`prepares` counts `SimulationManager::prepareResimulation` calls, one per granted rewind, so
`prepares` and `grants` must agree; they are counted on opposite sides of the adapter boundary for
the same reason `requests` / `requested` are. `finishes` counts the `[Resim.Finish]` edge —
`chaosIsResim && !clockIsResim` in `onPostGameSimulation` — the sub-step on which the clock's resim
cursor caught up to the frontier and `applyResimAll` ran.

`abandoned` is `prepares - finishes`: **granted resims whose apply edge never ran**, measured at
about 20 % in every archived run. Healthy would be 0. Day-one readings are not, and that is the
point — the counter pins the defect and will pin its eventual fix.

⛔ **Do not read a single window's `abandoned`.** A prepare in the last frames of a window whose
finish falls in the next one is charged here as abandoned *and* credits the next window with an
unmatched finish. That is boundary noise of at most 1, and over a 120-frame window at the archived
1–2 trigger frames per window it is the same order as the signal. Read it across several windows —
or read `stuckResimFrames`, which has no boundary term at all.

**`stuckResimFrames` is the boundary-noise-free reading of the same defect**, and its side effect is
real rather than bookkeeping. While `ClientPredictionClock::isResimulating()` stays true through
normal prediction frames, `SimulationManager::currentStep()` returns the stale resim step on the
game thread, so `sendLocalInputToAuthorityAll` stamps a stale tick until the next
`startResimulation` resets the cursor. Whether input dedup absorbs that is measurable rather than
assumable; the counter is what makes it measurable.

## 11. I6 — the replay span

`replayTicks` counts `onGameSimulationResimulation` calls. It locks in permanently the answer that
started the investigation: **replays are short.** Healthy is `replayTicks ~= finishes * 1.7` —
measured as 69 replay ticks over 41 passes, mode 1. If this ever reads `finishes × 12–20`, the
rollback window is genuinely being spanned and the coalescing explanation is back on the table.

`replayOverruns` counts `tryInsertingResimulatedState` discards: a replayed tick whose slot is no
longer in the cache window. It is already visible at Warning in the cache's own line; it is counted
here so it has a denominator. The archived baseline is 1–2 per **run**, so a nonzero *window*
reading is the over-replay / window-skew evidence.

⚠ **Its population is unchanged by the correction-protection fix of §12.** A slot *protected* from
the replay is not a discard, so every archived `replayOverruns` reading still binds. The two
mechanisms count disjoint events.

## 12. The hollow-anchor ledger

A correction writes authority state into a slot; an in-flight replay then reaches that tick and
would overwrite it. `resimGate::classifyResimSlotWrite` refuses the write and splits the refusals
into two populations, because they mean opposite things.

**`freshClobbersAvoided` is the live defect rate.** Each one is a replay tick that would otherwise
have overwritten authority state the resim was supposed to act on — the hollow trigger, counted. The
surviving anchor would then correctly trigger a follow-up resim, which would restore the clobbered
state and faithfully reproduce the same prediction: **the trigger real, the data it pointed at
gone.**

⛔ **Expect a rate, not an event, on every run of this project.** It is near-zero only under the
compiled default, whose anchor is a frontier-exact landing over a ~1–2 tick replay span; for a slot
in a span that short to be corrected, a correction must land there *after* the gate opened. The
shipped configuration selects the disagreement policy, and that is what turns it from an event into
a rate. §20 records the header's previous, inverted statement.

**`staleClobbersAvoided` is a wiring check, not a tuning signal, and it is free.** It must read 0 in
any single-character session: for one character the replay span `anchor+1..frontier` and the stale
condition `tick < capturedAnchor` are **disjoint**. <!-- lint-anchor-ignore: prose shorthand for the per-cache capture; the real parameter is capturedAnchorTick, and ResimGatePolicy.h's own reachability note uses the same shorthand --> The only reachable stale population is a
non-min character restored at the shared min whose span dips below its own captured anchor. So a
nonzero reading in a one-character session means the classifier is comparing against the folded min
instead of the per-cache capture — *not* that a new population appeared. The full reachability
argument lives at `resimGate::classifyResimSlotWrite`.

**Why both populations go through one call.** `noteCorrectionProtections` takes `fresh` and `stale`
together so it is impossible for a caller to report one and forget the other. The pair is only
meaningful together: fresh is the defect rate, stale is the hygiene rate, and the split *is* the
instrument.

## 13. Stranded episodes — the three numbers, and what they discriminate

`StrandedResimEpisode` is handed back so the caller can emit one Verbose line per episode rather
than per frame. The three numbers discriminate the two surviving candidate mechanisms behind §10's
missing apply edges:

| reading | mechanism |
|---|---|
| `replayedTicks` one short of `catchUpDeficit` | the ±1 skew in the tick↔physics-frame mapper |
| a larger shortfall | a game-thread correction landing mid-replay and perturbing the cache mid-scan |

**A third candidate was ruled out from engine source, and the ruling is adapter-local**, in the
private-archive design `design_task43_resim_gate_fix.md`. The
push-data history shortfall that could silently skip leading replay frames is dead code in one
adapter, because the replay entry check guarantees `RecordedPushData.Num() == NumResimSteps`, so
the skip condition reduces to `Step >= ResimStep`, which is always true there. **Another adapter must re-check it.** Ruling
it out invalidated **no** instrument: these three numbers still discriminate the two live
candidates, which is why the episode plumbing stayed.

## 14. Ordering, one-shot lines, and what the window deliberately does not reset

**The ordering skew.** The window closes inside `noteCheck`, which runs at the *top* of the frame's
gate evaluation. That frame's own request / grant / prepare / finish are recorded after the flush
and therefore land in the **next** window. The skew is at most one frame per 120 and is a **shift,
not a drop**: no ratio is biased by it and every event is counted exactly once. The alternative — a
separate end-of-frame flush hook — would have meant a second physics-thread entry point on the
adapter for no measurable gain.

**The one exception, and it is the reason `noteDeepAnchorSkips` has a call-order fence.** The depth
exclusion count is derived from the *same* `checkDivergenceAll` call that produces `declined` and
`requested`, and `noteCheck` records those into the window it then flushes. Recording the exclusion
after the flush would charge it to the next window, putting one frame's numerator and its
denominator on two different lines. So it is called *before* `noteCheck`, unconditionally, with 0 on
a normal frame.

**One-shot stranded lines.** `noteStuckResimFrame` always counts, but returns true only on the first
such frame of an episode. The stuck state can persist for many frames by construction, so a
per-frame line here would be exactly the log-volume defect this family is budgeted against.

**What `resetWindow` deliberately leaves alone.** The previous-request pair, the last-requested-frame
pair and the whole episode block are *not* cleared at the window boundary. All four describe an
event sequence that **straddles** it — a refusal run, a request awaiting its grant, a resim awaiting
its apply edge. Clearing them would drop exactly the events most likely to be the interesting ones,
and would silently under-report `repeatRequests` and `clampedGrants` once per window. The protection
counters, by contrast, *are* per-window: each is a completed observation of a single replay tick,
with no straddling sequence to preserve.

## 15. The test seam

`ResimGateProbe::fillSummary` has **no shipped caller**. It exists so a test — and a future
on-demand diagnostic — can read a *partial* window, which is the only way to observe a rate in a
session whose windows never complete. The suite reaches it through
`getDiagnostics().resimGateProbe()`. It is the same contract as `CorrectionVerdictProbe::fillSummary`.

**The five physics-thread feed sites, as grep handles**: `SimulationManager::onCheckIsSimilar`
(I1, via `noteDivergenceCheck` and `noteDeepAnchorSkips`), `prepareResimulation` (I5),
`onPostGameSimulation` (I5's apply edge, `noteFinish` and `noteSurvivingAnchors`),
`onGameSimulationPrediction` (I5's stuck frames) and `onGameSimulationResimulation` (I6). The I3/I4
pair is fed from the adapter instead.

The gate's own behaviour is pinned by `ResimGatePolicyTest.cpp` and `ResimGateSemanticsTest.cpp`;
this probe's counters are pinned by `ResimGateProbeTest.cpp`, which is where
`ClampedGrantsCountAnyRequestedVsGrantedMismatch` lives.

## 16. I2 — three buckets, and what the pair proves

`CorrectionLandingSite` splits a correction by **where it landed relative to the prediction
frontier**, in three buckets rather than two:

| bucket | condition | what it is |
|---|---|---|
| `Behind` | `tick < predictionTick` | the correction set `m_containsCorrectTick` on its own slot and adopted authority state into that slot |
| `AtFrontier` | `tick == predictionTick` | literally the same predicate as `resimGate::shouldSetPendingAnchor`'s frontier-exact arm |
| `Discarded` | the tick had no slot at all | above the newest, or older than the 60-slot cache window |

**`AtFrontier` shares its predicate with the policy deliberately.** Both this classification and the
frontier-exact arm are given the same `tick == getPredictionTick()` comparison, which is what makes
this probe's archived `atFrontier` counts the baseline that policy has to reproduce.

**`Discarded` is why an anchor can never be ahead of the frontier.** No slot means no comparison, no
flag moved and no anchor set.

⛔ **The buckets classify a POSITION, not a TRIGGER**, which is why the switch to an edge-triggered
gate left them unchanged. The bucket *definitions* are untouched by which policy is in force; only
what a bucket **implies** changes. Under the compiled default a behind-frontier landing triggers
nothing; under the shipped configuration a *disagreeing* landing there sets the anchor, and `Behind`
becomes the trigger population rather than the suppressed one. **Do not read a
`requested` / `atFrontier` ratio without first establishing the session policy.**

**What the pair proves.** `landedBehind` large while triggers track only `landedAtFrontier` **is**
the demonstrated under-resimulation statement: relative to the design intent *resimulate when an
authoritative correction disagrees with what we predicted*, the system resimulates on a
clock-alignment artifact instead. The archived measurement of that, under the compiled default, was
roughly **9,100–9,400 behind-frontier landings against 1 and 0 resims** across two clients — the
frontier-exact landing count exactly.

⚠ **Whether it MATTERS is deliberately not answerable from here.** Are the suppressed corrections
micrometres or metres? `isSimilarTo` is a boolean fold that discards the distance it folded over, so
this instrument structurally cannot say. Divergence magnitude at the comparison site is a separate,
still-open question and is out of scope for these counters.

**`atFrontierRatePerMille` is the frontier-touch rate.** Under the compiled default it and
`[ResimProbe.Gate]`'s `requestRatePerMille` are the two halves of the central claim and should move
together. It reads **zero when the class saw nothing in the window — no observation, not a perfect
record** — which is why the caller skips a class block with no events rather than printing zeros
that would read as the opposite of "no data".

## 17. The class test, and why there is no per-id state

**Per class, never pooled, and the class test is PROVIDER-PRESENCE**, read live through the
input-resolution peer's `isLocallyControlled` rather than captured at bind time, so it cannot drift
from the map that decides behaviour. It is the same lookup `registerPredictionOwner` and
`collectInputAll` fork on, reached through the same `PredictedCharacterClass` enum
`CorrectionVerdictProbe` uses. **Never a second notion of "remote"** — see §1.

**No per-id state, hence no `forgetOwner`.** The summary is a per-class aggregate across characters,
so unregistering a character leaves nothing behind to erase. Do not copy the relay probes' per-id
watermark teardown here looking for symmetry; `CorrectionVerdictProbe` states the same rule for the
same reason, and the asymmetry is intended.

## 18. `classifyCorrectionLanding` — a free function, and one ordering rule

The classification is a free function so the **one** definition of "at the frontier" is shared by
the shipped call site and by every test, and so it can be exercised with no cache, no owner and no
simulatable.

⛔ **`landed == false` wins outright.** A discarded correction has no slot, so comparing its tick
against the frontier would classify an event that never happened. The `landed` flag is therefore
tested first and unconditionally.

The shipped call site computes the class and the site **unconditionally, before `landed` is
consulted**, because the landing probe needs both on the path the verdict probe returns early from.
That is why `landed` is a field on the arrival decision rather than a mid-block `return`.

## 19. Discards are samples, and log spellings are defined once

⛔ **`CorrectionLandingProbe` counts discards as samples**, and that is the one place it deliberately
differs from its `CorrectionVerdictProbe` neighbour, which excludes them because they produce no
verdict. Here the discard **is** an observation of the correction stream's alignment against the
frontier — the `aboveNewest` population is nothing but discards — so excluding them would hide the
very class this instrument was added to see.

`correctionLandingSiteName` is defined once, beside the enum, so the spellings an operator greps for
cannot drift between the shipped emitter and a test. It is the same reason
`predictedCharacterClassName` lives beside its own enum.

## 20. ⛔ Corrections — claims that were false or imprecise in the header, and the evidence

Recorded here rather than silently rewritten, because a compressed false claim is shorter, denser
and more authoritative than the original. Each row was checked against the tree before the sentence
it replaced was compressed.


| # | claim as the header stood | what the tree says |
|---|---|---|
| **F-36-1** | The depth-exclusion counter is *"STRUCTURALLY 0 UNDER THE SHIPPED CONFIGURATION … the compiled default is `FrontierExact`, so item 45 landed with no path to a nonzero. Item 46's flip is what makes it live"* | **FALSE, and inverted.** The `ResimTriggerPolicy` key under `[OGNetcode]` in the host application's configuration file is set to `OnDisagreement`, **uncommented**, so every run of this project selects it. `resimGate::policyEnforcesDepthCeiling` returns true only for that policy, so the ceiling is **armed** and the counter is live. It is structurally 0 only under the *compiled default*. "Item 46's flip" also mis-attributes the change: that item is still open, and the key was set as a cost-measurement experiment and then blessed. |
| **F-36-2** | `freshClobbersAvoided` is *"STRUCTURALLY NEAR-0 UNDER THE SHIPPED `FrontierExact` DEFAULT … Item 46's flip to `OnDisagreement` is what makes it a rate rather than an event"* | **Same defect, second site.** `FrontierExact` is the compiled default, never "the shipped default". Under the shipped configuration this is a rate today. |
| **F-36-3** | The `Behind` bucket is *"STILL TRUE ON A DEFAULT BUILD … the shipped `FrontierExact` policy sets no anchor for a behind-frontier landing"* | **Same defect, third site.** The parenthetical *"by configuration rather than by mechanism"* was right; the word **shipped** attached to the wrong half of the pair. |
| **F-36-4** | ⛔ **THE FILE CONTRADICTED ITSELF.** The surviving-anchor block says *"under the shipped `OnDisagreement` policy … it was structurally near-0 only under the legacy `FrontierExact` default"* | **That block is correct and current**, and it sits ~180 lines above F-36-1's block, which says the opposite. One later edit updated one site of four. Two *other* files in this repository already carried the corrected pair — `TimeConfig::rollbackWindowTicks`' own fence states the disagreement policy *"ships"*, and `SimulationNetSync`'s landing block says the two *"differ"* and points at `TimeConfig::resimTriggerPolicy` rather than re-deriving. **The defining header was the last file in the tree still stating it backwards.** |
| **F-36-5** | *"Fed from the OnRep-dispatched correction callback bound in `SimulationNetSync::registerPredictionOwner`"* | **True but no longer the useful handle.** The binding site is still that method, but the probe is now a member of `NetSyncTelemetry` and its immediate feeder is `NetSyncTelemetry::emitCorrectionArrival`, given a decision `SimulationNetSync::decideCorrectionArrival` computes. A reader following the old sentence lands two hops from the call. Both ends are named now. |
| **F-36-6** | *"Measured healthy baseline: 15-31 % refused across all 18 archived client logs"* | **Over-narrow range attributed to a wider population.** 15–31 % is the span of the **four** runs the investigation tabulates; its own text gives **10–31 %** for the archived runs generally. §8 now states both with the population each belongs to. |
| **F-36-7** | *"the engine-agnostic / no-simulatable / no-logger testability property that `RelayReadProbe.h` states is preserved **verbatim**"* | **The word *verbatim* is false.** That property's third clause is *no other OGSimulation header*, and this file includes one. Everything the clause protects survives, because the include is transitively `<cstdint>`-only — but not the clause. §1 states the distinction. |

⚠ **Two claims checked and confirmed, recorded because confirming them cost as much as refuting one.**
`ResimGateProbe::fillSummary` really has no shipped caller — the only external callers anywhere are
in `ResimGatePolicyTest.cpp`. And `RemoteMoveQueue` really does still exist and really does follow
the hands-back-a-summary convention the orientation block attributes to it; it was a plausible
casualty of the input-container renames and it is not one.

## 21. Honest gaps — what this document cannot tell you

- **The archived figures are not re-measured here.** They come from a private-archive investigation
  and its diagnosis pack. Nothing in this repository can regenerate them, and no run since has been
  taken under the compiled default at the same verbosity. Treat them as dated baselines.
- **Rungs 1 and 2 of §9 are arguments, not measurements**, and rung 3 was checked inside one
  adapter's engine only. A second adapter re-checking them could refute the impossibility claim.
- **The magnitude question of §16 is open**, and this instrument cannot close it by construction.
- **Nothing here establishes that a resim is *good*.** These counters measure whether the gate fires
  when the design says it should. Whether firing more improves prediction quality is a separate
  measurement, and the archived attempt at it was structurally unable to answer, because the verdict
  it depended on was degenerate.
