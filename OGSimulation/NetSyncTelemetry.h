#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include <cstdint>
#include <functional>
#include <unordered_map>

#include "OGSimulation/Network/CorrectionVerdictProbe.h"
#include "OGSimulation/Network/RelayReadProbe.h"
#include "OGSimulation/Network/RemoteInputCache.h"
#include "OGSimulation/ResimGateProbe.h"
#include "OGSimulation/SimulationLog.h"
#include "OGSimulation/SimulationReconciliation.h"
#include "OGSimulation/SimulationTimeContext.h"

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// ---------------------------------------------------------------------------
// [og-netcode-v2-input-relay task 79 / architecture review B3] NetSyncTelemetry
// — SimulationNetSync's sibling instrument, finishing the decide/project pattern
// RN-8/RN-10/RN-11 already established for this class (task 58/60/61): the peer
// computes and returns decision/report structs on its production path
// (`ScheduledRelayedReadDecision`, `CorrectionArrivalDecision` below,
// `RelayedInputIngestReport`), and THIS object owns everything that turns those
// structs into a probe count or a shipped log line.
//
// WHAT MOVED HERE (verbatim, `docs/DiagnosticsConventions.md` still the source of
// truth for the roster): the four probe MEMBERS (`RelayReadProbe`,
// `RelayArrivalProbe`, `CorrectionVerdictProbe`, `CorrectionLandingProbe`), all
// sixteen `emit*` helper BODIES, and the per-window flush bookkeeping they share.
//
// ⛔ THIS IS AN OWNER, NOT A DIAGNOSTICS VIEW — DiagnosticsConventions.md §2 STILL
// APPLIES, UNCHANGED IN OUTCOME. SimulationNetSync's production methods call
// straight into this class on every tick / every arrival / every correction —
// `collectInputAll`, `collectResimInputAll`, the OnRep-bound relay-arrival and
// correction callbacks, and `unregisterSimulatable`'s lifecycle cleanup all reach
// a public method here, unconditionally, on their own thread. Nothing on this
// class is reachable from `SimulationNetSync::getDiagnostics()` — that view still
// exposes only the four probes, CONST, as it did before this task; see
// `Diagnostics::relayReadProbe()` etc. on `SimulationNetSync`, which now simply
// delegate into this object's own const accessors below. "instrument = the
// sibling object" is now structure, not prose — this class IS the fence.
//
// THE ONE THING DELIBERATELY NOT HANDED IN: a reference into a production
// container. `emitRelayArrival` used to take the arriving `RemoteInputCache<InputT>&`
// store purely to call its one-shot `shouldLogVersionMismatchOnce()` latch —
// see that method below for why the latch moved IN here (id-keyed) instead of a
// pointer moving in. Handing this class a live reference into
// `SimulationNetSync`'s per-id maps would undercut the split this task exists to
// make: a telemetry object that can read (or, worse, outlive) a production
// container is no longer just an instrument.
//
// -----------------------------------------------------------------------------
// THE TWO-THREAD RULE, AS A CLASS PROPERTY (task 79's other stated deliverable).
// Previously this was prose scattered across a 2,900-line class — a comment at
// each call site plus one at the probe members' declaration. It is now stated
// ONCE, per method, here:
//
//   PHYSICS THREAD ONLY — reached only from SimulationNetSync::collectInputAll /
//   collectResimInputAll and their per-character helpers:
//     emitLocalInputRead, emitRemoteQueueRead, emitPredictionInputRead,
//     emitResimNoSlot, emitResimSentinel, emitResimLocalRead, emitResimNoStore,
//     emitResimRefRead, emitResimScheduledRead, emitRelayReadWindowIfDue
//     (which is also the sole caller of emitMissClassLine / emitDeltaLine).
//
//   GAME THREAD ONLY, relay-ring arrival (OnRep-bound, via
//   SimulationNetSync::onRelayedInputReceived):
//     emitRelayArrival, and with it `shouldLogVersionMismatchOnce` (public, but
//     production only ever reaches it from here — see that method's own note
//     on why it is exposed at all).
//
//   GAME THREAD ONLY, correction-state arrival (OnRep-bound, via
//   SimulationNetSync::onCorrectionReceived):
//     emitCorrectionArrival (which is also the sole caller of
//     emitCorrectionVerdictClassLine / emitCorrectionLandingClassLine).
//
//   EITHER THREAD, LIFECYCLE ONLY (never concurrent with the above by
//   construction — see `unregisterSimulatable`'s own ordering comment):
//     forgetOwner.
//
//   CONST, either thread: the four probe accessors — read-only, and the reason
//   `SimulationNetSync::Diagnostics` may call them from wherever a test likes.
//
// No method here is ever called from both threads, so nothing on this class is
// atomic — the same property the probe split existed to buy, now stated as a
// method-level fence instead of an object-level one. This complements, and does
// not restate, `Network/RelayReadProbe.h`'s own "two objects because there are
// two threads" banner: that one is about the PROBE TYPES; this one is about
// which of THIS class's METHODS may be called from which thread.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// [task 60 / RN-10 part C, relocated task 79] THE CORRECTION-ARRIVAL DECISION.
//
// Free struct rather than nested in SimulationNetSync (unlike before this task):
// `SimulationNetSync::decideCorrectionArrival` (the PURE half — computes this,
// touches no probe, calls no `note*`/`emit*`/`log*`) still lives on the peer and
// still returns this by value; `NetSyncTelemetry::emitCorrectionArrival` below
// (the PROJECTION half) is this struct's one consumer. It needs a name both
// classes can see without one owning the other, hence file scope here rather
// than nested in either — the same reason `ScheduledRelayedReadDecision` at the
// top of SimulationNetSync.h is a free template struct rather than nested.
//
// This callback's probing has an early `return` interleaved between two probe
// calls (`noteLanding` always runs; `noteCorrection` must NOT run on a discard),
// so folding "the probing" into one helper the way `emitRelayArrival` does would
// move what that `return` returns from — the verdict probe would then run on
// discarded corrections and silently corrupt item 41's `aboveNewest` population
// (the discard bucket `CorrectionLandingProbe` counts). `landed` is therefore a
// FIELD of the decision, not a mid-block `return`, exactly as
// `ScheduledRelayedReadDecision::isUnderflowMiss` is a field rather than a
// re-derivable fact (see the banner above that struct in SimulationNetSync.h).
// `characterClass` / `landingSite` are meaningful even when `!landed` — the
// landing probe needs them unconditionally, see the "hoisted above the gate"
// note in `decideCorrectionArrival` in SimulationNetSync.h.
// ---------------------------------------------------------------------------
struct CorrectionArrivalDecision
{
    PredictedCharacterClass characterClass = PredictedCharacterClass::LocallyPredicted;
    CorrectionLandingSite   landingSite    = CorrectionLandingSite::Discarded;

    // Mirrors CorrectionInsertVerdict::landed. Kept as its own field —
    // rather than re-derived from `landingSite == Discarded` — so the two
    // notions can never silently diverge in the projection below.
    bool landed = false;

    // The correction's tick, echoed back so the caller's log line needs no
    // second decode of the wire buffer. Set on BOTH paths — mirrors
    // CorrectionInsertVerdict::tick's own rule. `emitCorrectionArrival`'s
    // `[Verbose][ResimProbe.Landing]` SIMLOG relies on this: it prints
    // `decision.tick` unconditionally, BEFORE the `!landed` gate below, so a
    // discarded correction's landing-probe line still carries the wire tick.
    uint32 tick = 0u;

    // Meaningless when `!landed` (CorrectionInsertVerdict's own rule) — unlike
    // `tick` above, this one really is discard-invalid: no comparison happened,
    // so it must never be read as either an agreement or a disagreement.
    bool predictionWasCorrect = false;
};

class NetSyncTelemetry
{
public:
    void setLogger(std::function<void(const char*)> logger)
    {
        m_logger = std::move(logger);
    }

    // =======================================================================
    // [task 59, retargeted task 79] THE FOUR PROBE ACCESSORS — CONST-ONLY, same
    // contract as before this task. `SimulationNetSync::Diagnostics` delegates
    // into these rather than reading its own members directly; every test call
    // site on that view is unchanged (see the diagnostics banner on
    // SimulationNetSync for the full rationale, and
    // `docs/DiagnosticsConventions.md` §2/§3 for the classification these views
    // follow — not re-derived here).
    // =======================================================================
    const RelayReadProbe&    relayReadProbe()    const { return m_relayReadProbe; }
    const RelayArrivalProbe& relayArrivalProbe() const { return m_relayArrivalProbe; }
    const CorrectionVerdictProbe& correctionVerdictProbe() const { return m_correctionVerdictProbe; }
    const CorrectionLandingProbe& correctionLandingProbe() const { return m_correctionLandingProbe; }

    // [T19, relocated task 79] LIFECYCLE CLEANUP, NOT DIAGNOSTICS — called once
    // from `SimulationNetSync::unregisterSimulatable`, itself unchanged: step 4
    // of that fixed, load-bearing ordering. Neither relay probe holds anything
    // the simulation reads, but both keep an id-keyed map (the stale run and the
    // capture-tick watermark), and without this they would grow with every
    // character that has ever existed in the session — the unbounded-memo shape
    // this codebase has already had to fix once in the log throttles. The
    // version-mismatch latch (below) is the same shape and is forgotten here for
    // the same reason.
    //
    // `m_correctionVerdictProbe` / `m_correctionLandingProbe` are deliberately
    // NOT forgotten here, and the asymmetry is by design rather than an
    // omission: neither keeps any id-keyed map at all. Their counters are
    // per-CLASS aggregates across characters, so an unregistered character
    // leaves nothing to erase and the class totals for the window it was part
    // of stay correct. Forgetting something here would need per-id state to
    // exist first, which would be a different metric.
    void forgetOwner(unsigned int id)
    {
        m_relayReadProbe.forgetOwner(id);
        m_relayArrivalProbe.forgetOwner(id);
        m_versionMismatchLogged.erase(id);
    }

    // [item 61] Local-provider branch's classification line — a plain,
    // unconditional single-statement fold (Pattern 1); nothing gates it.
    void emitLocalInputRead(unsigned int id, uint32 tick, StepKind stepKind, int32 effectiveDelay)
    {
        SIMLOG(m_logger,
            "[CollectInput] id=%u tick=%u source=Provider kind=%s delay=%d",
            id, tick, stepKindName(stepKind), effectiveDelay);
    }

    // [item 61] Remote-queue branch's classification line — same shape as
    // `emitLocalInputRead` above.
    void emitRemoteQueueRead(unsigned int id, uint32 tick, uint32 queuedTick)
    {
        SIMLOG(m_logger, "[CollectInput] id=%u tick=%u source=RemoteQueue queuedTick=%u",
            id, tick, queuedTick);
    }

    // [item 61, task 79] Simulated-proxy branch's WHOLE probing tail: the
    // T19/T20 probe write (only when a store exists), its two rare-event
    // Verbose lines, and the unconditional per-tick [CollectInput]
    // classification line. `hasStore` replaces the pointer
    // `SimulationNetSync` used to pass — this method never dereferenced the
    // store, only null-checked it, so the caller now passes that one bit
    // rather than a reference into its own `RemoteInputCache` map (see the
    // file banner's note on not handing this class a production reference).
    void emitPredictionInputRead(unsigned int id, uint32 tick,
                                 bool hasStore,
                                 const ScheduledRelayedReadReport& readReport)
    {
        if (hasStore)
        {
            // [T20] The WHOLE report, not just the outcome: the miss class
            // and the signed probe-to-newest delta are tallied here too.
            m_relayReadProbe.notePredictionRead(id, readReport);

            // PER-EVENT DETAIL AT VERBOSE, AND ONLY FOR THE OUTCOME THAT IS
            // SILENT IN THE STEADY STATE — the [RelaySkip] precedent exactly.
            // A verify-fail means the delay regime moved under this reader,
            // which does not happen while the schedule is stable, so this
            // costs nothing per tick in the ordinary case. Hits and misses
            // are RATES and are reported by the per-window summary; emitting
            // them per event would be another per-tick line, which is the
            // thing this task exists to stop adding.
            if (readReport.outcome == ScheduledRelayedReadOutcome::VerifyFail)
            {
                SIMLOG(m_logger,
                    "[Verbose][RelayProbe.Read] id=%u tick=%u VERIFY-FAIL probeTick=%u "
                    "candidateDA=%u dLatest=%u src=Prediction",
                    id, tick, readReport.probeTick,
                    static_cast<unsigned int>(readReport.candidateDA),
                    static_cast<unsigned int>(readReport.dLatest));
            }

            // [T20] THE OTHER OUTCOME THAT IS SILENT IN THE STEADY STATE.
            // missInSpan and missAboveNewest are the two expected classes and
            // are RATES — the per-window summary reports them. A read landing
            // BELOW the oldest resident entry is different in kind: it means
            // the receiver's clock has drifted out of the store's 64-tick
            // reach, which should not happen at all, so it gets the same
            // rare-event treatment the verify-fail line gets.
            if (readReport.missClass == ScheduledRelayedReadMissClass::BelowOldest)
            {
                SIMLOG(m_logger,
                    "[Verbose][RelayProbe.Read] id=%u tick=%u BELOW-OLDEST probeTick=%u "
                    "oldest=%u newest=%u resident=%u src=Prediction",
                    id, tick, readReport.probeTick,
                    readReport.oldestResident, readReport.newestResident,
                    readReport.residentCount);
            }
        }

        SIMLOG(m_logger, "[CollectInput] id=%u tick=%u source=RemoteInputCache hasStore=%d",
            id, tick, hasStore ? 1 : 0);
    }

    // [item 61] NoSlot rung — single unconditional line, straight fold.
    void emitResimNoSlot(unsigned int id, uint32 tick)
    {
        SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=NoSlot", id, tick);
    }

    // [item 61] Sentinel rung — same shape as `emitResimNoSlot` above.
    void emitResimSentinel(unsigned int id, uint32 tick)
    {
        SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=Sentinel", id, tick);
    }

    // [item 61] Local/delay-line rung.
    void emitResimLocalRead(unsigned int id, uint32 tick, AppliedCaptureRefKind refKind, int32 captureTick)
    {
        SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=%s src=LocalInputCache capture=%d",
            id, tick, refKind == AppliedCaptureRefKind::Ref ? "Ref" : "NoRef", captureTick);
    }

    // [item 61] Remote/no-store rung.
    void emitResimNoStore(unsigned int id, uint32 tick)
    {
        SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=Remote src=NoStore", id, tick);
    }

    // [item 61] Remote/Ref rung.
    void emitResimRefRead(unsigned int id, uint32 tick, uint32 captureTick, bool hit)
    {
        SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=Ref src=RemoteInputCache ref=%u hit=%d",
            id, tick, captureTick, hit ? 1 : 0);
    }

    // [item 61] Remote/NoRef rung — the ONE rung with a probe write
    // (`noteResimRead`) plus two SIMLOGs (one gated on VerifyFail, one
    // unconditional). Folds cleanly: nothing after this call in the caller's
    // `collectResimInputForCharacter` branches on whether the probe fired —
    // the `map.emplace`/`return` there run unconditionally either way, same as
    // they did with the diagnostics inline.
    void emitResimScheduledRead(unsigned int id, uint32 tick, const ScheduledRelayedReadReport& readReport)
    {
        // [T20] The whole report — same reason as the prediction site.
        m_relayReadProbe.noteResimRead(readReport);

        if (readReport.outcome == ScheduledRelayedReadOutcome::VerifyFail)
        {
            SIMLOG(m_logger,
                "[Verbose][RelayProbe.Read] id=%u tick=%u VERIFY-FAIL probeTick=%u "
                "candidateDA=%u dLatest=%u src=Resim",
                id, tick, readReport.probeTick,
                static_cast<unsigned int>(readReport.candidateDA),
                static_cast<unsigned int>(readReport.dLatest));
        }

        SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=NoRef src=ScheduledRead", id, tick);
    }

    // [RN-10 part B, task 79] ALL of the relay-ring arrival callback's probing +
    // logging, reusing the existing `emit*` verb rather than coining a new one.
    // See `docs/DiagnosticsConventions.md` §3 for the current `emit*` roster;
    // this and `emitCorrectionArrival` extend that convention rather than
    // inventing a second one.
    //
    // ⚠ [task 79 / THE ONE REAL DESIGN DECISION] NO `RemoteInputCache&` PARAMETER
    // HERE, UNLIKE THE PRE-TASK-79 SHAPE. The only thing this method ever needed
    // from the store was its one-shot `shouldLogVersionMismatchOnce()` latch —
    // "have I already warned about THIS character's incompatible peer, this
    // session" — and handing this class a live reference into
    // `SimulationNetSync`'s per-id map would be exactly the production-container
    // coupling this split exists to avoid. The latch itself MOVED here instead,
    // id-keyed (`m_versionMismatchLogged`), because log-suppression state
    // belongs with the logger, not with the data container being logged about.
    // `RemoteInputCache::shouldLogVersionMismatchOnce()` and its backing bool are
    // retired with zero remaining callers (see `Network/RemoteInputCache.h`);
    // `RemoteInputCacheTest.cpp`'s four assertions against it move to
    // `NetSyncTelemetryTest.cpp` against this method instead — see this task's
    // impl notes for the full before/after.
    void emitRelayArrival(unsigned int id, const RelayedInputIngestReport& report)
    {
        // [T19] PROBE 2 — replication cadence, measured in CAPTURE
        // TICKS. GAME-THREAD window, a different object from the
        // physics-side read probe; see the two-thread rule at the top of
        // this file (and Network/RelayReadProbe.h's own probe-level
        // statement, which this complements).
        //
        // `report.newestCaptureTick` is the newest tick THIS RING
        // carried, not the newest the store holds — the distinction is
        // load-bearing and the report field's own comment explains it.
        if (report.newestCaptureTickValid)
        {
            RelayArrivalWindowSummary arrival;
            std::uint32_t gapCaptureTicks = 0u;
            // ⛔ [T34 loss-counter fix] `newCaptureTicksIngested`, NOT
            // `entriesIngested` and NOT a hard-coded 1. It is the count
            // of capture ticks this arrival made newly resident, and it
            // is what turns `lostCaptureTicksX1000` from a measure of
            // the BURST RATE into a measure of loss. `entriesIngested`
            // would count re-delivered ticks as new coverage and hide
            // real loss; a hard-coded 1 is the retired replace-latest
            // premise and is exactly what reported ~120 per mille on a
            // working flush.
            const bool windowClosed = m_relayArrivalProbe.noteArrival(
                id,
                report.newestCaptureTick,
                static_cast<std::uint32_t>(report.newCaptureTicksIngested),
                arrival,
                &gapCaptureTicks);

            // Per-event Verbose, and only when the cadence actually
            // hiccuped. A gap of exactly 1 is the healthy depth-1
            // steady state and would be a per-tick line; the interesting
            // events are the stalls, which are what set the depth rule.
            if (gapCaptureTicks > 1u)
            {
                SIMLOG(m_logger,
                    "[Verbose][RelayProbe.Arrival] id=%u newestCapture=%u "
                    "gapCaptureTicks=%u",
                    id, report.newestCaptureTick, gapCaptureTicks);
            }

            if (windowClosed)
            {
                // PER-WINDOW SUMMARY AT WARNING — the cadence
                // [InputStats] already uses. p99 is what the
                // `depth >= gap_p99 + margin` rule reads; the mean is
                // deliberately absent because it hides the tail that
                // sets depth.
                //
                // ⭐ [T34] `lostCaptureTicksX1000` IS THE R = 0 LOSS
                // INSTRUMENT, and it is on this line rather than its
                // own because it is derived from these same samples.
                // WARNING, not Log, is load-bearing:
                // `Config/DefaultEngine.ini` sets `LogOGNet=Warning`, so
                // a Log line does not exist on a dedicated server —
                // items 35 and 36 each cost this initiative a proof line
                // for exactly that. Steady-state expectation ~ 11 per
                // mille (the measured 1.122 % wire loss); the raw
                // numerator and denominator ride along so a window can
                // be re-derived rather than trusted.
                //
                // ⭐ [T34 rework] `discont=` IS PART OF THE GATE, not
                // garnish. A window reporting `discont=` > 0 was
                // interrupted (see kRelayArrivalDiscontinuityTicks) and
                // must be DISCARDED rather than averaged in — the same
                // rule `[RelayProbe.Write] discont=` already carries.
                // `discontMax=` is the largest excluded gap, exact, so
                // discarding a window never hides how bad it was.
                //
                // ⭐ [T34 loss-counter fix] `delivered=` IS THE FIELD
                // THAT MAKES THIS LINE SELF-CHECKING. `lost + delivered
                // == expected` must hold on every window; a reader who
                // sees it fail knows the delivered count is not being
                // plumbed and that `lostCaptureTicksX1000` is measuring
                // the burst rate again. Before this field existed, that
                // failure mode was indistinguishable from a lossy wire.
                SIMLOG(m_logger,
                    "[Warning][RelayProbe.Arrival] samples=%u gapCaptureTicks "
                    "p50=%u p99=%u%s max=%u noAdvance=%u saturated=%u "
                    "lostCaptureTicksX1000=%u lost=%u delivered=%u expected=%u "
                    "discont=%u discontMax=%u",
                    arrival.samples, arrival.p50, arrival.p99,
                    arrival.p99Saturated ? "+" : "",
                    arrival.maxGap, arrival.noAdvance,
                    arrival.saturatedSamples,
                    arrival.lostCaptureTicksX1000,
                    arrival.lostCaptureTicks,
                    arrival.deliveredCaptureTicks,
                    arrival.expectedCaptureTicks,
                    arrival.discontinuities,
                    arrival.maxDiscontinuityGap);
            }
        }

        if (report.outcome == RelayedInputIngestOutcome::VersionMismatch
            && shouldLogVersionMismatchOnce(id))
        {
            // ONCE per component per session — an incompatible peer
            // re-replicates its ring forever.
            SIMLOG(m_logger,
                "[Warning][RelayedInput] DROP wire-version mismatch id=%u onWire=%u expected=%u",
                id,
                static_cast<unsigned int>(report.versionOnWire),
                static_cast<unsigned int>(relayedInputRing::kWireFormatVersion));
        }
    }

    // [task 60 / RN-10 part C, relocated task 79] THE PROJECTION — the landing
    // probe fires UNCONDITIONALLY (a discard IS an observation, item 41's
    // `aboveNewest` population); the verdict probe only if `decision.landed`.
    // THIS `return` IS THE SAME CONTROL-FLOW FACT the pre-split inline lambda
    // encoded with its mid-block `if (!verdict.landed) return;` — moved here,
    // not removed, and reading `decision.landed` rather than re-deriving it
    // from `landingSite`, per that field's own comment above.
    void emitCorrectionArrival(unsigned int id, const CorrectionArrivalDecision& decision)
    {
        SIMLOG(m_logger,
            "[Verbose][ResimProbe.Landing] id=%u tick=%u class=%s site=%s",
            id, decision.tick,
            predictedCharacterClassName(decision.characterClass),
            correctionLandingSiteName(decision.landingSite));

        CorrectionLandingWindowSummary landingWindow;
        if (m_correctionLandingProbe.noteLanding(
                decision.characterClass, decision.landingSite, landingWindow))
        {
            emitCorrectionLandingClassLine(
                PredictedCharacterClass::LocallyPredicted,
                landingWindow.local, landingWindow.samples);
            emitCorrectionLandingClassLine(
                PredictedCharacterClass::RemoteProxy,
                landingWindow.remote, landingWindow.samples);
        }

        // A correction whose tick had no slot was DISCARDED — no comparison
        // happened. Counting it would put a denominator under a verdict that
        // was never reached, and the discard path already logs itself
        // (isAnomalousMiss-gated, in the cache).
        //
        // [item 42] THE LANDING PROBE ABOVE DELIBERATELY SITS ON THE OTHER
        // SIDE OF THIS RETURN. Its `discarded` bucket is the one place the
        // two probes' sample sets are required to differ, and moving this
        // gate up would silently empty it. [RN-10 part C] `decision.landed`
        // carries the exact same fact `verdict.landed` did before the split
        // — see `SimulationNetSync::decideCorrectionArrival`'s comment for
        // why it is a field rather than re-derived here.
        if (!decision.landed)
            return;

        // PER-EVENT DETAIL AT VERBOSE — off under the shipped
        // LogOGDivergenceProbe=Warning. Emitted on EVERY landed correction
        // rather than on disagreements only, because "per-correction verdict
        // observable with id and class" is the acceptance criterion and a
        // disagreement-only line cannot distinguish "predicted correctly"
        // from "no correction arrived". The cost is one snprintf at a site
        // that already performs two per correction ([InjectCorrectionState]
        // and the cache's own line), so this adds no new volume CLASS — the
        // thing T19 was filed to stop.
        SIMLOG(m_logger,
            "[Verbose][DivergenceProbe.Correction] id=%u tick=%u class=%s correct=%u",
            id, decision.tick,
            predictedCharacterClassName(decision.characterClass),
            decision.predictionWasCorrect ? 1u : 0u);

        CorrectionVerdictWindowSummary window;
        if (!m_correctionVerdictProbe.noteCorrection(
                decision.characterClass, decision.predictionWasCorrect, window))
            return;

        // PER-WINDOW SUMMARY AT WARNING, ONE LINE PER CLASS. Never one
        // pooled line: only the remote half can move with the relay delay
        // floor, and summing it with a locally-predicted population that
        // cannot move would dilute exactly the signal T23 scenario 4 reads.
        //
        // A class with no corrections in the window is SKIPPED rather than
        // printed as `rate=0`, which would read as a perfect record instead
        // of as no observation. That is also the steady state on a client
        // with no remote proxies.
        emitCorrectionVerdictClassLine(
            PredictedCharacterClass::LocallyPredicted, window.local, window.samples);
        emitCorrectionVerdictClassLine(
            PredictedCharacterClass::RemoteProxy, window.remote, window.samples);
    }

    // [T24, relocated task 79] ONE per-window class block of the
    // correction-verdict summary. Called twice — once per class — from
    // `emitCorrectionArrival` above, and ONLY when a window closed.
    //
    // SILENT ON AN EMPTY CLASS. `corrections == 0` means this window observed
    // nothing about that class; printing `disagreed=0 ratePerMille=0` would assert
    // a perfect record where there is no record at all, and that misreading is
    // precisely the one a benefit claim would be built on. It is also the steady
    // state for the remote block on a client that has no proxies and for the local
    // block on a spectator, so gating it keeps those sessions quiet as well.
    //
    // ONE LINE PER CLASS, NEVER ONE POOLED LINE — the reason is at the top of
    // Network/CorrectionVerdictProbe.h and is the whole point of the split.
    void emitCorrectionVerdictClassLine(PredictedCharacterClass characterClass,
                                        const CorrectionVerdictClassSummary& classSummary,
                                        std::uint32_t windowSamples)
    {
        if (classSummary.corrections == 0u)
        {
            return;
        }

        SIMLOG(m_logger,
            "[Warning][DivergenceProbe.Window] class=%s corrections=%u disagreed=%u "
            "ratePerMille=%u windowSamples=%u",
            predictedCharacterClassName(characterClass),
            classSummary.corrections, classSummary.disagreements,
            classSummary.disagreementRatePerMille, windowSamples);
    }

    // [og-netcode-v2-input-relay item 42 / I2, relocated task 79] ONE per-window
    // class block of the frontier-landing split. Called twice — once per class —
    // from `emitCorrectionArrival` above, and ONLY when a window closed.
    //
    // SILENT ON AN EMPTY CLASS, same rule and same reason as the verdict line
    // above: printing `behind=0 atFrontier=0 discarded=0` would assert a perfect
    // record where there is no record at all, and that is the misreading this whole
    // instrument exists to prevent. It is also the steady state for the remote
    // block on a client with no proxies.
    //
    // ⭐ HOW TO READ THE PAIR THIS LINE FORMS WITH `[ResimProbe.Gate]`. Under the
    // mechanism, resim triggers track `atFrontier` and are blind to `behind`. So:
    //   * `atFrontierPerMille` here ~= `requestedPerMille` on the Gate line  ⇒ the
    //     finding's central claim reproducing live;
    //   * `behind` large with the Gate line's `requested` small  ⇒ the suppressed-
    //     correction population, i.e. the under-resimulation statement itself;
    //   * `discarded` large  ⇒ item 41's `aboveNewest` anomaly, whose fix will MOVE
    //     this mix (it is referenced here, not solved here).
    // The two lines cannot be merged into one: they are fed by different threads
    // and item 42 requires one probe per thread with no sharing. See the cost note
    // at the top of ResimGateProbe.h.
    void emitCorrectionLandingClassLine(PredictedCharacterClass characterClass,
                                        const CorrectionLandingClassSummary& classSummary,
                                        std::uint32_t windowSamples)
    {
        if (classSummary.total() == 0u)
        {
            return;
        }

        SIMLOG(m_logger,
            "[Warning][ResimProbe.Landing] class=%s behind=%u atFrontier=%u discarded=%u "
            "atFrontierPerMille=%u windowSamples=%u",
            predictedCharacterClassName(characterClass),
            classSummary.landedBehind, classSummary.landedAtFrontier,
            classSummary.discarded, classSummary.atFrontierRatePerMille, windowSamples);
    }

    // [T19, relocated task 79] PROBES 1 + 3 — the per-window summary. Called once
    // per prediction tick from `SimulationNetSync::collectInputAll`; silent unless
    // a window both CLOSED and carried at least one scheduled read, so the
    // authority (no relay stores, so neither call site is ever reached) and an
    // idle client never heartbeat a Warning line.
    //
    // TWO LINES, ONE PER CALL SITE, plus a third only when a stale run occurred.
    // Split rather than concatenated because a single line carrying both blocks runs
    // close to SIMLOG's 256-byte buffer once the counts reach five digits, and a
    // silently truncated telemetry line is worse than no line.
    void emitRelayReadWindowIfDue(uint32 predictionTick)
    {
        RelayReadWindowSummary summary;
        if (!m_relayReadProbe.maybeCloseWindow(predictionTick, summary))
        {
            return;
        }

        SIMLOG(m_logger,
            "[Warning][RelayProbe.Read] window=[%u,%u] src=Prediction hit=%u miss=%u "
            "verifyFail=%u rung0=%u total=%u",
            summary.windowStartTick, summary.windowEndTick,
            summary.prediction.hit, summary.prediction.miss,
            summary.prediction.verifyFail, summary.prediction.noProbe,
            summary.prediction.total());

        SIMLOG(m_logger,
            "[Warning][RelayProbe.Read] window=[%u,%u] src=Resim hit=%u miss=%u "
            "verifyFail=%u rung0=%u total=%u",
            summary.windowStartTick, summary.windowEndTick,
            summary.resim.hit, summary.resim.miss,
            summary.resim.verifyFail, summary.resim.noProbe,
            summary.resim.total());

        // [T20] PROBE B — the miss PARTITION and the signed-delta distribution, per
        // call site. SEPARATE LINES rather than more fields on the two above: the
        // existing lines already run to ~130 characters and SIMLOG's buffer is 256,
        // so folding ten more five-digit counters in would silently truncate exactly
        // when the counts get interesting. Each is gated so a call site that carried
        // nothing (the resim block on a client that never resimmed) stays silent.
        emitMissClassLine(summary, summary.prediction, "Prediction");
        emitMissClassLine(summary, summary.resim,      "Resim");
        emitDeltaLine(summary, summary.prediction, "Prediction");
        emitDeltaLine(summary, summary.resim,      "Resim");

        // PROBE 3 — the D4 stale window, which is what sets `K` for the deferred
        // stale-hold rule. Silent when nothing went stale, which is the healthy
        // state; rung-0 serves are excluded from the run (review F5), so a join
        // window does not produce one.
        if (summary.maxConsecutiveFallbackRun > 0u)
        {
            SIMLOG(m_logger,
                "[Warning][RelayProbe.Stale] window=[%u,%u] maxConsecutiveFallbackRun=%u id=%u",
                summary.windowStartTick, summary.windowEndTick,
                summary.maxConsecutiveFallbackRun, summary.maxConsecutiveFallbackId);
        }
    }

    // [T20, relocated task 79] PROBE B — the miss partition for ONE call site.
    // Silent when that call site missed nothing, so a healthy window costs no
    // line.
    //
    // THE THREE COUNTERS ARE THE WHOLE POINT OF T20 and they answer three different
    // questions: `inSpan` is the coverage hole raising the relay depth would close;
    // `aboveNewest` is the delay deficit, which depth cannot touch; `belowOldest` is
    // a clock or capacity fault. `noProbeTick` is the early-session underflow guard,
    // reported alongside so the four always visibly sum to `miss`.
    void emitMissClassLine(const RelayReadWindowSummary& summary,
                           const RelayReadCounters& counters, const char* site)
    {
        if (counters.miss == 0u)
        {
            return;
        }

        SIMLOG(m_logger,
            "[Warning][RelayProbe.Miss] window=[%u,%u] src=%s inSpan=%u aboveNewest=%u "
            "belowOldest=%u noProbeTick=%u miss=%u",
            summary.windowStartTick, summary.windowEndTick, site,
            counters.missInSpan, counters.missAboveNewest,
            counters.missBelowOldest, counters.missNoProbeTick,
            counters.miss);
    }

    // [T20, relocated task 79] PROBE B — the signed `probeTick - newestResident`
    // distribution for ONE call site. At depth 1 this is the richer signal: it
    // says WHERE the receiver is asking relative to what it holds, continuously,
    // rather than in three buckets. A window whose p50 sits above 0 is a receiver
    // reading ahead of its data (no depth helps); one whose p50 sits below 0 while
    // missing is reading inside a span full of holes (depth does).
    void emitDeltaLine(const RelayReadWindowSummary& summary,
                       const RelayReadCounters& counters, const char* site)
    {
        RelayDeltaSummary delta;
        counters.delta.fillSummary(delta);
        if (delta.samples == 0u)
        {
            return;
        }

        SIMLOG(m_logger,
            "[Warning][RelayProbe.Delta] window=[%u,%u] src=%s n=%u p10=%d p50=%d "
            "p90=%d min=%d max=%d satLow=%u satHigh=%u",
            summary.windowStartTick, summary.windowEndTick, site,
            delta.samples, delta.p10, delta.p50, delta.p90,
            delta.minDelta, delta.maxDelta,
            delta.saturatedLow, delta.saturatedHigh);
    }

    // [task 79] ONE-SHOT gate for the wire-version-mismatch log, id-keyed —
    // relocated from `RemoteInputCache::shouldLogVersionMismatchOnce` (see the
    // file banner and `emitRelayArrival` above for why it moved rather than
    // being handed in by reference). Returns true exactly once per id, i.e.
    // exactly once PER CHARACTER PER SESSION — the same cadence the fence
    // required when the latch lived on the store, because an incompatible peer
    // re-replicates its ring forever and an ungated log would fire on every
    // single replication. Forgotten in `forgetOwner` for the same
    // unbounded-memo reason the two relay probes are.
    //
    // PUBLIC, same as the store's version was: this is the one piece of
    // task 79 that is genuinely NEW behaviour rather than a relocation (a
    // per-store latch becoming an id-keyed one on a shared object), and
    // `NetSyncTelemetryTest.cpp` exercises it directly rather than only
    // indirectly through `emitRelayArrival`'s no-logger path.
    bool shouldLogVersionMismatchOnce(unsigned int id)
    {
        return m_versionMismatchLogged.try_emplace(id, true).second;
    }

private:
    std::function<void(const char*)> m_logger;

    // [T19] THE TWO CLIENT-SIDE RELAY PROBES. Pure telemetry: nothing in the
    // resolution path reads them, and every consumer is a SIMLOG.
    // [item 83 / 79 f1] STATED DIRECTLY, not only by cross-reference: this pair
    // is declared adjacent — exactly as it was before task 79's move — but each
    // member is owned by a DIFFERENT thread. The two-thread rule at the top of
    // this file states the full method-level rule once; the tags below answer
    // "which thread may touch THIS member" without requiring that extra hop.
    // See also Network/RelayReadProbe.h's own header banner for why they are
    // two objects rather than one.
    RelayReadProbe    m_relayReadProbe;    // PHYSICS thread only.
    RelayArrivalProbe m_relayArrivalProbe; // GAME thread only.

    // [T24] THE CORRECTION-VERDICT PROBE. Also pure telemetry, also client-side,
    // and — unlike the pair above — a SINGLE object, because it has a single
    // feeder: the OnRep-dispatched correction-state callback, on the GAME
    // thread. There is no physics-thread correction arrival, so there is no
    // second window to keep apart. It also carries no per-id state, which is
    // why `forgetOwner` above forgets the two relay probes and not this one.
    // Full statement at the top of Network/CorrectionVerdictProbe.h.
    CorrectionVerdictProbe m_correctionVerdictProbe;

    // [og-netcode-v2-input-relay item 42 / I2] THE FRONTIER-LANDING SPLIT. Same
    // feeder, same thread and same no-per-id-state rule as the verdict probe above
    // — it is deliberately a second object on the same site rather than three more
    // fields on the first, because the two answer different questions on different
    // denominators (a DISCARDED correction is a non-event for the verdict and a
    // first-class observation for the landing site) and merging them would have
    // forced one of the two to adopt the other's sample set.
    //
    // ITS PHYSICS-THREAD SIBLING IS ON SimulationManager (ResimGateProbe). The two
    // are never shared and never atomic — see the two-object rule at the top of
    // OGSimulation/ResimGateProbe.h.
    CorrectionLandingProbe m_correctionLandingProbe;

    // [task 79] See `shouldLogVersionMismatchOnce` above.
    std::unordered_map<unsigned int, bool> m_versionMismatchLogged;
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
