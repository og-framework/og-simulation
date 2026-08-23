<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationReconciliation.h` — rationale

The header keeps a one-line guard at every site that has one, plus an orientation block
naming the threads and the resim-cycle order. **This file holds the full text those guards
were compressed from** — the derivation, the rejected alternatives, the measurement records
and the migration history.

**If this file and `SimulationReconciliation.h` disagree, the header is authoritative and
this file is stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a guard into this file.** Every section below is the *expansion* of a fence
that still fires at its own line. If you find yourself deleting a one-liner because "it is
in the rationale doc now", the guard has stopped working — the person about to make the
change is reading code, not documentation.

> ⚠ **Every section body below is the VERBATIM pre-compression comment text**, emitted by
> `impl/pilot7/gen_doc_round2.py` rather than retyped, so this file cannot have quietly
> edited what it copied. Bracketed tags (`[item 45]`, `[T16]`, `RN-6`) are provenance
> labels that resolve only in the `og-netcode-v2-input-relay` archive — they are not
> references, and nothing in this repository resolves them.

⚠ **One adapter binding the quoted text names.** `og-simulation` is engine-free — it names no
game-engine type and is reached from a host engine only through `concept`s. Two quoted blocks below
name **`Chaos`**, which is one adapter's **physics engine**: the party that owns the rewind loop, and
whose rewind is global across every body, which is why `checkDivergenceAll` folds to a `min` across
characters before asking for one. The globalness of the rewind is the fact; the engine's name is one
adapter's binding, and another adapter substitutes its own. The quotations are byte-verbatim and are
not edited to say so.

<!-- lint-external-ref: pushPredictionInput -- RETIRED NAME (T16): the input-column writer wrapper; retired WITH the column, no successor, must not resolve -->
<!-- lint-external-ref: injectCorrectionInput -- RETIRED NAME (T8): the client half of the server->client correction-INPUT channel; retired, must not resolve -->
<!-- lint-external-ref: getLastCorrectionInput -- RETIRED NAME (T8): the correction-input reader; no successor, must not resolve -->
<!-- lint-external-ref: getLatestInput -- RETIRED NAME (T16): the second reader of the cache input column; no successor, must not resolve -->
<!-- lint-external-ref: Absent -- THE RULING'S NAME, NOT A C++ ENUMERATOR: the third outcome, split into NoSlot + NoRef in AppliedCaptureRefKind; it must not resolve -->
<!-- lint-external-ref: SimulationNetSync::collectInputAll -- DEAD OWNER: collectInputAll moved to SimulationInputResolution at item 87; the qualified form must not resolve -->
<!-- lint-external-ref: SimulationManagerUImpl::EmitOGLine -- ⛔ FALSE SYMBOL, quoted verbatim from the pre-compression header: no such member exists anywhere in the tree. The prefix routing is the free function RouteOGMessage in SimulationManagerUImpl.cpp. Filed as finding F-10; the compressed header no longer repeats it -->
<!-- lint-external-ref: prepareSimulationStep -- RETIRED NAME: collectInputAll carried it between items 90 and 94 only; must not resolve -->

---

## 1. `AppliedCaptureRef` — four kinds, three outcomes

**`SimulationReconciliation.h` pre-compression `:21-22` — NARRATIVE**

```
[og-netcode-v2-input-relay T6 / design D3] AppliedCaptureRef — the answer to
"which capture tick did the authority apply at tick T for this character".
```

**`SimulationReconciliation.h` pre-compression `:24-26` — FENCE**

```
Consumed by SimulationInputResolution::collectResimInputAll; this class
deliberately exposes NO cache and NO store, only this one answer — it
knows nothing about how resolution then resolves an input from it.
```

**`SimulationReconciliation.h` pre-compression `:28-30` — FENCE**

```
FOUR KINDS, THREE OUTCOMES. The ruling names three — ref / sentinel / absent —
and `Absent` is split in two here because the two halves must produce different
BEHAVIOUR at the call site while meaning the same thing to this class:
```

**`SimulationReconciliation.h` pre-compression `:32-49` — FENCE**

```
NoSlot   this tick is outside the character's cache window, so the character
is not part of this resim at all. Resim's state restore
(prepareResimAll) skips such a character for the same reason, so
handing the integrator an input for it would integrate an
un-restored state. The caller must emit NO input — which is exactly
what the pre-T6 body did by only emplacing on a cache hit.
NoRef    the slot exists but no correction has landed in it. Covers the
prediction FRONTIER (ticks newer than the last correction) and
correction HOLES (an intermediate tick whose correction was never
replicated — routine, since the net update rate is below the sim
rate). Both re-derive; neither is a sentinel.
Sentinel a correction landed and named NO capture: the authority substituted
an input (RemoteMoveQueue underrun, D1). Resolves to the injected
game zero — the value T17 made the authority actually integrate.
Ref      a correction landed carrying a real capture tick. `captureTick` is
that tick, and it WINS over any relay-entry schedule stamp
(RelayDelaySpectrumDesign.md §5.3 — see the precedence note at the
resolution site).
```

**`SimulationReconciliation.h` pre-compression `:63-63` — FENCE**

```
Meaningful only when `kind == Ref`; the sentinel otherwise.
```

---

## 2. `ResimSweepDiagnostics` — one sweep, one pass

**`SimulationReconciliation.h` pre-compression `:68-75` — FENCE**

```
[og-netcode-v2-input-relay item 55] ResimSweepDiagnostics — the whole report
of one `postResimulationAll` sweep, replacing the return value + two
defaulted out-pointers that carried `discards` / `freshProtections` /
`staleProtections` separately (RN-3, option B — see
docs/DiagnosticsConventions.md §4: the SWEEP that produces these is
production, this struct's fields are its report, and all three must keep
coming from the SAME pass — a second pass would run against a cache the
first one already mutated).
```

**`SimulationReconciliation.h` pre-compression `:77-81` — FENCE**

```
📌 `staleProtections` has never been observed nonzero in the field (item 43's
live run measured 0 on both clients, matching item 47's review prediction
that `ReplayedOverCorrection` is unreachable under protect-all). The field's
value is that it CAN move, not that it does — reading 0 is not evidence the
counter is broken, and it is not evidence it is unneeded either.
```

---

## 3. Cache lifecycle, and where the trigger policy lives

**`SimulationReconciliation.h` pre-compression `:119-119` — NARRATIVE**

```
Lifecycle — called by registration facade
```

**`SimulationReconciliation.h` pre-compression `:122-128` — FENCE**

```
[item 45] `try_emplace`, NOT `emplace`, AND THAT IS A REQUIREMENT NOW.
`emplace(id, Cache(m_logger))` built a temporary and MOVED it into the node;
`StateCorrectionCache` is non-movable since it acquired the atomic resim
anchor (a duplicated cache would give two objects one gate). `try_emplace`
forwards the ctor arguments and constructs in place, so no copy or move
exists to delete. `std::unordered_map` is node-based, so rehashing relinks
nodes and never moves the value either.
```

**`SimulationReconciliation.h` pre-compression `:130-133` — FENCE**

```
THE POLICY STAMP IS WHY THIS CLASS REMEMBERS THE POLICY AT ALL: characters
register mid-session, long after the composition root pushed the configured
value, so a cache created here must be born with it. See
`setResimTriggerPolicy`.
```

**`SimulationReconciliation.h` pre-compression `:673-679` — FENCE**

```
[item 45] The trigger-policy door. ONE-WAY PUBLICATION: `TimeConfig::
resimTriggerPolicy` is the source of truth, `SimulationManager::
setResimTriggerPolicy` is the only writable entry point, and this method fans
it out to every existing cache AND remembers it for caches created later
(`createCacheFor`). The remembered copy exists because character registration
happens long after composition; it is never written from anywhere else, which
is what keeps it from becoming a second source of truth.
```

**`SimulationReconciliation.h` pre-compression `:994-999` — FENCE**

```
[item 45] Visits every allocated cache of every simulatable type. Note it
walks the CACHE MAPS, not storage: the policy fan-out runs at composition,
before any character is registered, so a `forEachSimulatable` sweep would
reach nothing. It is a config-publication helper only — every per-tick sweep
in this class stays storage-driven so it visits characters in the storage's
order.
```

**`SimulationReconciliation.h` pre-compression `:1018-1021` — FENCE**

```
[item 45] The session trigger policy, remembered ONLY so that
`createCacheFor` can stamp a cache created after composition. Source of truth
is TimeConfig; see setResimTriggerPolicy. Default read from the TimeConfig
default, not a literal (R-P1).
```

---

## 4. The prediction push, and the input column that is gone

**`SimulationReconciliation.h` pre-compression `:148-149` — NARRATIVE**

```
Prediction push — the low-level PER-CACHE allocating primitive THE
FRONTIER-PAIR CONTRACT's opening half is built from.
```

**`SimulationReconciliation.h` pre-compression `:151-158` — NARRATIVE**

```
[og-netcode-v2-input-relay item 86, relocated item 94] The frontier-pair
contract's full text now lives at its FINAL HOME — `allocateFrontierSlotsAll`
below, the per-tick sweep that calls this method for every prediction-owned
id. See that banner for the predicate, the cache-population filter, the
blind spots, and why the push must not move to capture time. (Parked
above `SimulationInputResolution::collectInputAll` from item 84 through
item 93; item 94 moved the allocating SWEEP itself into this class,
alongside the slots it opens, and the contract text moved with it.)
```

**`SimulationReconciliation.h` pre-compression `:167-176` — FENCE**

```
[og-netcode-v2-input-relay T16] `pushPredictionInput` — the wrapper around
StateCorrectionCache's input-column writer — IS GONE with the column. Its
two production callers were both arms of what was then
`SimulationNetSync::collectInputAll` (item 90 renamed the peer's method
`prepareSimulationStep`; item 94 reverted that once allocation left the
method entirely — see `collectInputAll`'s own banner on
`SimulationInputResolution.h`); both now push only the prediction TICK,
which is what allocates the slot. A cache slot is state + the
applied-capture-tick ref; it holds no input value, and nothing in this
class can put one there.
```

**`SimulationReconciliation.h` pre-compression `:178-181` — FENCE**

```
Called on StepKind::Skip — back-fills the skipped tick with the prior state.
skippedTick is step.getTick() - 1 (the tick that was jumped over).
Must push the tick before the state: pushPredictionState writes into the slot
for the current prediction tick, so the tick advance must happen first.
```

**`SimulationReconciliation.h` pre-compression `:183-187` — FENCE**

```
[T16] The `pushPredictionInput(InputType{})` that used to sit between them is
gone with the column. Note it was a VALUE-INITIALISED input, not the injected
game zero — the (0,0,0)-forward poison T17 hunted elsewhere. Removing the
column removes that write entirely rather than having to fix it, since the
backfilled slot exists to be re-derivable, not to carry a remembered input.
```

---

## 5. The frontier-pair contract

**`SimulationReconciliation.h` pre-compression `:198-198` — NARRATIVE**

```
THE FRONTIER-PAIR CONTRACT — this method's ALLOCATING half of the pair.
```

**`SimulationReconciliation.h` pre-compression `:200-216` — FENCE**

```
[og-netcode-v2-input-relay item 84, relocated item 86 -> item 90's sweep
2 -> RELOCATED HERE AT ITEM 94] Both halves of the frontier pair now
live in the class that owns the slots: this sweep is the OPENING half,
`postPredictionAll` below (one screen away) is the CLOSING half, the
detector (`m_frontierSlotAwaitingState`) and the cache it guards are
both members of the class this method is defined on. Every tick this
method pushes via `pushPredictionTick` is completed by
`postPredictionAll`'s `pushPredictionState` later in the SAME manager
tick sequence (collect -> pre -> integrate -> post -> capture). Both
halves gate on ONE predicate, `stepAllocatesFrontierSlot(StepKind)`
(SimulationTimeContext.h) — never re-derive it. The pairing is why the
cache may stamp `Predicted` at allocation; a violation is caught by the
cache's `m_frontierSlotAwaitingState` check at the NEXT allocation
(CorrectionCache.h, write site 1). Frontier advance touches no gate
state (item 45); do not add gate writes here, and do not move this push
to capture time — frontier timing is what `FrontierExact`'s anchor-set
predicate is defined against (DesignInputResolutionPeer.md §B, §F).
```

**`SimulationReconciliation.h` pre-compression `:218-224` — FENCE**

```
[item 94] THREE PRODUCTION CALL SITES OF `stepAllocatesFrontierSlot`
STILL, NOT FEWER — but TWO OF THE THREE ARE NOW LOCAL TO THIS FILE, for
the first time: this method's own evaluation below, and
`postPredictionAll`'s early return. The third — the sweep-1 delay-line
capture-history gate — stays in `SimulationInputResolution::
collectInputForCharacter`, unmoved (see SimulationTimeContext.h's own
banner for the full enumeration).
```

**`SimulationReconciliation.h` pre-compression `:286-289` — NARRATIVE**

```
This method now JOINS `SimulationReconciliationConcept` — it is
reconciliation-DISTINGUISHING, alongside `postPredictionAll` and
`consumeResimAnchorsAll` (see this file's own concept, below, and the
proof namespace in `SimulationManager.h`).
```

**`SimulationReconciliation.h` pre-compression `:293-294` — NARRATIVE**

```
ONE evaluation for the whole tick — the same discipline item 90
established for the (now-deleted) resolution-side sweep 2.
```

**`SimulationReconciliation.h` pre-compression `:322-323` — NARRATIVE**

```
Post-prediction state push — called by SimulationManager after integrate
(replaces postSimulationAll from the retired SimulationNetworking class).
```

**`SimulationReconciliation.h` pre-compression `:328-334` — FENCE**

```
[og-netcode-v2-input-relay item 84, relocated item 94] THE
COMPLETING half of the frontier-pair contract — see
`allocateFrontierSlotsAll`'s banner above (one screen up, same file,
as of item 94) for the full text. Was a literal `== StepKind::Stall`
early return; now shares the one predicate both halves of the pair
gate on, so a new StepKind can no longer make the two
independently-written gates diverge.
```

---

## 6. The allocating sweep — filter, iteration, and what item 94 traded away

**`SimulationReconciliation.h` pre-compression `:226-236` — FENCE**

```
[item 94] WHY THIS SWEEP CAN FILTER ON ITS OWN CACHE POPULATION, WITH NO
REFERENCE TO RESOLUTION. "Prediction-owned id" is equivalent to "id with
a correction cache" for every fully-registered id, on both roles: the
client creates a cache for every registered id (cache-before-storage
ordering, `SimulationNetSync.h`'s client `registerSimulatable` overload);
the server creates none, ever (the server overload never calls
`createCacheFor`). Filtering on `findInputCache<T>(id) != nullptr`
therefore answers exactly "is this id prediction-owned" — no resolution
reference, no cycle, no id-set duplicated in a second place (this is the
fact that dissolves ground (i) of task 84's §B.3 caller-hoist rejection;
see the corrected paragraph there).
```

**`SimulationReconciliation.h` pre-compression `:238-250` — FENCE**

```
⛔ ITERATION MUST BE STORAGE-DRIVEN, FILTERED ON THE NULLABLE
`findInputCache` — NOT A BARE WALK OF THE CACHE MAP. A cache-map walk has
its own client-teardown hazard: if `storage.remove` ran before the cache
erase (it does not — item 93's reorder makes storage the FIRST thing to
stop exposing an id — but this sweep must not assume that ordering to be
safe), a dying id would still be swept — a `Skip` step's backfill would
read absent storage, and a `Normal` step would allocate a slot whose
capture (which also iterates storage) never completes, firing
`m_frontierSlotAwaitingState` as a FALSE POSITIVE BY CONSTRUCTION.
`for id in storage: cache = findInputCache(id); if (!cache) continue;`
has neither problem, and makes the allocate-set a SUBSET of the
capture-set by construction — both sweeps below and in `postPredictionAll`
visit exactly the same population, storage's, for exactly this reason.
```

**`SimulationReconciliation.h` pre-compression `:252-278` — FENCE**

```
⚠ WHAT THIS TRADES AWAY, PRICED HERE RATHER THAN HIDDEN:
1. ITEM 92's LOUD GUARD IS GONE. This class cannot distinguish "an
authority id (correct skip)" from "a half-registered prediction id
(broken invariant)" — both read as `findInputCache == nullptr`, and
both are now silently skipped where item 92's `OG_CHECK` used to
abort loudly in `SimulationInputResolution::
allocateFrontierSlotForCharacter` (deleted by this task). The
ordering invariants that guard describes remain enforced by items
92/93's landed reorders and the registration-site comments and
LLTs — but the RUNTIME BACKSTOP that turned a broken invariant into
an immediate, attributed abort is gone. If it reopens, this sweep
no longer tells you; it just quietly does not allocate for that id.
2. THE CAN'T-MISUSE-IT FUSION WEAKENS. `SimulationInputResolution::
collectInputAll` is now callable without a paired call to this
method — a caller who collects and lets capture (`postPredictionAll`)
run anyway pushes state into the OLD frontier slot: blind spot #2 of
the detector below (the reverse direction — state pushed without a
paired allocation — is legal at the cache and is covered only by
the shared predicate, never by `m_frontierSlotAwaitingState`). See
`collectInputAll`'s own banner for the successor obligation this
trade motivates. [item 96] `SimulationInputResolution::
preparePredictionSimulationStep` is the documented door that calls
this method paired with `collectInputAll`, on the register/
unregister facade precedent — default-correctness and
discoverability, NOT enforcement; both methods stay public and
independently callable, and this blind spot is unchanged by its
existence.
```

**`SimulationReconciliation.h` pre-compression `:280-284` — FENCE**

```
[item 90 / item 94] `[item 45]` RESTATED HERE, AT THE ALLOCATION SITE
ITSELF: frontier advance touches no gate state. `pushPredictionTick` and
`backfillSkippedTick` write the ring's slot directory and its state
buffer; neither reads nor writes `needsResimulation` or any other gate-
decision input. Do not add a gate write to this method.
```

**`SimulationReconciliation.h` pre-compression `:300-309` — FENCE**

```
[item 94] THE FILTER — see the banner above for why this alone
is sound: nullptr here means authority id, half-registered id or
half-torn-down id, all correctly and silently skipped by the
same test. Re-derived via the public nullable accessor rather
than a private cache-pointer cache, so `pushPredictionTick` /
`backfillSkippedTick` below re-look-up through `getCacheFor`'s
`.at(id)` — safe, because this line already proved the entry
exists; the double lookup mirrors the pre-94 loud-failure guard's
own shape (`findInputCache` check, then `getCacheFor` writes) and
is not a new cost.
```

---

## 7. `postPredictionAll` — the capture-side check that was rejected

**`SimulationReconciliation.h` pre-compression `:336-349` — FENCE**

```
[item 94, D-2] NO CAPTURE-SIDE `OG_CHECK` HERE, AND THAT WAS
AUDITED, NOT ASSUMED. A per-cache `getPredictionTick() ==
step.getTick()` check was considered to close blind spot (ii)
(state pushed without a paired allocation) and REJECTED: this
method's one production caller (`SimulationManager::
onPostGameSimulation`) always passes the SAME step
`allocateFrontierSlotsAll` just ran for, but a character whose
GAME-thread registration (`storage.add`, unsynchronized against
this physics-thread sequence) completes between this tick's
allocate call and this call is visible here with its cache still
at its just-constructed frontier — an ordinary player-join
condition indistinguishable, with the information available here,
from a real violation. See DesignInputResolutionPeer.md's D-2
section for the full audit and the named failing path.
```

---

## 8. `postResimulationAll` — gate state, discards, protections

**`SimulationReconciliation.h` pre-compression `:361-363` — FENCE**

```
Resim equivalent of postPredictionAll. Writes into the cache slot for the
resim tick (step.getTick()), not into the prediction-frontier slot that
pushPredictionState targets.
```

**`SimulationReconciliation.h` pre-compression `:365-373` — FENCE**

```
⛔ [item 45] IT NO LONGER TOUCHES GATE STATE, AND MUST NOT. This comment used
to end "also flips m_isResimulated on the slot so getLastResimulationTick /
needsResimulation can see the resim has progressed" — that bit, that scan and
that gate are all retired. A replay tick now writes STATE ONLY; the gate is
closed once, explicitly, by the CAS on the completion edge
(`consumeResimAnchorsAll`). This is the site that makes candidates A/B's
1-tick-resim storm structurally impossible instead of merely improbable: if a
replay tick wrote gate state, the gate would again be derivable from what the
replay just did, and a completed resim could re-trigger itself.
```

**`SimulationReconciliation.h` pre-compression `:375-382` — FENCE**

```
[og-netcode-v2-input-relay item 42] `discards` COUNTS replayed ticks whose
slot had already left the 60-slot cache window. Observational only; the
caller (SimulationManager) hands it to the resim-gate probe as I6's
`replayOverruns`. Behaviour is unchanged: every character is still swept,
the cache's own Warning line still fires per discard, and nothing in this
sweep's own logic reads its own tallies back. The baseline is 1-2 per RUN,
so a nonzero window reading is the over-replay / domain-skew evidence
finding §3 predicts and could not previously count.
```

**`SimulationReconciliation.h` pre-compression `:384-387` — FENCE**

```
⛔ [item 47] AND A REPLAY TICK NO LONGER OVERWRITES A CORRECTED SLOT. The
per-slot rule, its two invariants and the fresh/stale classifier live in
`resimGate::classifyResimSlotWrite`; what is visible from HERE is the
sweep's report of it, via `freshProtections` / `staleProtections`.
```

**`SimulationReconciliation.h` pre-compression `:389-393` — FENCE**

```
⚠ A PROTECTED SLOT IS NOT A DISCARD. `discards` keeps its item 42 meaning
exactly — replayed ticks whose slot had left the 60-slot window — because
`replayOverruns` has an archived baseline (1-2 per RUN) that a redefinition
would silently invalidate. Protections are a different population and get
their own two counters.
```

**`SimulationReconciliation.h` pre-compression `:395-402` — FENCE**

```
[item 55] ALL THREE TALLIES NOW TRAVEL ONE MECHANISM — the
`ResimSweepDiagnostics` returned by value (RN-3, option B; the two
out-pointers this used to take are gone). ⛔ ONE SWEEP, ONE TICK, ALL
THREE COUNTS: they are derived from the same per-character pass below, and
computing any of them in a follow-up pass would run against a cache this
pass already mutated — same failure mode as `noteDeepAnchorSkips` needing
to be called before `noteCheck`. See the type's own comment and
docs/DiagnosticsConventions.md §4.
```

---

## 9. `injectCorrectionState` — the applied-capture ref and the verdict channel

**`SimulationReconciliation.h` pre-compression `:427-427` — NARRATIVE**

```
Correction injection — called from OnRep_-dispatched lambdas (game thread)
```

**`SimulationReconciliation.h` pre-compression `:430-436` — FENCE**

```
[og-netcode-v2-input-relay T4 / design D3] The correction now carries a
SECOND scalar beside the tick: the capture tick of the input the authority
applied when it produced this state (kNoInputCaptureTick when it substituted
one). It is stashed in the SAME cache slot as the state it corrects, so a
later resim can ask "which input produced tick T" for every tick it replays
rather than only for the newest correction — the reason a single scalar
stash would not do (T6 consumes it).
```

**`SimulationReconciliation.h` pre-compression `:438-440` — FENCE**

```
Read through the buffer's getAppliedCaptureTick rather than a second
readInto: the ref is a fixed-offset header field, and the state read has
already paid for the composite.
```

**`SimulationReconciliation.h` pre-compression `:442-450` — FENCE**

```
[og-netcode-v2-input-relay T24] `outDiagnosticVerdict` forwards the cache's existing
prediction-vs-authority verdict to the caller. It is DEFAULTED and purely
observational — see CorrectionInsertVerdict in CorrectionCache.h for why the
verdict has to leave the cache at all (the cache is id-agnostic; the
attribution this initiative needs is by id AND by character class, and
neither fact exists down there). The marker names the reporting CHANNEL,
not the fact it carries — see `tryInsertingCorrectState` in CorrectionCache.h
and docs/DiagnosticsConventions.md §4 for why the underlying verdict is
production, not diagnostic.
```

**`SimulationReconciliation.h` pre-compression `:452-456` — FENCE**

```
THIS CLASS IS NOT THE PLACE THE VERDICT IS INTERPRETED, and deliberately so.
It knows the `id` but not whether that id is locally controlled or a remote
proxy — that is SimulationInputResolution::isLocallyControlled's job, and
reconciliation owning a second answer to it is exactly the duplicate-truth
shape the T6/T16 retirements exist to avoid. So this method only relays.
```

**`SimulationReconciliation.h` pre-compression `:470-476` — FENCE**

```
[og-netcode-v2-input-relay T8] `injectCorrectionInput` IS GONE. It was the
client-side half of the SERVER->CLIENT correction-INPUT channel: decode the
replicated (tick, input) payload, stash it in the cache slot. The whole
channel is retired — the server no longer writes it (sendCorrectionAll), the
component no longer replicates it, and StateCorrectionCache no longer has an
insertion point for it. A remote character's input now travels on the relay
ring and is resolved by CAPTURE TICK, not by application tick.
```

**`SimulationReconciliation.h` pre-compression `:478-479` — FENCE**

```
`injectCorrectionState` above is untouched and carries the T4
applied-capture-tick ref, which is what replaced this channel's usefulness.
```

---

## 10. `checkDivergenceAll` — the depth policy

**`SimulationReconciliation.h` pre-compression `:482-482` — NARRATIVE**

```
Divergence check — called by SimulationManager (replaces checkIsSimilarAll)
```

**`SimulationReconciliation.h` pre-compression `:485-488` — FENCE**

```
[og-netcode-v2-input-relay item 45] R1 — THE GATE READ, once per physics
frame, folded across characters with `min` because a Chaos rewind is global:
one restore tick has to serve every character, so it must be the OLDEST tick
anybody still needs replayed.
```

**`SimulationReconciliation.h` pre-compression `:490-496` — FENCE**

```
`maxAnchorDepthTicks` IS THE DEPTH POLICY, and 0 MEANS NO POLICY — the same
convention `resimGate::isAnchorWithinDepthPolicy` documents, so the caller can
hand it `rollbackWindowTicks` or 0 without a second boolean. The manager
decides which (`resimGate::policyEnforcesDepthCeiling`) and reads the value
live from its TimeConfig, exactly as `sendCorrectionAll` reads
`correctionRotationK` — caching it here would make an ini-driven setting
silently ineffective.
```

**`SimulationReconciliation.h` pre-compression `:498-507` — FENCE**

```
AN OVER-DEEP ANCHOR IS SKIPPED AND COUNTED, NEVER CLAMPED. Clamping would
restore at a mid-window slot no correction ever landed in, replay identical
inputs from identical state, and reproduce the same prediction: a guaranteed
no-op costing a full Chaos rewind. Skipping leaves the anchor PENDING — it is
not consumed here — so recovery is a newer correction raising it back inside
the window, or the HardResync failsafe. That is also why
`outDiagnosticDeepAnchorSkips` counts CHARACTER-FRAMES rather than distinct
anchors: a stranded deep anchor is re-examined and re-counted every frame it
stays stranded, which is the shape `refusedFrames` already uses for the
second gate and the reading that makes a stuck one visible.
```

**`SimulationReconciliation.h` pre-compression `:509-519` — FENCE**

```
⛔ THE SKIP IS PRODUCTION, THE COUNT OF SKIPS IS DIAGNOSTIC
(docs/DiagnosticsConventions.md §4). Below, `if (!withinDepth) { ++...;
return; }` — the `return` IS the depth policy doing its job (item 45): it
excludes this character from the min-fold. Only the `++` is observation.
`outDiagnosticDeepAnchorSkips` marks that reporting channel; it must never be
read as license to touch the four DUAL-USE locals that feed both the
decision and the `[ResimCheck.Check]` log line — `needsResim`, `anchorTick`,
`predictionTick`, `withinDepth` (the last comes from
`resimGate::isAnchorWithinDepthPolicy` and gates the skip itself). Marking,
moving, or guarding any of those four behind a diagnostic branch breaks the
depth policy silently.
```

---

## 11. `prepareResimAll` — capture for every character, and consume-all

**`SimulationReconciliation.h` pre-compression `:552-552` — NARRATIVE**

```
Resim restore — called by SimulationManager before resim replay
```

**`SimulationReconciliation.h` pre-compression `:555-563` — FENCE**

```
[item 45] R2 lives here: every cache CAPTURES the anchor it currently has
pending, so the completion edge can CAS against it. Captured for EVERY
character, including one whose slot for `simTick` is missing and which is
therefore not restored — deliberately, because that character is still
replayed forward by `integrateAll` and its UNCORRECTED slots are still
written by `postResimulationAll`, so its pending correction is consumed by
this resim whether or not the restore reached it. ([item 47] "its slots are
still overwritten" was true without qualification when item 45 wrote it;
corrected slots are now protected, which is the next paragraph's subject.)
```

**`SimulationReconciliation.h` pre-compression `:565-580` — FENCE**

```
⚠ [item 47] THE PARENTHETICAL THAT STOOD HERE IS NOW HALF FALSE, AND THE
HALF THAT CHANGED IS ITEM 47's WHOLE SUBJECT. It read: "that its corrected
state is OVERWRITTEN rather than applied is a PRE-EXISTING property of
anchoring the whole replay on a single min tick ... and is not something
this change alters". Item 45 did not alter it; ITEM 47 DOES. A replay no
longer overwrites any corrected slot, so the newer-anchored character's
authority state SURVIVES its shared replay — which is what makes the
follow-up trigger point at real data instead of at a re-derivation of the
prediction (the HOLLOW ANCHOR). What is STILL true, and is item 47's named
P1 policy rather than an oversight: that character's ANCHOR is still
consumed by this resim's per-cache CAS even though the resim restored at
the shared min and never applied its correction — consume-all, as shipped.
Recovery is the next rotation landing (<= ceil(N/K) ticks). The alternative
(P2, capture-the-restore-tick, a convergent cascade of <= N resims) is
priced by item 46's per-resim cost data and is deliberately NOT decided
here — see item 47's acceptance criteria for the flip condition.
```

**`SimulationReconciliation.h` pre-compression `:582-585` — FENCE**

```
[item 47] `captureResimAnchorForConsume` now captures TWO values: the
anchor for the consume CAS, and the landing-sequence baseline the
fresh/stale classifier compares against. One call, one instant — see the
note on that method.
```

---

## 12. `consumeResimAnchorsAll` — the consume edge

**`SimulationReconciliation.h` pre-compression `:601-604` — FENCE**

```
[item 45] W2 — THE CONSUME EDGE, swept across characters. Called from the
`[Resim.Finish]` block in `SimulationManager::onPostGameSimulation`, beside
`applyResimAll`, which is the one place where "a resim actually completed" is
known.
```

**`SimulationReconciliation.h` pre-compression `:606-614` — FENCE**

```
Returns the number of caches whose anchor SURVIVED the CAS, i.e. characters
for which a newer correction landed on the game thread mid-replay and which
will therefore re-trigger. That is the intended behaviour, not an error, and
the count exists so a future reader can tell "mid-replay landings are
frequent" from "the consume edge is broken" — the failure mode this design is
chosen for is fail-LOUD (the anchor stays pending and the gate retries,
visible in item 42's `repeatRequests` within one window) rather than the
legacy fail-silent (a broken flag discipline pins the gate shut and
under-resimulates for months).
```

**`SimulationReconciliation.h` pre-compression `:616-619` — FENCE**

```
⚠ IT MUST STAY ON THE COMPLETION EDGE. An anchor consumed at PREPARE time
would be lost if the resim never reaches its apply edge — item 42 measures
~20 % of prepares doing exactly that (the stranded-cursor class) — and the
correction it stood for would never be replayed by anybody.
```

**`SimulationReconciliation.h` pre-compression `:621-628` — FENCE**

```
⛔ AND THIS FUNCTION ITSELF STILL EMITS NO LOG LINE, deliberately: item 45
forbids new `[Resim.` / `[ResimCheck.`-prefixed lines outright (`[Resim.`
inherits `LogOGSim=Verbose`, T19's 10 MB defect) and a per-character-per-resim
line is exactly that volume class. A surviving anchor leaves the gate OPEN, so
the next frame requests again with a DIFFERENT anchor — visible in item 42's
existing `requests` without a matching `repeatRequests`, at Warning, in one
probe window; that cross-check is unchanged by the promotion below, it is just
no longer the ONLY way to see this count.
```

**`SimulationReconciliation.h` pre-compression `:630-651` — FENCE**

```
[RN-6, item 57 — LANDED] The RETURN VALUE is fed to `ResimGateProbe::
noteSurvivingAnchors` at the sole call site (`SimulationManager.h`'s
`[Resim.Finish]` block) and surfaced as a field on the existing
`[ResimProbe.Gate]` Warning line — no new log line, per the paragraph above.
⛔ THE RETURN'S DIAGNOSTIC PROMOTION DOES NOT MAKE THE CALL OPTIONAL. The CAS
inside this sweep is what closes the gate for every anchor that does NOT
survive (see the item-45 comment above `postResimulationAll`, this file,
"the gate is closed once, explicitly, by the CAS on the completion edge").
That is production-load-bearing regardless of whether anything reads the
count back — do not group this function under `getDiagnostics()` or
otherwise treat it as read-only/removable on the strength of its return
being diagnostic; RN-7/task 56 moved `logSlotProvenanceAll` there for the
opposite reason (its one caller only logs, decides nothing) and this
function is the shape that rule is meant to keep OUT.
⚠ HISTORICAL, kept so a stale rationale cannot mislead a future reader: this
comment used to end "...the alternative — asserting it through a probe field —
would add a shipped counter that reads 0 until item 46." Item 46
(`ResimTriggerPolicy=OnDisagreement`) shipped 2026-08-13, which made that
rationale stale on the same day (RN-6) — a surviving anchor is exactly the
population item 47's protect-all fires on, and item 43 measured ~1,400 fresh
protections per client per run, so the count was almost certainly nonzero and
discarded in the field the whole time.
```

**`SimulationReconciliation.h` pre-compression `:653-659` — NARRATIVE**

```
The SIGNATURE IS UNCHANGED — the concept constrains the return
(`{ t.consumeResimAnchorsAll() } -> std::convertible_to<unsigned int>`, this
file's own `SimulationReconciliationConcept`, below), so `void` was never
available; this only adds a reader. Its LLT readers — the two
`REQUIRE(... == 0u)` assertions in og-brawler-tests
`SimulationReconciliationTest.cpp` — keep passing unchanged, and a third case
there now constructs the surviving branch too (RN-6/item 57).
```

---

## 13. `applyResimAll` — the frontier-slot ruling

**`SimulationReconciliation.h` pre-compression `:686-695` — FENCE**

```
⭐ [item 47 amendment 3] THE FRONTIER-SLOT RULING LIVES HERE, AND IT NEEDS NO
CODE. This method publishes whatever the frontier SLOT holds. Item 47 stops
the replay overwriting a corrected slot, so if the frontier slot is
corrected at replay end, what this publishes into live state is the
AUTHORITY state rather than the replay's result — authority beats a
re-derivation of it at the one slot where the difference is externally
visible, exactly as it does everywhere else. That is a deliberate
behaviour change in what `[Resim.Finish]` publishes, and it is pinned by a
named case (`AFrontierExactLandingAtReplayEndIsWhatResimPublishes`) rather
than left implied.
```

**`SimulationReconciliation.h` pre-compression `:698-700` — FENCE**

```
Read from the prediction frontier — resim postPredictionAll writes into
the predictionTick slot every step, so the freshly-resimulated state lives
there, not at the earliest-resim slot.
```

---

## 14. The slot-provenance diagnostic view, and its one formatting site

**`SimulationReconciliation.h` pre-compression `:715-718` — FENCE**

```
[og-netcode-v2-input-relay item 48] THE SLOT-PROVENANCE DIAGNOSTIC VIEW —
the one SHIPPED reader of `StateCorrectionCache`'s diagnostic provenance
column, and the reason that column satisfies T16 instead of re-breaking
it.
```

**`SimulationReconciliation.h` pre-compression `:720-729` — FENCE**

```
[T53 / RN-7, task 56 — LANDED] `logSlotProvenanceAll` is `const` — it
returns nothing and mutates nothing observable, but has a side effect
(logging), so it is neither a plain `getDiagnostics()` read nor an
`editDiagnostics()` mutator by the letter. RN-7's ruling is that it
groups here anyway because the fence that actually decides every case is
"who calls it, and what breaks if it is deleted" (see
`docs/DiagnosticsConventions.md` §2), and this method has no production
role at all. This is a deliberate ONE-MEMBER view: a convention with
unexplained exceptions is worse than a view with one member today, and it
gives a home to any future diagnostics on this class.
```

**`SimulationReconciliation.h` pre-compression `:731-731` — NARRATIVE**

```
[Verbose][ResimProbe.SlotMap] id=%u frontier=%u map=<60 chars>
```

**`SimulationReconciliation.h` pre-compression `:733-739` — FENCE**

```
⚠ THE VOLUME, ROUTING, ORDERING AND DECIDES-NOTHING RULINGS ARE ALL AT THE
ONE FORMATTING SITE, `logSlotProvenanceFor` in this class's private
section — read that before changing anything here or adding a third caller.
The short version: Verbose-only on the existing `LogOGResimProbe`, so the
line does not exist at shipped verbosity; raw slot-index order; and it
feeds no decision anywhere, which is machine-checked one level down by
`…TheProvenanceColumnCannotReachAnyProductionOutput`.
```

**`SimulationReconciliation.h` pre-compression `:749-755` — FENCE**

```
CALL SITE 1 of 2 — ONE COMPLETED RESIM. Called from the `[Resim.Finish]`
block in `SimulationManager::onPostGameSimulation`, after `applyResimAll`
and the anchor consume, because that is the only point at which a replay's
whole effect on the cache is finished and visible: the span has been
written, the protections have been taken and the frontier slot has been
published. Emitting at prepare would show the map the resim was ABOUT to
change.
```

**`SimulationReconciliation.h` pre-compression `:774-774` — NARRATIVE**

```
Resim wipe — called via clock callback on hard resync
```

**`SimulationReconciliation.h` pre-compression `:783-785` — FENCE**

```
[item 48] CALL SITE 2 of 2 — ONE WIPE, and deliberately BEFORE it,
in the SAME sweep so the map cannot describe a different set of
characters than the wipe it accompanies.
```

**`SimulationReconciliation.h` pre-compression `:787-791` — FENCE**

```
⭐ AFTER THE WIPE THE MAP IS ALL `.` BY CONSTRUCTION, which is
knowable without reading a log and is therefore not a diagnostic.
The interesting map is the one a hard resync is about to DESTROY —
what lineage the cache carried at the moment the clock gave up on
it.
```

**`SimulationReconciliation.h` pre-compression `:793-798` — FENCE**

```
⚠ IT LIVES HERE RATHER THAN BESIDE THE `[TimeResync.Wipe]` LINE IN
`SimulationManager`'s resync callback for a mechanical reason worth
recording: that callback is instantiated by every LLT that builds a
`SimulationManager` on a MOCK reconciliation, so a call there would
have required adding this method to four mock types that model
nothing about it.
```

**`SimulationReconciliation.h` pre-compression `:943-945` — FENCE**

```
[og-netcode-v2-input-relay item 48] THE ONE FORMATTING SITE for the
slot-provenance map, shared by both call sites above so the two cannot
print different shapes for the same column.
```

**`SimulationReconciliation.h` pre-compression `:947-947` — NARRATIVE**

```
[Verbose][ResimProbe.SlotMap] id=%u frontier=%u map=<60 chars>
```

**`SimulationReconciliation.h` pre-compression `:949-957` — FENCE**

```
⛔ VERBOSE, AND THAT IS NOT NEGOTIABLE DOWNWARD. This is per-slot,
per-window data — 60 characters per character per emission. At Warning it
would be T19's 10 MB defect wearing a different tag. `LogOGResimProbe`
ships at `Warning`, so on a default run THIS LINE DOES NOT EXIST;
`-LogCmds="LogOGResimProbe Verbose"` turns it on beside the existing
`[ResimProbe.Request]` / `[ResimProbe.Stranded]` per-event lines — same
knob, same category, same family. NO new category and NO new ini key:
`[ResimProbe` already routes here by prefix
(`SimulationManagerUImpl::EmitOGLine`).
```

**`SimulationReconciliation.h` pre-compression `:959-962` — FENCE**

```
⚠ AT MOST ONCE PER COMPLETED RESIM AND ONCE PER WIPE, and that bound is
STRUCTURAL rather than a throttle — those are the only two callers. A
per-frame or per-replay-tick call would be exactly the volume class T19
was filed to stop. Do not add one.
```

**`SimulationReconciliation.h` pre-compression `:964-968` — FENCE**

```
⭐ WHY THE MAP IS IN RAW SLOT-INDEX ORDER (0..59) AND NOT TICK ORDER: the
column IS the ring, so ring order is the literal contents with no derived
ordering to go stale, and `frontier=` is printed beside it so a reader can
locate the newest tick. A tick-ordered map would be a second, computed
view that could disagree with the thing it claims to show.
```

**`SimulationReconciliation.h` pre-compression `:970-975` — FENCE**

```
⛔ IT DECIDES NOTHING AND MUST NEVER BE MADE TO. Deleting this method would
change no simulated value; the independence of the column it reads is
machine-checked one level down by
`…TheProvenanceColumnCannotReachAnyProductionOutput`. See
SlotStateProvenance.h for what fence 2's "production output" excludes and
why this line is not one.
```

**`SimulationReconciliation.h` pre-compression `:982-984` — FENCE**

```
+1 for the terminator. Sized from the cache's own constant rather than
a literal 60, so a future ring resize cannot silently truncate the map
into a half-truth.
```

---

## 15. The retired accessors, and what survives them

**`SimulationReconciliation.h` pre-compression `:805-806` — NARRATIVE**

```
Resim replay input — moved out by T6, home finished by item 87. See
SimulationInputResolution.h.
```

**`SimulationReconciliation.h` pre-compression `:809-810` — FENCE**

```
Do not re-add a resim collector here. If a future task needs one, it needs
the stores, and the stores are not this class's business.
```

**`SimulationReconciliation.h` pre-compression `:813-813` — NARRATIVE**

```
The cache input column — RETIRED. There are no input readers here.
```

**`SimulationReconciliation.h` pre-compression `:816-823` — FENCE**

```
[og-netcode-v2-input-relay T8] `getLastCorrectionInput` IS GONE, at this
level and on StateCorrectionCache. This section's old heading — "remote-
client input lookup, called by collectInputAll for simulatables with no
local input provider" — described the pre-T7 proxy branch, which now
resolves through the relay store. With the correction-input channel retired
there is nothing left to flag an input slot as server-sourced, so the
accessor could only have answered nullopt for the rest of time: a call that
compiles, looks authoritative and quietly always fails.
```

**`SimulationReconciliation.h` pre-compression `:825-829` — FENCE**

```
[og-netcode-v2-input-relay T16] `getLatestInput` IS ALSO GONE, and with it
the COLUMN both of them read. T8 left it standing under "remove what T8
makes FALSE, leave what T8 makes merely UNUSED" — it still returned real,
freshly-written data, it just had no caller. T16 removes the writer, so
there is nothing left to return.
```

**`SimulationReconciliation.h` pre-compression `:831-837` — FENCE**

```
WHAT SURVIVES THIS SECTION, and it is the point of the whole retirement:
`findInputCache` below (still the route for every accessor here — it is NOT
an input reader despite the name, which is pre-initiative), plus the two
applied-capture-tick queries. A slot answers "WHICH capture the authority
applied at tick T", never "what that capture WAS". The value comes from
LocalInputCache (local) or RemoteInputCache (remote), both owned by
SimulationInputResolution.
```

**`SimulationReconciliation.h` pre-compression `:905-910` — FENCE**

```
[T16] THE NAME IS HISTORICAL — it predates this initiative and no longer
describes anything about inputs. This is the nullable route every accessor
above takes, and in particular the route for getAppliedCaptureTickRef, which
is T6's join-key query and the single most load-bearing read in the resim
path. It was NOT retired with the input column. Renaming it is a separate,
purely cosmetic change and was deliberately not bundled into a deletion task.
```

---

## 16. The applied-capture queries

**`SimulationReconciliation.h` pre-compression `:839-840` — NARRATIVE**

```
[T4 / D3] The join key stored for `tick` — "which capture tick did the
authority apply when it produced the state it corrected us to at `tick`".
```

**`SimulationReconciliation.h` pre-compression `:842-846` — FENCE**

```
Returns nullopt when the tick is outside this character's cache window (no
slot to have stored anything), and kNoInputCaptureTick when a slot exists
but names no capture — the two are deliberately distinguishable: the first
means "cannot answer", the second means "the authority itself had no client
capture behind that tick" (D1), which T6 resolves to game-zero.
```

**`SimulationReconciliation.h` pre-compression `:848-849` — FENCE**

```
Routed through findInputCache (nullable) so it is safe to call on the
authority, where no correction caches are allocated at all.
```

**`SimulationReconciliation.h` pre-compression `:864-867` — FENCE**

```
THE ONE QUERY SimulationInputResolution::collectResimInputAll consumes —
the classified form of the raw accessor above. See the AppliedCaptureRef
block at the top of this header for what each kind means and why `Absent`
is two kinds rather than one.
```

**`SimulationReconciliation.h` pre-compression `:869-874` — FENCE**

```
WHY BOTH EXIST. getAppliedCaptureTick (T4) reports the SLOT VALUE and is the
accessor T4's own wire/stash tests assert against; it cannot tell a
correction that named no capture from a slot no correction ever reached,
because both hold the sentinel. This one adds that single discriminator (the
slot's correction flag) and is the only form safe to dispatch a resolution
table on. Neither exposes the cache.
```

**`SimulationReconciliation.h` pre-compression `:876-878` — FENCE**

```
Routed through the nullable findInputCache, so it is safe on the authority
(no caches allocated there) — where it answers NoSlot for every id, which is
correct: the authority never resimulates.
```

**`SimulationReconciliation.h` pre-compression `:890-892` — FENCE**

```
An uncorrected slot is NOT a sentinel — it is "no answer yet". Checking
the flag before the value is what keeps the prediction frontier (and any
never-replicated intermediate tick) out of the sentinel row.
```

**`SimulationReconciliation.h` pre-compression `:902-903` — NARRATIVE**

```
Returns a pointer to the correction cache for the given simulatable type and id,
or nullptr if no cache exists (e.g. on the authority, where caches are not allocated).
```

**`SimulationReconciliation.h` pre-compression `:933-936` — NARRATIVE**

```
[RN-7 / task 56] Const overload, added so `Diagnostics::logSlotProvenanceAll`
can look up a cache through a `const SimulationReconciliation&`. The
non-const overload above and its production callers (`applyResimAll`,
`wipeAllForResync`, the correction/prediction sweeps) are unchanged.
```

---

## 17. The concept

**`SimulationReconciliation.h` pre-compression `:1033-1036` — NARRATIVE**

```
[item 94] THE OPENING half of the frontier-pair contract, relocated
here from the resolution peer's tick concept — reconciliation-
distinguishing alongside postPredictionAll / consumeResimAnchorsAll
below, per this method's own banner.
```

**`SimulationReconciliation.h` pre-compression `:1040-1042` — FENCE**

```
[item 45] The gate read takes the DEPTH POLICY (0 == no policy) and reports
depth-skipped character-frames through a defaulted out-pointer. The manager
supplies both from its live TimeConfig.
```

**`SimulationReconciliation.h` pre-compression `:1047-1050` — FENCE**

```
[item 45] The resim-completion consume edge, and the trigger-policy door. Both
are named here because SimulationManager calls them, so a substitute
reconciliation without them would fail at the call site rather than at the
concept — which is the whole reason this concept exists.
```

**`SimulationReconciliation.h` pre-compression `:1053-1053` — FENCE**

```
collectResimInputAll moved again, to SimulationInputResolutionConcept.
```

---

## 18. Keeping this document true

Every section body is a **closed record**: the text a header comment carried before round 2
compressed it, dated 2026-08-21. It is not a claim about what the code does today — the
header is. That is deliberate, and it is what makes the bulk of this file un-rottable:
a record of what was written cannot become false, only less relevant.

⚠ **What it does NOT protect is the symbols those records name.** A rename lands here as a
stale identifier in a quoted block. The retired names above are declared for
`tools/lint/doc_anchor_lint.ps1`; a new retirement needs a new declaration in the same change.

⛔ **A clean lint run is not evidence that anything here is TRUE.** That checker resolves
names, and it has been demonstrated to return an identical clean verdict for a sentence and
for its negation. Read the header, not the lint result.

