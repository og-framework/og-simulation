#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

// PredictedCharacterClass — REUSED, NOT REDECLARED. See the class-test note on
// CorrectionLandingProbe below: a second, independently-derived notion of "remote"
// is the thing most likely to disagree with the first one after a future edit, and
// then the two halves of a summary would describe different populations while
// looking authoritative. This header stays STL-only either way: the included file
// is itself `<cstdint>`-only, so the engine-agnostic / no-simulatable / no-logger
// testability property that RelayReadProbe.h states is preserved verbatim.
#include "OGSimulation/Network/CorrectionVerdictProbe.h"

// ---------------------------------------------------------------------------
// ResimGateProbe / CorrectionLandingProbe — THE RESIMULATION GATE, MADE
// COUNTABLE AT SHIPPED VERBOSITY.
// (og-netcode-v2-input-relay item 42; instruments the mechanism established by
// `impl/finding_task31_resim_rate.md`.)
//
// INSTRUMENTATION ONLY. Nothing here feeds back into any decision: every counter
// is written by a call whose only other effect is a log line, and no gate, clock,
// cache or integrator reads one. Deleting this header would change no simulated
// value. In particular this file does NOT repair the mechanism it measures — item
// 42 produced an instrument, and item 45 produced the repair, as separate changes
// on purpose (this file's numbers are the baseline that repair had to reproduce).
//
// ---------------------------------------------------------------------------
// ⭐ THE MECHANISM THESE COUNTERS EXIST TO MAKE VISIBLE — read this before
// reading a number off a log line, because every field below is named against it.
//
// ⚠⚠ HISTORICAL AS OF ITEM 45 (2026-08-11). The mechanism described in the next
// three paragraphs is THE ONE THIS PROBE WAS BUILT TO MEASURE and the one every
// archived number below was measured under. It has since been REPLACED — see "WHAT
// THE GATE IS NOW", after it. Both are kept: the counters' names, their baselines
// and item 43's whole comparison table only make sense against the old mechanism,
// and a reader who assumes the new one when reading an archived line will
// mis-attribute every figure.
//
// `StateCorrectionCache::getLastResimulationTick()` scanned newest->oldest STARTING
// AT THE PREDICTION-FRONTIER SLOT (offset 0) and returned the first slot flagged
// `m_isResimulated` or `m_containsCorrectTick`. `pushPredictionTick()` COPIED the
// frontier's `m_isResimulated` bit into every newly allocated frontier slot, and
// `postResimulationAll` flagged every replayed slot INCLUDING the frontier. So after
// any completed resim the bit inherited forward tick after tick, the scan
// terminated at offset 0 with `lastResimulationTick == getPredictionTick()`, and
// `needsResimulation()` was pinned FALSE.
//
// A correction landing BEHIND the frontier set `m_containsCorrectTick` on its own
// slot and could never be seen by that scan, because the frontier's inherited flag
// shadowed it. `tryInsertingCorrectState` cleared `m_isResimulated` on the slot it
// landed in — so the ONLY in-play event that re-opened the gate was a correction
// landing EXACTLY ON the frontier slot.
//
// ⇒ **Resim frequency measured how often the correction stream touched the
//   prediction frontier exactly, not how often prediction diverged.**
//
// That is the whole reason this file exists. The archived evidence: 4,552
// corrections, every one judged `correct=0`, produced 59 resim triggers — a ratio
// that the two prior explanations (arrival-driven triggering; rollback-window
// coalescing) miss by 4-20x, because replays are ~1-2 ticks deep and account for
// ~1.5 % of it.
//
// ---------------------------------------------------------------------------
// WHAT THE GATE IS NOW (item 45; design_task43_resim_gate_fix.md §3 candidate D).
// EDGE-TRIGGERED: a landed correction SETS a per-character pending anchor tick (one
// atomic word, coalescing to the newest via a CAS-max), `needsResimulation()` is
// `anchor != 0 && anchor != frontier` with nothing derived from slot bits, and the
// resim-completion edge CONSUMES the anchor with a CAS — so a correction landing
// mid-replay makes that CAS fail, survives, and re-triggers. `m_isResimulated`, the
// scan and the inheritance are all retired.
//
// ⚠⚠ [item 48, 2026-08-12] ANNOTATION, NOT A REVERSAL — `m_isResimulated` NAMED
// ABOVE IS STILL RETIRED. What item 48 added is a separate, deliberately-fenced
// DIAGNOSTIC column, `StateCorrectionCache::m_stateProvenance` (an enum, not a
// bitset, and pointedly not the old name), whose independence from this gate is
// MACHINE-CHECKED by an LLT that garbage-fills it and asserts every production
// output is byte-identical. It feeds NO counter on this page and NO decision
// anywhere: the numbers below are unaffected and every archived reading still
// binds. Its one shipped reader is the Verbose `[ResimProbe.SlotMap]` line —
// same category as this family, off at the shipped `Warning`. ⛔ Do not wire it
// into a trigger or into a summary field here; see SlotStateProvenance.h.
//
// WHAT THAT MEANS FOR THESE COUNTERS — nothing was renamed, and two readings
// changed:
//   * WHICH LANDINGS TRIGGER is now a CONFIGURED policy
//     (`TimeConfig::resimTriggerPolicy`). Item 45 shipped it defaulted to
//     `FrontierExact`, which reproduces the historical mechanism's observable
//     behaviour, so `requested` still tracks `atFrontier` on a default build and
//     every baseline below still binds. Item 46 flips it to `OnDisagreement`, at
//     which point `requested` should track the DISAGREEMENT rate instead and
//     `atFrontier` is demoted from gate to irrelevance. If you are reading a log
//     whose `requested` does not track `atFrontier`, check the policy before
//     concluding the instrument is broken.
//   * `refusedFrames` KEEPS ITS VALIDITY, by a stronger argument than before: a
//     refusal used to leave the gate open because no bit had moved; it now leaves it
//     open because nothing but the completion edge can consume an anchor. Same
//     number, no longer contingent on a flag discipline.
//   * `deepAnchorExclusions` IS NEW and is item 45's only added field. Structurally
//     0 until item 46's flip — see its own comment.
//
// ---------------------------------------------------------------------------
// TWO OBJECTS BECAUSE THERE ARE TWO THREADS, AND THAT IS A CORRECTNESS PROPERTY.
// The same rule `RelayReadProbe` / `RelayArrivalProbe` / `FrameHealthProbe` state,
// for the same reason, and the finding's §1 thread note is why it applies here:
//
//   ResimGateProbe          PHYSICS thread. Fed from `SimulationManager::
//                           onCheckIsSimilar` (I1), `prepareResimulation` (I5),
//                           `onPostGameSimulation`'s apply edge (I5),
//                           `onGameSimulationPrediction` (I5 stuck frames),
//                           `onGameSimulationResimulation` (I6) — and, across the
//                           adapter boundary but STILL on the physics thread, from
//                           `FSimulationManagerAsyncCallback::
//                           TriggerRewindIfNeeded_Internal` and
//                           `FirstPreResimStep_Internal` (I3, I4). Window driven by
//                           CHECK COUNT.
//   CorrectionLandingProbe  GAME thread. Fed from the OnRep-dispatched correction
//                           callback bound in `SimulationNetSync::
//                           registerPredictionOwner` — the same site
//                           `CorrectionVerdictProbe` is fed from, and for the same
//                           reason (that is where the id and the class are known).
//                           Window driven by SAMPLE COUNT.
//
// They are SEPARATE OBJECTS with SEPARATE WINDOWS, never shared and never atomic.
// A single shared window would have one thread reset counters the other is
// mid-increment on, costing a whole window's totals rather than one sample.
//
// ⚠ THE COST OF THAT SPLIT, STATED SO NOBODY LOOKS FOR THE LINE THAT CANNOT EXIST:
// the under-resimulation reading wants `landedBehind` (game thread) next to
// `requested`/`grants`/`finishes` (physics thread) — and those cannot share one
// log line without sharing state across the two threads. So the reading is TWO
// adjacent Warning lines, `[ResimProbe.Landing]` and `[ResimProbe.Gate]`, each
// self-contained. `[ResimProbe.Landing]` alone already answers the question the
// finding poses (is there a large behind-frontier population that produced no
// trigger?), because `atFrontierRatePerMille` is the frontier-touch rate the
// trigger rate tracks; `[ResimProbe.Gate]`'s `requested` is what confirms it does.
//
// NEITHER OBJECT LOGS. They accumulate and hand back a summary struct; the caller
// owns the logger and does the SIMLOG. Same convention as RelayReadProbe,
// CorrectionVerdictProbe and RemoteMoveQueue — and it is what lets the Low-Level
// Tests assert on numbers rather than on strings.
//
// NAMESPACE NOTE: global namespace, matching the rest of the OGSim core.
// ---------------------------------------------------------------------------

// ONE WINDOW LENGTH FOR THE WHOLE PROBE FAMILY, TAKEN FROM THE EXISTING CONSTANT
// RATHER THAN RE-LITERALLED. Item 42 says "mirror LogOGDivergenceProbe's window
// mechanism and window length exactly (same constant source, do not invent a
// second window size)", and an `= 120u` here would have been a second source that
// drifts silently the day the first one moves. Aliasing keeps the two families
// comparable window-for-window in a log, which is the point: a
// `[ResimProbe.Landing]` line and a `[DivergenceProbe.Window]` line closing on the
// same denominator describe the same interval.
//
// WHAT 120 MEANS ON EACH THREAD, because the two units genuinely differ:
//   * ResimGateProbe        120 DIVERGENCE CHECKS == 120 non-resim physics frames
//                           (`onCheckIsSimilar` runs once per such frame), i.e. one
//                           window per 2 s at 60 Hz. 3 Warning lines per window.
//   * CorrectionLandingProbe 120 CORRECTION EVENTS across both classes and both
//                           landed/discarded outcomes — the same drive
//                           CorrectionVerdictProbe uses, one step wider because a
//                           DISCARD is an observation here (it is item 41's
//                           `aboveNewest` population) where for the verdict probe
//                           it was a non-event. So this window closes slightly
//                           sooner than its DivergenceProbe neighbour on the same
//                           run; the two are not required to align, only to be the
//                           same size. 2 Warning lines per window.
// Total <= 5 Warning lines per window per category, inside item 42's budget of 6.
inline constexpr std::uint32_t kResimGateProbeWindowSamples =
    kCorrectionVerdictProbeWindowSamples;

// ---------------------------------------------------------------------------
// I1 + I3 + I4 + I5 + I6 — the per-window summary handed to the caller.
//
// PLAIN `std::uint32_t` COUNTERS, no floats and no accumulating averages: every
// derived figure below is integer parts-per-thousand so it is exactly reproducible
// in a test and in a log parser (the CorrectionVerdictProbe rule, verbatim).
// ---------------------------------------------------------------------------
struct ResimGateWindowSummary
{
    // --- I1: THE DENOMINATOR. -------------------------------------------------
    // Physics frames on which `checkDivergenceAll` ran, split by outcome. This is
    // the hole item 31 named: the pre-existing `[ResimCheck.IsSimilar]` line —
    // literally the "no resim needed" branch, i.e. the denominator — emits at `Log`
    // under a category that ships at `Warning`, so it has ZERO occurrences in every
    // log on disk. Trigger counts were being read against no denominator at all.
    //
    // `checks` ALWAYS EQUALS the window length, because the window is driven by it.
    // It is reported anyway, exactly as CorrectionVerdictWindowSummary::samples is:
    // it is the denominator of `requestRatePerMille` and it is what makes the line
    // self-describing. Its FAILURE mode is therefore not "checks reads 0" but "no
    // [ResimProbe.Gate] line exists at all" — which is precisely the shape the old
    // dead line failed in, and which is also the expected, correct state on the
    // AUTHORITY (see the role note at the bottom of this comment).
    std::uint32_t checks = 0u;

    // `checkDivergenceAll` returned 0 — no character reported `needsResimulation()`.
    // Under the mechanism at the top of this file this is the overwhelmingly common
    // outcome, and it is common for a reason that has nothing to do with prediction
    // quality.
    std::uint32_t declined = 0u;

    // `checkDivergenceAll` returned a nonzero anchor tick — the gate opened and a
    // rewind was asked for. Always `checks - declined`.
    std::uint32_t requested = 0u;

    // requested / checks in PARTS PER THOUSAND, rounded to nearest. Measured
    // baseline for the archived runs: 59 trigger frames over ~2,700 ticks, i.e.
    // ~22 per mille (1-3 %).
    std::uint32_t requestRatePerMille = 0u;

    // --- [item 45] THE DEPTH-POLICY EXCLUSION COUNT. --------------------------
    // CHARACTER-FRAMES on which a character reported `needsResimulation()` but its
    // pending anchor sat more than `rollbackWindowTicks` below that character's own
    // prediction frontier, so `checkDivergenceAll` excluded it from the min fold
    // instead of clamping it (skip-not-clamp: clamping restores at an uncorrected
    // mid-window slot and replays the identical prediction, a no-op costing a full
    // rewind).
    //
    // CHARACTER-FRAMES, NOT DISTINCT ANCHORS, and the difference matters when
    // reading it: a stranded deep anchor is re-examined every frame it stays
    // stranded, so a sustained nonzero means "an anchor is stuck out of reach",
    // which is the reading that matters, and it is the same convention
    // `refusedFrames` uses one section down.
    //
    // ⛔ STRUCTURALLY 0 UNDER THE SHIPPED CONFIGURATION, and that is not an
    // unexercised counter: the depth policy is consulted only under
    // `resimTriggerPolicy == OnDisagreement`, and the compiled default is
    // `FrontierExact`, so item 45 landed with no path to a nonzero. Item 46's flip
    // is what makes it live, and it exists NOW so that flip needs no probe change —
    // the alternative is measuring the flip's cost with an instrument added in the
    // same diff, which is how this initiative lost four instruments already.
    std::uint32_t deepAnchorExclusions = 0u;

    // --- I3: THE CHAOS REQUEST / REFUSAL LEDGER. ------------------------------
    // The engine's own refusal diagnostics are compiled out behind
    // `DEBUG_REWIND_DATA` / `DEBUG_NETWORK_PHYSICS`, so in any normal build a
    // refused rewind is COMPLETELY SILENT and our side simply retries next frame
    // because nothing cleared `needsResimulation()`. That silence is what these
    // four fields end.
    //
    // Frames on which our `TriggerRewindIfNeeded_Internal` returned a chaos frame
    // (i.e. the request actually crossed into the engine). Distinct from
    // `requested` above only by the adapter's own short-circuits (null manager,
    // authority), so on a predicting client the two must agree — and that agreement
    // is a free wiring self-check across the core/adapter boundary. A persistent
    // `requests != requested` means one of the two hooks is mis-placed.
    std::uint32_t requests = 0u;

    // Requests for which `FirstPreResimStep_Internal` subsequently fired, i.e.
    // Chaos accepted and began a rewind.
    std::uint32_t grants = 0u;

    // requests - grants. VALID AS A REFUSAL COUNT because a refusal leaves
    // `needsResimulation()` true, so the request repeats next frame rather than
    // being lost — finding §4a. Measured healthy baseline: 15-31 % refused across
    // all 18 archived client logs. A DROP toward 0 after a future fix is that fix's
    // proof; a RISE is a regression alarm.
    std::uint32_t refusedFrames = 0u;

    // refusedFrames / requests in parts per thousand. Zero when `requests` is zero
    // — no observation, not a perfect record.
    std::uint32_t refusedRatePerMille = 0u;

    // Requests whose anchor tick equals the PREVIOUS request's anchor tick. This is
    // the refusal-RUN signature from finding §4a: `[ResimCheck.Divergence]
    // correctionTick=137` three times with no `[Resim.Prepare]` between, because
    // the retry keeps hammering a frame at or below Chaos's `BlockResimFrame` until
    // a newer correction moves the anchor past the block.
    std::uint32_t repeatRequests = 0u;

    // --- I4: THE DOMAIN-CONVERSION PIN. ---------------------------------------
    // Grants whose `PhysicsStep` differs from the chaos frame we last requested.
    // A mismatch means the grant was DEEPENED — an engine-side requester merged in
    // via `FMath::Min` (`FNetworkPhysicsCallback::TriggerRewindIfNeeded_Internal`:
    // physics replication's `GetResimFrame`, or `CompareTargetsToLastFrame`), or
    // `FindValidResimFrame`'s validation walking DOWN. A SHALLOW clamp — a grant
    // LATER than requested — is structurally impossible on this wiring, verified
    // end-to-end in engine source (item 42 review §2):
    //   1. `FRewindData::FindValidResimFrame` walks DOWNWARD; its return set is
    //      {INDEX_NONE} ∪ [EarliestFrame+1, RequestedFrame]. Never later.
    //   2. The `FMath::Min` merge can only deepen the frame or leave it.
    //   3. The replay-loop push-data skip that could start a replay late is dead
    //      code on this engine (`RecordedPushData.Num() == NumResimSteps` always),
    //      so the `PhysicsStep` this probe observes always equals the solver's
    //      `ResimStep`, which is ≤ the requested frame.
    // The detector is direction-agnostic, but this project drives no engine-side
    // requester today, so the live reading is a CONSTANT 0 BY CONSTRUCTION — a
    // verified assertion, not an unexercised counter. A nonzero is an
    // engine-behaviour-change alarm (an engine upgrade, or a replicated physics
    // body starting to move our rewind depth), not a tuning signal.
    std::uint32_t clampedGrants = 0u;

    // Requested rewind DEPTH in chaos frames (`lastCompletedStep - requestedChaosFrame`),
    // min and max over the window's requests. Healthy: 1-2, matching the measured
    // ~1.7-tick mean replay span. Both read 0 when `requests` is 0.
    //
    // A NEGATIVE depth is clamped to 0 rather than stored: it would mean we asked
    // to rewind to a frame at or ahead of the last completed one, which the ±1
    // ChaosTickMapper skew (finding §3) can produce transiently. It is visible as
    // `depthMin=0` next to a nonzero `refusedFrames` (the skewed request lands on
    // Chaos's refusal paths, not on a moved grant — see `clampedGrants` above).
    std::uint32_t minRequestDepth = 0u;
    std::uint32_t maxRequestDepth = 0u;

    // --- I5: THE APPLY-EDGE LEDGER. -------------------------------------------
    // `SimulationManager::prepareResimulation` calls. One per granted rewind, so
    // `prepares` and `grants` must agree; they are counted on opposite sides of the
    // adapter boundary on purpose, for the same reason `requests` / `requested` are.
    std::uint32_t prepares = 0u;

    // The `[Resim.Finish]` edge — `chaosIsResim && !clockIsResim` in
    // `onPostGameSimulation`, the sub-step on which the clock's resim cursor caught
    // up to the frontier and `applyResimAll` ran.
    std::uint32_t finishes = 0u;

    // prepares - finishes: granted resims whose apply edge NEVER RAN. Measured
    // baseline ~20 % in every archived run. Healthy would be 0; day-one readings
    // are not, and that is the point — the counter pins the defect and its
    // eventual fix.
    //
    // ⚠ WINDOW-BOUNDARY NOISE OF AT MOST 1: a prepare in the last frames of a
    // window whose finish falls in the next one is charged here as abandoned and
    // credits the next window with an unmatched finish. Over a 120-frame window at
    // the measured ~1-2 trigger frames per window that is the same order as the
    // signal, so DO NOT read a single window's `abandoned`; read it across several,
    // or read `stuckResimFrames`, which has no boundary term at all.
    std::uint32_t abandoned = 0u;

    // Normal prediction frames entered while `ClientPredictionClock::isResimulating()`
    // is STILL TRUE — the stranded-cursor state finding §4b derives, and the
    // boundary-noise-free reading of the same defect. Its side effect is real:
    // `SimulationManager::currentStep()` then returns the stale resim step on the
    // game thread, so `sendLocalInputToAuthorityAll` stamps a stale tick until the
    // next `startResimulation` resets the cursor. Healthy: 0.
    std::uint32_t stuckResimFrames = 0u;

    // --- I6: REPLAY-SPAN ACCOUNTING. ------------------------------------------
    // Resim replay ticks executed (`onGameSimulationResimulation` calls). This locks
    // item 31 step 0's answer in permanently: replays are SHORT. Healthy:
    // `replayTicks ~= finishes * 1.7` (measured: 69 replay ticks over 41 passes).
    // If this ever reads `finishes * 12-20` the rollback window is genuinely being
    // spanned and the coalescing explanation is back on the table.
    std::uint32_t replayTicks = 0u;

    // `tryInsertingResimulatedState` discards — a replayed tick whose slot is no
    // longer in the cache window. Already visible at Warning in the cache's own
    // line; counted here so it has a denominator. Baseline 1-2 per RUN, so a
    // nonzero window reading is the over-replay / window-skew evidence.
    //
    // ⚠ [item 47] ITS POPULATION IS UNCHANGED — a slot PROTECTED from the replay
    // is not a discard. Every archived `replayOverruns` reading still binds.
    std::uint32_t replayOverruns = 0u;

    // --- [item 47] THE HOLLOW-ANCHOR LEDGER. ----------------------------------
    // Replay ticks whose slot carried a correction and was therefore NOT
    // overwritten (`resimGate::classifyResimSlotWrite`), split into the two
    // populations that discriminator exists to separate. Character-slot events,
    // like `replayOverruns`: one replay tick can protect one slot per character.
    //
    // `freshClobbersAvoided` IS THE LIVE DEFECT RATE. Each one is a replay tick
    // that, before item 47, would have overwritten authority state the resim was
    // supposed to act on — the hollow trigger, counted. Its PRE-FIX twin was
    // never shipped: the fix and its instrument land together here because the
    // pre-fix counter would have had to be added, measured and removed inside one
    // regression window, and this initiative has already lost four instruments
    // that way. What makes the number honest instead is the LLT pair recorded in
    // the impl note: for each case, the input where the mechanism protects AND
    // the input where it does not.
    //
    // ⛔ STRUCTURALLY NEAR-0 UNDER THE SHIPPED `FrontierExact` DEFAULT, and that
    // is a prediction, not an excuse. Under the legacy policy the anchor is a
    // frontier-exact landing and the replay span is `anchor+1..frontier`, which
    // is ~1-2 ticks; for a slot in that span to be corrected, a correction must
    // land there AFTER the gate opened — i.e. the rare legacy mid-replay landing,
    // or a correction that arrived while a request was being refused (item 42's
    // 15-31 % class). Item 46's flip to `OnDisagreement` is what makes it a rate
    // rather than an event, which is exactly why item 47 lands BEFORE that flip:
    // a hollow resim repairs nothing, so 46's prediction-quality direction check
    // could read FLAT for this reason and be misread as "resims do not help".
    std::uint32_t freshClobbersAvoided = 0u;

    // ⭐ A 2+-CHARACTER-ONLY SIGNAL — IT MUST READ 0 IN ANY SINGLE-CHARACTER
    // SESSION, AND THAT IS A FREE CLASSIFIER-WIRING CHECK. For one character the
    // replay span `anchor+1..frontier` and the stale condition
    // `tick < capturedAnchor` are DISJOINT. The only reachable stale population
    // is a non-min character restored at the shared min whose span dips below its
    // OWN captured anchor. A nonzero reading in a 1-character session therefore
    // means the classifier is comparing against the wrong anchor (the folded min
    // instead of the per-cache capture) — not that a new population appeared.
    // Full reachability argument at `resimGate::classifyResimSlotWrite`.
    std::uint32_t staleClobbersAvoided = 0u;
};

// One stranded-resim episode, handed back so the caller can emit ONE Verbose line
// per episode. The three numbers together discriminate finding §4b's two SURVIVING
// candidate mechanisms: a `replayedTicks` one short of `catchUpDeficit` is the ±1
// ChaosTickMapper domain skew; a larger shortfall points at a GT correction landing
// mid-replay and perturbing the cache mid-scan (finding §1's thread note). A third
// candidate the finding originally listed — push-data history shortfall silently
// skipping leading replay frames — is DEAD CODE on this engine: the replay entry
// check guarantees `RecordedPushData.Num() == NumResimSteps`, so the skip condition
// reduces to `Step >= ResimStep`, always true (item 42 review §2.3). Ruling it out
// invalidates NO instrument — this line still discriminates the two live candidates.
struct StrandedResimEpisode
{
    // The simulation tick `prepareResimulation` restored to.
    std::uint32_t anchorTick = 0u;
    // The prediction frontier at the moment the stranded frame was observed.
    std::uint32_t predictionTick = 0u;
    // Replay ticks actually executed since that prepare.
    std::uint32_t replayedTicks = 0u;
    // predictionTick - anchorTick, i.e. how many the clock needed to catch up.
    // Saturates at 0 rather than wrapping if the frontier is somehow behind.
    std::uint32_t catchUpDeficit = 0u;
};

// ---------------------------------------------------------------------------
// ResimGateProbe — PHYSICS THREAD ONLY.
//
// ORDERING NOTE, and it is the only surprising thing about this class. The window
// closes inside `noteCheck`, which runs at the TOP of the frame's gate evaluation;
// that frame's own request / grant / prepare / finish are recorded AFTER the flush
// and therefore land in the NEXT window. The skew is at most one frame per 120 and
// is a SHIFT, not a drop — no ratio is biased by it, and every event is counted
// exactly once. The alternative (a separate end-of-frame flush hook) would have
// meant a second physics-thread entry point on the adapter for no measurable gain.
// ---------------------------------------------------------------------------
class ResimGateProbe
{
public:
    explicit ResimGateProbe(std::uint32_t windowSamples = kResimGateProbeWindowSamples)
        : m_windowSamples(windowSamples == 0u ? kResimGateProbeWindowSamples : windowSamples)
    {
    }

    // --- I1 -----------------------------------------------------------------
    // Record one divergence check and its outcome. Returns true — filling
    // `outSummary` and resetting the window — when this check completed a window.
    bool noteCheck(bool requestedResim, ResimGateWindowSummary& outSummary)
    {
        ++m_checks;
        if (requestedResim)
            ++m_requested;
        else
            ++m_declined;

        if (m_checks < m_windowSamples)
            return false;

        fillSummary(outSummary);
        resetWindow();
        return true;
    }

    // --- [item 45] the depth-policy exclusion count for THIS frame ----------
    // Takes a count rather than being a per-event call, for the same reason
    // `noteReplayOverruns` does: `checkDivergenceAll` sweeps every character and can
    // exclude more than one per frame. Called unconditionally (with 0 on a normal
    // frame) from the same site that reports the check outcome, so the two can never
    // describe different frames.
    //
    // ⚠ CALL IT BEFORE `noteCheck`, NOT AFTER. Unlike the request / grant / prepare
    // family — which the ORDERING NOTE on this class deliberately lets fall into the
    // NEXT window — this count is derived from the SAME `checkDivergenceAll` call
    // that produces `declined` / `requested`, and `noteCheck` records those into the
    // window it then flushes. Recording this one after the flush would charge it to
    // the next window, putting the numerator and the denominator of one frame into
    // two different lines.
    void noteDeepAnchorSkips(std::uint32_t count) { m_deepAnchorExclusions += count; }

    // --- I3 + I4 ------------------------------------------------------------
    // One rewind request that crossed into the engine.
    //
    // `lastCompletedStep` and `requestedChaosFrame` are CHAOS-domain frames (signed,
    // because the engine's are); `anchorTick` is a SIMULATION tick. Mixing those two
    // domains is exactly the mistake finding §3 warns about, so they are separate
    // parameters with names that say which is which rather than one "tick" pair.
    void noteRequest(std::uint32_t anchorTick,
                     std::int32_t  lastCompletedStep,
                     std::int32_t  requestedChaosFrame)
    {
        ++m_requests;

        if (m_hasPreviousRequest && m_previousRequestAnchorTick == anchorTick)
            ++m_repeatRequests;
        m_hasPreviousRequest       = true;
        m_previousRequestAnchorTick = anchorTick;

        m_lastRequestedChaosFrame    = requestedChaosFrame;
        m_hasLastRequestedChaosFrame = true;

        const std::int32_t rawDepth = lastCompletedStep - requestedChaosFrame;
        const std::uint32_t depth =
            rawDepth <= 0 ? 0u : static_cast<std::uint32_t>(rawDepth);
        if (m_requestDepthSamples == 0u)
        {
            m_minRequestDepth = depth;
            m_maxRequestDepth = depth;
        }
        else
        {
            if (depth < m_minRequestDepth) m_minRequestDepth = depth;
            if (depth > m_maxRequestDepth) m_maxRequestDepth = depth;
        }
        ++m_requestDepthSamples;
    }

    // One granted rewind, at the chaos frame Chaos actually started it from.
    //
    // A grant with no request on record is NOT a clamp — it is an engine-side
    // requester (physics replication, or `CompareTargetsToLastFrame`) rewinding
    // without us, which finding §3 lists as a real possibility. It is counted as a
    // grant and left out of `clampedGrants` rather than being charged as a mismatch
    // against a request that never happened.
    void noteGrant(std::int32_t grantedChaosFrame)
    {
        ++m_grants;
        if (m_hasLastRequestedChaosFrame && grantedChaosFrame != m_lastRequestedChaosFrame)
            ++m_clampedGrants;
        m_hasLastRequestedChaosFrame = false;
    }

    // --- I5 -----------------------------------------------------------------
    // `prepareResimulation` ran: the clock started a resim at `anchorTick` and the
    // cache restored into live state. Opens an EPISODE, which stays open until a
    // finish closes it or a stranded prediction frame reports it.
    void notePrepare(std::uint32_t anchorTick)
    {
        ++m_prepares;
        m_episodeOpen           = true;
        m_episodeReported       = false;
        m_episodeAnchorTick     = anchorTick;
        m_episodeReplayedTicks  = 0u;
    }

    // The `[Resim.Finish]` apply edge. Closes the episode.
    void noteFinish()
    {
        ++m_finishes;
        m_episodeOpen     = false;
        m_episodeReported = false;
    }

    // A normal prediction frame entered while the clock still believes it is
    // resimulating. Always counted. Returns true — filling `outEpisode` — only on
    // the FIRST such frame of an episode, so the caller's Verbose line is one-shot
    // per stranded resim rather than one per frame for as long as the cursor stays
    // stuck (a per-frame line here is precisely the T19 volume defect, and the
    // stuck state can persist for many frames by construction).
    bool noteStuckResimFrame(std::uint32_t predictionTick, StrandedResimEpisode& outEpisode)
    {
        ++m_stuckResimFrames;

        if (!m_episodeOpen || m_episodeReported)
            return false;

        m_episodeReported = true;
        outEpisode.anchorTick     = m_episodeAnchorTick;
        outEpisode.predictionTick = predictionTick;
        outEpisode.replayedTicks  = m_episodeReplayedTicks;
        outEpisode.catchUpDeficit =
            predictionTick > m_episodeAnchorTick ? predictionTick - m_episodeAnchorTick : 0u;
        return true;
    }

    // --- I6 -----------------------------------------------------------------
    // One resim replay tick executed.
    void noteReplayTick()
    {
        ++m_replayTicks;
        if (m_episodeOpen)
            ++m_episodeReplayedTicks;
    }

    // `count` slots that a replay tick could not be written into. Takes a count
    // rather than being a per-event call because `postResimulationAll` sweeps every
    // character and can discard more than one per replay tick.
    void noteReplayOverruns(std::uint32_t count) { m_replayOverruns += count; }

    // [item 47] `fresh` + `stale` slots that the replay was refused permission to
    // overwrite on THIS replay tick. Takes counts, from the same sweep and the
    // same call site as `noteReplayOverruns`, for the same reason: one replay
    // tick sweeps every character. Passing both in one call rather than two makes
    // it impossible for a caller to report one population and forget the other —
    // which matters here because the pair is only meaningful together (fresh is
    // the defect rate, stale is the hygiene rate, and the split IS the
    // instrument).
    void noteCorrectionProtections(std::uint32_t fresh, std::uint32_t stale)
    {
        m_freshClobbersAvoided += fresh;
        m_staleClobbersAvoided += stale;
    }

    // --- introspection; tests and diagnostics only -------------------------
    std::uint32_t checkCount()   const { return m_checks; }
    std::uint32_t windowSamples() const { return m_windowSamples; }

    // Snapshot of the window IN PROGRESS, without closing or resetting it. The
    // shipped code never calls this — it exists so a test (and a future on-demand
    // diagnostic) can read a partial window, which is the only way to observe a
    // rate in a session whose windows never complete. Same contract as
    // CorrectionVerdictProbe::fillSummary.
    void fillSummary(ResimGateWindowSummary& out) const
    {
        out.checks              = m_checks;
        out.declined            = m_declined;
        out.requested           = m_requested;
        out.requestRatePerMille = perMille(m_requested, m_checks);
        out.deepAnchorExclusions = m_deepAnchorExclusions;

        out.requests            = m_requests;
        out.grants              = m_grants;
        out.refusedFrames       = m_requests > m_grants ? m_requests - m_grants : 0u;
        out.refusedRatePerMille = perMille(out.refusedFrames, m_requests);
        out.repeatRequests      = m_repeatRequests;

        out.clampedGrants       = m_clampedGrants;
        out.minRequestDepth     = m_requestDepthSamples == 0u ? 0u : m_minRequestDepth;
        out.maxRequestDepth     = m_requestDepthSamples == 0u ? 0u : m_maxRequestDepth;

        out.prepares            = m_prepares;
        out.finishes            = m_finishes;
        out.abandoned           = m_prepares > m_finishes ? m_prepares - m_finishes : 0u;
        out.stuckResimFrames    = m_stuckResimFrames;

        out.replayTicks         = m_replayTicks;
        out.replayOverruns      = m_replayOverruns;

        out.freshClobbersAvoided = m_freshClobbersAvoided;
        out.staleClobbersAvoided = m_staleClobbersAvoided;
    }

private:
    static std::uint32_t perMille(std::uint32_t numerator, std::uint32_t denominator)
    {
        if (denominator == 0u)
            return 0u;
        return static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(numerator) * 1000u + denominator / 2u) / denominator);
    }

    void resetWindow()
    {
        m_checks = m_declined = m_requested = 0u;
        m_deepAnchorExclusions = 0u;
        m_requests = m_grants = m_repeatRequests = 0u;
        m_clampedGrants = 0u;
        m_minRequestDepth = m_maxRequestDepth = 0u;
        m_requestDepthSamples = 0u;
        m_prepares = m_finishes = m_stuckResimFrames = 0u;
        m_replayTicks = m_replayOverruns = 0u;
        // [item 47] The protection counters ARE per-window, like every other
        // event count here: each one is a completed observation of a single
        // replay tick, with no sequence straddling the boundary to preserve.
        m_freshClobbersAvoided = m_staleClobbersAvoided = 0u;
        // DELIBERATELY NOT RESET: m_previousRequestAnchorTick / m_hasPreviousRequest,
        // m_lastRequestedChaosFrame / m_hasLastRequestedChaosFrame, and the whole
        // episode block. All four describe an event sequence that STRADDLES the
        // window boundary — a refusal run, a request awaiting its grant, a resim
        // awaiting its apply edge. Clearing them at the boundary would drop exactly
        // the events most likely to be the interesting ones and would silently
        // under-report `repeatRequests` and `clampedGrants` once per window.
    }

    // I1
    std::uint32_t m_checks    = 0u;
    std::uint32_t m_declined  = 0u;
    std::uint32_t m_requested = 0u;

    // [item 45] Depth-policy exclusions this window; see deepAnchorExclusions.
    std::uint32_t m_deepAnchorExclusions = 0u;

    // I3
    std::uint32_t m_requests       = 0u;
    std::uint32_t m_grants         = 0u;
    std::uint32_t m_repeatRequests = 0u;
    bool          m_hasPreviousRequest        = false;
    std::uint32_t m_previousRequestAnchorTick = 0u;

    // I4
    std::uint32_t m_clampedGrants       = 0u;
    std::uint32_t m_minRequestDepth     = 0u;
    std::uint32_t m_maxRequestDepth     = 0u;
    std::uint32_t m_requestDepthSamples = 0u;
    bool          m_hasLastRequestedChaosFrame = false;
    std::int32_t  m_lastRequestedChaosFrame    = 0;

    // I5
    std::uint32_t m_prepares         = 0u;
    std::uint32_t m_finishes         = 0u;
    std::uint32_t m_stuckResimFrames = 0u;
    bool          m_episodeOpen          = false;
    bool          m_episodeReported      = false;
    std::uint32_t m_episodeAnchorTick    = 0u;
    std::uint32_t m_episodeReplayedTicks = 0u;

    // I6
    std::uint32_t m_replayTicks    = 0u;
    std::uint32_t m_replayOverruns = 0u;

    // [item 47] The hollow-anchor ledger; see the summary fields for what each
    // population means and why `stale` is a wiring check rather than a tuning
    // signal.
    std::uint32_t m_freshClobbersAvoided = 0u;
    std::uint32_t m_staleClobbersAvoided = 0u;

    std::uint32_t m_windowSamples;
};

// ---------------------------------------------------------------------------
// I2 — WHERE A CORRECTION LANDED RELATIVE TO THE PREDICTION FRONTIER.
//
// THE FIRST-GATE DISCRIMINATOR, and the reason it is three buckets and not two:
//
//   Behind      tick < predictionTick. The correction set `m_containsCorrectTick`
//               on its own slot, adopted authority state INTO THAT SLOT — and, UNDER
//               THE HISTORICAL GATE, triggered NOTHING, because the frontier's
//               inherited resim bit shadowed it. Live simulatable state is never
//               touched outside a resim, and resims restore at the NEWEST corrected
//               slot, so an older corrected slot was never replayed through either.
//               **Those corrections were dead weight**, and their count is the
//               physically meaningful "suppressed trigger" number.
//               ⚠ [item 45] STILL TRUE ON A DEFAULT BUILD, BY CONFIGURATION RATHER
//               THAN BY MECHANISM: the shipped `FrontierExact` policy sets no anchor
//               for a behind-frontier landing. Under `OnDisagreement` (item 46) a
//               DISAGREEING landing here sets the anchor and this bucket becomes the
//               trigger population rather than the suppressed one. The bucket's
//               DEFINITION is untouched either way — it is a position, not a verdict.
//   AtFrontier  tick == predictionTick. Under the historical gate, THE ONLY EVENT
//               THAT RE-OPENED THE GATE in play: `tryInsertingCorrectState` cleared
//               `m_isResimulated` on the slot it landed in, the next
//               `pushPredictionTick` inherited `false`, and the next
//               `checkDivergenceAll` found the corrected slot one tick behind the new
//               frontier.
//               [item 45] It is now the `FrontierExact` policy's anchor-set condition
//               — LITERALLY the same predicate, because both this classification and
//               `resimGate::shouldSetPendingAnchor` are given the same
//               `tick == getPredictionTick()` comparison. That is deliberate: it is
//               what makes this probe's archived `atFrontier` counts the baseline the
//               legacy policy has to reproduce.
//   Discarded   the tick had no slot at all. Item 41's `aboveNewest` population
//               lands here, and so does anything older than the 60-slot window. No
//               comparison happened, no flag moved, and no anchor is set — which is
//               also why an anchor can never be AHEAD of the frontier.
//
// ⭐ WHAT THE PAIR PROVES, so nobody has to re-derive it from the finding:
// `landedBehind` large while triggers track only `landedAtFrontier` IS the
// demonstrated under-resimulation statement — relative to the design intent
// "resimulate when an authoritative correction disagrees with what we predicted",
// the system resimulates on a clock-alignment artifact instead. Whether that
// MATTERS is a different question (are the suppressed corrections micrometres or
// metres?) and it is deliberately NOT answerable from here: `isSimilarTo` is a
// boolean fold that discards the distance it folded over. That magnitude is
// backlog item 28 and is out of scope for item 42.
//
// PER CLASS, NEVER POOLED, and the class test is PROVIDER-PRESENCE — the same
// lookup `registerPredictionOwner` forks on and `collectInputAll` forks on every
// tick, reached through the same `PredictedCharacterClass` enum
// CorrectionVerdictProbe uses. A second, independently-derived notion of "remote"
// is the thing most likely to disagree with the first one after a future edit, and
// then the two halves of this summary would describe different populations while
// looking authoritative.
//
// NO PER-ID STATE, hence no `forgetOwner`: the summary is a per-CLASS aggregate
// across characters, so unregistering a character leaves nothing behind to erase.
// Do not copy the relay probes' per-id watermark teardown here looking for
// symmetry — see the same note on CorrectionVerdictProbe.
// ---------------------------------------------------------------------------
enum class CorrectionLandingSite : std::uint8_t
{
    Behind = 0,
    AtFrontier,
    Discarded,
};

// The classification, as a free function so the ONE definition of "at the frontier"
// is shared by the shipped call site and by every test, and so it can be swept as a
// unit with no cache, no owner and no simulatable.
//
// `landed == false` wins outright: a discarded correction has no slot, so comparing
// its tick against the frontier would classify an event that never happened.
inline CorrectionLandingSite classifyCorrectionLanding(bool          landed,
                                                       std::uint32_t correctionTick,
                                                       std::uint32_t predictionTick)
{
    if (!landed)
        return CorrectionLandingSite::Discarded;
    return correctionTick == predictionTick
        ? CorrectionLandingSite::AtFrontier
        : CorrectionLandingSite::Behind;
}

struct CorrectionLandingClassSummary
{
    // Landed strictly behind the frontier — triggered nothing. See the Behind
    // bucket note above: this is the suppressed-trigger count.
    std::uint32_t landedBehind = 0u;
    // Landed exactly ON the frontier — the only in-play gate opener.
    std::uint32_t landedAtFrontier = 0u;
    // No slot; no comparison; no flag moved.
    std::uint32_t discarded = 0u;

    // landedAtFrontier / (landedBehind + landedAtFrontier + discarded), in parts
    // per thousand. THE FRONTIER-TOUCH RATE, which under the mechanism at the top
    // of this file is what the resim trigger rate actually tracks — so this number
    // and `[ResimProbe.Gate]`'s `requestRatePerMille` are the two halves of the
    // finding's central claim and should move together.
    //
    // Zero when the class saw nothing in the window — no observation, not a perfect
    // record. The caller SKIPS a class block with no events rather than printing
    // zeros, which would read as the opposite of "no data".
    std::uint32_t atFrontierRatePerMille = 0u;

    std::uint32_t total() const { return landedBehind + landedAtFrontier + discarded; }
};

struct CorrectionLandingWindowSummary
{
    // Events across BOTH classes — the window's own size. Always equals
    // local.total() + remote.total().
    std::uint32_t samples = 0u;

    CorrectionLandingClassSummary local;
    CorrectionLandingClassSummary remote;
};

// ---------------------------------------------------------------------------
// CorrectionLandingProbe — GAME THREAD ONLY.
// ---------------------------------------------------------------------------
class CorrectionLandingProbe
{
public:
    explicit CorrectionLandingProbe(
        std::uint32_t windowSamples = kResimGateProbeWindowSamples)
        : m_windowSamples(windowSamples == 0u ? kResimGateProbeWindowSamples : windowSamples)
    {
    }

    // Record one correction's landing site. Returns true — filling `outSummary` and
    // resetting the window — when this sample completed a window.
    //
    // ⚠ DISCARDS ARE SAMPLES HERE. That is the one place this probe deliberately
    // differs from its CorrectionVerdictProbe neighbour, which excludes them
    // because they produce no verdict. Here the discard IS an observation of the
    // correction stream's alignment against the frontier — item 41's `aboveNewest`
    // population is nothing but discards — so excluding them would hide the very
    // class this instrument was added to see.
    bool noteLanding(PredictedCharacterClass characterClass,
                     CorrectionLandingSite   site,
                     CorrectionLandingWindowSummary& outSummary)
    {
        ClassCounters& counters = countersFor(characterClass);
        switch (site)
        {
        case CorrectionLandingSite::Behind:     ++counters.landedBehind;     break;
        case CorrectionLandingSite::AtFrontier: ++counters.landedAtFrontier; break;
        case CorrectionLandingSite::Discarded:  ++counters.discarded;        break;
        }

        ++m_samples;
        if (m_samples < m_windowSamples)
            return false;

        fillSummary(outSummary);
        resetWindow();
        return true;
    }

    // --- introspection; tests and diagnostics only -------------------------
    std::uint32_t sampleCount()   const { return m_samples; }
    std::uint32_t windowSamples() const { return m_windowSamples; }

    std::uint32_t countFor(PredictedCharacterClass characterClass,
                           CorrectionLandingSite   site) const
    {
        const ClassCounters& counters = countersFor(characterClass);
        switch (site)
        {
        case CorrectionLandingSite::Behind:     return counters.landedBehind;
        case CorrectionLandingSite::AtFrontier: return counters.landedAtFrontier;
        case CorrectionLandingSite::Discarded:  return counters.discarded;
        }
        return 0u;
    }

    // Snapshot of the window IN PROGRESS; does not close or reset it.
    void fillSummary(CorrectionLandingWindowSummary& out) const
    {
        out.samples = m_samples;
        fillClassSummary(m_local,  out.local);
        fillClassSummary(m_remote, out.remote);
    }

private:
    struct ClassCounters
    {
        std::uint32_t landedBehind     = 0u;
        std::uint32_t landedAtFrontier = 0u;
        std::uint32_t discarded        = 0u;
    };

    static void fillClassSummary(const ClassCounters& counters,
                                 CorrectionLandingClassSummary& out)
    {
        out.landedBehind     = counters.landedBehind;
        out.landedAtFrontier = counters.landedAtFrontier;
        out.discarded        = counters.discarded;

        const std::uint32_t total = out.total();
        out.atFrontierRatePerMille =
            (total == 0u)
                ? 0u
                : static_cast<std::uint32_t>(
                      (static_cast<std::uint64_t>(out.landedAtFrontier) * 1000u + total / 2u)
                      / total);
    }

    ClassCounters& countersFor(PredictedCharacterClass characterClass)
    {
        return (characterClass == PredictedCharacterClass::RemoteProxy) ? m_remote : m_local;
    }

    const ClassCounters& countersFor(PredictedCharacterClass characterClass) const
    {
        return (characterClass == PredictedCharacterClass::RemoteProxy) ? m_remote : m_local;
    }

    void resetWindow()
    {
        m_local   = ClassCounters{};
        m_remote  = ClassCounters{};
        m_samples = 0u;
    }

    ClassCounters m_local;
    ClassCounters m_remote;

    std::uint32_t m_samples = 0u;
    std::uint32_t m_windowSamples;
};

// Human-readable landing-site name for a log line. Defined once, here, so the
// spellings an operator greps for cannot drift between the shipped emitter and a
// test — the same reason `predictedCharacterClassName` lives beside its enum.
inline const char* correctionLandingSiteName(CorrectionLandingSite site)
{
    switch (site)
    {
    case CorrectionLandingSite::Behind:     return "Behind";
    case CorrectionLandingSite::AtFrontier: return "AtFrontier";
    case CorrectionLandingSite::Discarded:  return "Discarded";
    }
    return "Unknown";
}
