<!-- SPDX-License-Identifier: MPL-2.0 -->
# Diagnostics conventions: `getDiagnostics()` / `editDiagnostics()`

This is the one place the diagnostics vocabulary used across `og-simulation` is defined. Files that
carry a view or a fenced instrument point here with a one-line comment instead of restating this.

**If this file and a header disagree, the header is authoritative and this file is stale.** This doc
defines the vocabulary; the sites named below hold the truth about what they actually do. Fix this
file; do not soften a header to match it.

Origin: `ReviewNotes.md` RN-2/RN-3/RN-4/RN-5/RN-7/RN-9 (+ the RN-9 amendment) and
`ObservabilityGapAnalysis.md` §7.6, in the `og-netcode-v2-input-relay` initiative workspace. This doc
supersedes the RN-9 amendment's wording (below); it does not change any ruling's outcome.

> ⚠ **Those two documents are private working material and are NOT distributed with this
> submodule.** They are named here as provenance, deliberately unlinked; every claim this file
> *asserts* is anchored to a file in this repository and only to those. The declarations below tell
> `tools/lint/doc_anchor_lint.ps1` that the two names are intentionally unresolvable, so it
> enumerates them with this reason instead of reporting them as drift.

<!-- lint-external-ref: ReviewNotes.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: ObservabilityGapAnalysis.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->

## 1. The view convention

`get`/`edit` on the *outer* accessor is existing house style, not new: `editStorage`, `editState`,
`editResimGateProbe`, `editReconciliation`, `editQueryAdapter`, `editAllState`, `editServerClock`,
`editNetworkEstimator`, `editClientClock`, `editPhysicsComposite`. `getDiagnostics()` (const) /
`editDiagnostics()` (non-const) apply that same pair to diagnostics.

Members *inside* a view carry no `get` prefix and no `Diagnostic` infix — the view's TYPE is the <!-- lint-anchor-ignore: `Diagnostic` here is a naming INFIX under discussion, not a type this file claims exists -->
marker. `cache.getDiagnostics().slotLandingSeqNr(i)`, not
`cache.getDiagnostics().getDiagnosticSlotLandingSeqNr(i)`: the outer call already says "diagnostics", <!-- lint-anchor-ignore: the ANTI-example this rule forbids; `getDiagnosticSlotLandingSeqNr` must NOT resolve, and the day it does this convention has been broken -->

so repeating it on every member triples the word for no information. A nested view class has the
same access to its outer class's private members as any other member function — no `friend` needed.

## 2. The fence, restated on the axis that actually decides every case

The RN-9 amendment stated the rule as *"views group READ SEAMS, never WRITE SITES."* That phrasing
fails on two of the rulings it is supposed to govern: `editDiagnostics()` exists specifically to carry
a write (`scribbleStateProvenanceForFenceTest`, a test-only mutator), and the amendment's own verb
inventory classes `log*` as an instrument-write verb, while RN-7 moves `logSlotProvenanceAll` *into*
`getDiagnostics()`. Read/write is the wrong axis. The axis that classifies every case without
exception is **who calls it, and what breaks if it is deleted**:

> A member belongs in `getDiagnostics()` / `editDiagnostics()` iff it has **no role in the production
> frame path** — deleting it and every caller changes no production behaviour and no shipped
> telemetry. Read seams and test-only mutators qualify. Anything production code calls to **feed** an
> instrument — `note*`, the `emit*` helpers, `noteDivergenceCheck`, and any future instrument write
> site (Layer 1's `IDesyncDiagnosticSink` call included, once it lands) — is instrumentation. It stays
> on the production path and is fenced from grouping, no matter what verb it uses.

### Worked examples

| member | verdict | why |
|---|---|---|
| the 8 accessors on `StateCorrectionCache`'s views (task 52) | **IN** | read seams / a test-only mutator; deleting them changes nothing production reads |
| `scribbleStateProvenanceForFenceTest` | **IN**, via `editDiagnostics()` | a write, but test-only — no production caller |
| `logSlotProvenanceAll` (RN-7, task 56 — landed: made `const`, moved onto `getDiagnostics()`, renamed from its pre-RN-7 name) | **IN** | a `log*` verb, but its one production caller (`SimulationManager.h`) only logs — Verbose-only, decides nothing, machine-checked unreachable from any production output |
| `SimulationManager::noteDivergenceCheck` | **OUT** | private; one production caller, every frame; feeds `m_resimGateProbe` directly |
| the 13 `note*` instrument write sites (probe API + the manager) | **OUT** | production-called; deleting them deletes the shipped `[ResimProbe.*]` / `[RelayProbe.*]` / `[DivergenceProbe.*]` telemetry |
| `NetSyncTelemetry`'s four GT `emit*` helpers + `InputResolutionTelemetry`'s twelve PT `emit*` helpers (relocated from `SimulationNetSync`, task 79 / B3; split into the two siblings along the class's own two-thread banner, item 85 / design C.6) | **OUT** | different verb, same role — they format the Warning-level window lines this initiative reads; deleting them deletes shipped output. Neither the task-79 relocation onto a sibling object nor item 85's further split along the thread boundary changes the verdict: production still calls into every one of them, unconditionally, on the owning peer's hot path — see task 79's and item 85's impl notes for the full call-site inventories |
| Layer 1's `IDesyncDiagnosticSink` call, **when Stage 4 lands** | **OUT** | does not exist yet (`ObservabilityGapAnalysis.md` §7.6) — the rule is worded to cover it on arrival, not re-derived when it lands |

Getting this wrong in one direction deletes shipped telemetry (grouping a `note*`/`emit*` site);
getting it wrong in the other leaves a genuinely inert read seam scattered as a one-off `get*` instead
of grouped where a reader expects it. The "who calls it / what breaks" test resolves both directions
without a per-case exception.

## 3. The verb inventory

Four verbs currently mean "touch an instrument", and each has exactly one job:

| verb | means | where | ruling |
|---|---|---|---|
| `note*` | write to a probe | the probe types' own API (`noteCheck`, `noteGrant`, `noteDeepAnchorSkips`, …) and `SimulationManager::noteDivergenceCheck` | production write site — never grouped |
| `emit*` | format + write a shipped diagnostic line | `NetSyncTelemetry`'s four GAME-THREAD helpers + `InputResolutionTelemetry`'s twelve PHYSICS-THREAD helpers (RN-10/RN-11 and earlier; owned by `SimulationNetSync`'s sibling telemetry objects since task 79 / B3, split along the two-thread banner at item 85 / design C.6 — see §5) | production write site — never grouped; **reuse this verb for any future extraction of this shape, do not coin `report*`/`log*`/`diag*`** |
| `log*` | observation-side logging with no production role | `SimulationReconciliation::logSlotProvenanceAll` / `logSlotProvenanceFor` | groups under `getDiagnostics()`, `const` (RN-7, landed task 56) |
| *(the verb RN-7 retired)* | — | superseded by `log*` (RN-7, task 56 — landed); 0 occurrences of that verb codebase-wide as of this task. See `ReviewNotes.md` RN-7 for its history and the name it retired. |
| `outDiagnostic…` (out-param prefix) | names the reporting CHANNEL an out-param carries, never the fact it carries | `outDiagnosticVerdict` (RN-4), `outDiagnosticDeepAnchorSkips` (RN-5) | the parameter is diagnostic; the fact it copies (`isSimilarTo`'s verdict, the depth-policy skip) is production and must never be marked |
| `ResimSweepDiagnostics` (by-value return) | names the reporting VALUE a sweep hands back, no out-param involved | `postResimulationAll`'s return (RN-3, task 55 — landed: no longer an out-param; the sweep returns the struct by value) | the return is diagnostic; the sweep's write-back (`discards`/`freshProtections`/`staleProtections`) is production and the single-pass invariant must never be split (§4) |

## 4. The fact/channel fence

RN-3/4/5's shared hazard: an out-param that reports a fact sits directly beside the code that
*decides* the fact, and marking the wrong one silently disables production logic. Three canonical
examples, none of them touchable by this convention:

- **The `isSimilarTo` verdict** (`tryInsertingCorrectState`) feeds `resimGate::shouldSetPendingAnchor`
  — under the shipped `ResimTriggerPolicy=OnDisagreement` this boolean decides whether a resim runs at
  all. `CorrectionInsertVerdict`/`outDiagnosticVerdict` is a copy for reporting; the fact itself is not
  diagnostic and `isSimilarTo` must never be described as removable.
- **The depth-policy skip** (`checkDivergenceAll`'s `if (!withinDepth) { ++diagnosticDeepAnchorSkips; return; }`)
  — the `return` is the depth policy doing its job (item 45); only the `++` is observation. The four
  dual-use locals that feed both the decision and the log line (`needsResim`, `anchorTick`,
  `predictionTick`, `withinDepth`) are never marked, moved, or guarded behind a diagnostic branch.
  `checkDivergenceAll` keeps its `unsigned int* outDiagnosticDeepAnchorSkips` out-param rather than
  following `postResimulationAll`'s by-value `ResimSweepDiagnostics` return (RN-3, task 55): the two
  sweeps are neighbours but not interchangeable shapes — `SimulationReconciliationConcept` constrains
  `checkDivergenceAll`'s own return to `-> std::convertible_to<unsigned int>` (RN-5), which the sweep's
  return already carries as its trigger count, so a second value can only leave via an out-param.
  `postResimulationAll`'s concept has no such return constraint, which is what made the by-value struct
  available there and not here (`ReviewNotes.md` RN-5, 2026-08-13).
- **The sweep write-back** (`postResimulationAll`) — the sweep over characters is production; the
  counts it hands back (`discards`/`freshProtections`/`staleProtections`) are observation. The
  write-back and the count share one pass by invariant (separating them into two sweeps would let the
  counts describe different replay ticks) — that invariant is a production property, not a diagnostics
  one, and no view or rename may split the pass.

The rule in one line: **the marker names the reporting channel, never the fact it carries.** A pass
that marks the fact breaks the gate, the depth policy, or the sweep invariant it sits beside.

## 5. Classes carrying a view

`getDiagnostics()` / `editDiagnostics()` is a subsystem-wide convention across six classes, not a
local tidy-up on one:

| class | status | members |
|---|---|---|
| `StateCorrectionCache` (`CorrectionCache.h`) | **landed, task 52** | the 8 RN-2 accessors + `scribbleStateProvenanceForFenceTest` |
| `SimulationReconciliation` (`SimulationReconciliation.h`) | **landed, task 56; second member added for the input-history display** | `logSlotProvenanceAll` and `slotStateProvenance` — **no longer a one-member view.** It landed as a deliberate one, on the ruling that a convention with unexplained exceptions is worse than a view with one member; the display's per-tick read of the provenance column is the second member that ruling always allowed for. `slotStateProvenance(id, simTick)` routes through `findCorrectionCache` exactly as `getAppliedCaptureTickRef` does, so it answers `nullopt` for every id on the authority, where no correction cache is allocated at all; `nullopt` (no slot) and `SlotStateProvenance::Empty` (a slot nothing wrote) stay distinguishable. RN-3's counters were restructured off the out-param onto `postResimulationAll`'s by-value `ResimSweepDiagnostics` return (task 55) rather than routed through this view; RN-5/6's `checkDivergenceAll`/`consumeResimAnchorsAll` counters stay on their own out-param/return shapes (§4) and are not homed here either |
| `SimulationNetSync` (`SimulationNetSync.h`) | **landed, task 59; retargeted task 79; re-split item 85; `relayReadProbe()` left this view at item 87** | `relayArrivalProbe()`, `correctionVerdictProbe()`, `correctionLandingProbe()` (RN-9) — const-only, no `MutableDiagnostics`, **three members, not four.** Task 79 (B3) relocated the (then four) probe MEMBERS, the sixteen `emit*` helpers, and the per-window flush bookkeeping onto a sibling `NetSyncTelemetry` object (`NetSyncTelemetry.h`); item 85 (design C.6) split that sibling along its own two-thread banner into two objects — `RelayReadProbe` and the twelve PHYSICS-THREAD-ONLY `emit*` helpers moved to a new `InputResolutionTelemetry` object (`InputResolutionTelemetry.h`), the other three probes and the four GAME-THREAD-ONLY `emit*` helpers stayed on `NetSyncTelemetry`. Items 85/86 had `SimulationNetSync` temporarily own both telemetry siblings and forward `relayReadProbe()` through a two-hop delegation; **item 87 (peer promotion) deleted that forwarder** — `InputResolutionTelemetry`'s owner is now a real composition-root peer, so `relayReadProbe()` moved to that peer's own view (see the next row) instead of continuing to delegate through NetSync |
| `SimulationInputResolution` (`SimulationInputResolution.h`) | **landed, item 86 (scaffold); promoted to its own view's final home, item 87; second member added for the input-history display** | `relayReadProbe()` (RN-9's fourth probe) and `localInputCache` — const-only, and **no longer a one-member view.** `relayReadProbe()` delegates into `InputResolutionTelemetry`'s own const accessor; reached as `inputResolution.getDiagnostics().relayReadProbe()` — no NetSync hop. `localInputCache<T>(id)` returns a `const LocalInputCache<T::InputType>*` into `m_localInputCaches`, which this peer owns: `nullptr` for an id with no line — an unknown id, or one with no input provider — and never a throw. It stays on THIS class rather than fronting reconciliation's data, per §2's owner test |
| `SimulationManager` (`SimulationManager.h`) | **landed, task 59** | `resimGateProbe()` — the mutable write handle (formerly paired with it) stays directly on the class, unmoved; it is a production door, not a read seam. This accessor had zero callers anywhere before task 59 — see `ResimGate.Policy.TheResimGateProbeAccessorObservesTheShippedFeed` in `ResimGatePolicyTest.cpp` for the wiring proof the ruling required |
| `ClientPredictionClock` (`ClientPredictionClock.h`) | **landed with the input-history display's rate marks** | the seven event-seam reads — `skipCount`, `lastSkipTick`, `stallCount`, `lastStallTick`, `hardResyncCount`, `lastHardResyncFromTick`, `lastHardResyncToTick` — plus `scribbleEventCountersForFenceTest` on `editDiagnostics()`. **The class's FIRST view**, landing with seven members under the same ruling that allowed reconciliation's to land with one. The seven words are written PER EVENT inside `advancePrediction`'s branches — the skip branch, BOTH stall branches (tier debt and graduated), and the hard-resync branch — and are read by nothing in production; each branch stores its tick before its count, so a game-thread reader taking the count first can see a stale tick with a fresh count and never the reverse. Those WRITES stay on the production path and are fenced from grouping by §2's owner test exactly as `note*` is: they feed an instrument, and only the reads and the test-only mutator moved onto the view. The independence is machine-checked by a fence case named at the members' own declaration, on the model of `scribbleStateProvenanceForFenceTest`'s |

A one-member view is accepted by ruling (RN-7): the alternative — grouping some classes and leaving
others as bare `get*` because "there's only one" — is the inconsistency this convention exists to
remove. `SimulationReconciliation`'s and `SimulationInputResolution`'s views both landed under that
ruling and both have since grown a second member; the ruling is about what a view may START as, not
a ceiling on what it may hold.
