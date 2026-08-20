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

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// ---------------------------------------------------------------------------
// [og-netcode-v2-input-relay task 79 / architecture review B3; SPLIT AT ITEM 85]
// NetSyncTelemetry — SimulationNetSync's sibling instrument, finishing the
// decide/project pattern RN-8/RN-10/RN-11 already established for this class
// (task 58/60/61): the peer computes and returns decision/report structs on
// its production path (`ScheduledRelayedReadDecision` — now on the split-off
// sibling, see below — `CorrectionArrivalDecision` below,
// `RelayedInputIngestReport`), and THIS object owns everything that turns
// its own (GAME-THREAD) structs into a probe count or a shipped log line.
//
// ⚠ ITEM 85 SPLIT THIS CLASS ALONG ITS OWN TWO-THREAD BANNER (design C.6):
// the PHYSICS-THREAD-ONLY half — `emitLocalInputRead`, `emitRemoteQueueRead`,
// `emitPredictionInputRead`, the six `emitResim*`, `emitRelayReadWindowIfDue`
// (+ its two now-private helpers), the `RelayReadProbe` member, and that
// probe's `forgetOwner` half — MOVED OUT to `InputResolutionTelemetry.h`,
// verbatim. THIS class keeps the GAME-THREAD-ONLY half: `emitRelayArrival`
// (+ the version-mismatch latch), `emitCorrectionArrival` (+ the two
// class-line helpers), the three GT probe members
// (`RelayArrivalProbe`, `CorrectionVerdictProbe`, `CorrectionLandingProbe`),
// and this class's own `forgetOwner` half. See `InputResolutionTelemetry.h`
// for its half of this same banner, and design C.6 / this item's impl notes
// for the full cut-line rationale. `docs/DiagnosticsConventions.md`'s roster
// is updated to match.
//
// ⚠ NETSYNC TEMPORARILY OWNS BOTH SIBLINGS. `InputResolutionTelemetry` does
// not yet belong to an input-resolution peer — that peer does not exist
// until item 86 — so `SimulationNetSync` holds one of each
// (`m_telemetry` / `m_inputResolutionTelemetry`) for now. That is correct for
// this step, not a leftover; see `InputResolutionTelemetry.h`'s own note.
//
// ⛔ THIS IS AN OWNER, NOT A DIAGNOSTICS VIEW — DiagnosticsConventions.md §2 STILL
// APPLIES, UNCHANGED IN OUTCOME. SimulationNetSync's production methods call
// straight into this class on every arrival / every correction — the
// OnRep-bound relay-arrival and correction callbacks, and
// `unregisterSimulatable`'s lifecycle cleanup, all reach a public method here,
// unconditionally, on the game thread. Nothing on this class is reachable
// from `SimulationNetSync::getDiagnostics()` — that view still exposes the
// four probes, CONST, as it did before this split; three delegate into this
// object's own const accessors below, one (`relayReadProbe()`) now delegates
// into `InputResolutionTelemetry` instead — see
// `Diagnostics::relayReadProbe()` etc. on `SimulationNetSync`. "instrument =
// the sibling object" is still structure, not prose — this class (and its new
// PT sibling) IS the fence.
//
// THE ONE THING DELIBERATELY NOT HANDED IN: a reference into a production
// container. `emitRelayArrival` used to take the arriving `RemoteInputCache<InputT>&`
// store purely to call its one-shot `shouldLogVersionMismatchOnce()` latch —
// see that method below for why the latch moved IN here (id-keyed) instead of a
// pointer moving in. Handing this class a live reference into
// `SimulationNetSync`'s per-id maps would undercut the split this task exists to
// make: a telemetry object that can read (or, worse, outlive) a production
// container is no longer just an instrument. Unaffected by item 85 — the ruling
// was never PT/GT-specific.
//
// -----------------------------------------------------------------------------
// THE TWO-THREAD RULE, AS A CLASS PROPERTY (task 79's stated deliverable; now
// trivially true rather than merely stated, per item 85's split — every
// method left on this class is GAME THREAD ONLY):
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
//     forgetOwner (this class's GT half — see `InputResolutionTelemetry.h`
//     for the PT half, called from the same unregistration site).
//
//   CONST, either thread: the three probe accessors — read-only, and the
//   reason `SimulationNetSync::Diagnostics` may call them from wherever a
//   test likes.
//
// No method here is ever called from both threads, so nothing on this class is
// atomic — the same property the probe split existed to buy, now stated as a
// method-level fence instead of an object-level one (and, since item 85, also
// an OBJECT-level one: this class is single-threaded in its entirety, bar the
// never-concurrent `forgetOwner`). This complements, and does not restate,
// `Network/RelayReadProbe.h`'s own "two objects because there are two
// threads" banner: that one is about the PROBE TYPES; this one is about which
// of THIS class's METHODS may be called from which thread.
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
    // [task 59, retargeted task 79, split at item 85] THE THREE GT PROBE
    // ACCESSORS — CONST-ONLY, same contract as before this task.
    // `SimulationNetSync::Diagnostics` delegates into these rather than
    // reading its own members directly; every test call site on that view is
    // unchanged (see the diagnostics banner on SimulationNetSync for the full
    // rationale, and `docs/DiagnosticsConventions.md` §2/§3 for the
    // classification these views follow — not re-derived here).
    // `relayReadProbe()` MOVED to `InputResolutionTelemetry` at item 85 (it
    // was the PT-only probe); `SimulationNetSync::Diagnostics::relayReadProbe()`
    // now delegates there instead of here.
    // =======================================================================
    const RelayArrivalProbe& relayArrivalProbe() const { return m_relayArrivalProbe; }
    const CorrectionVerdictProbe& correctionVerdictProbe() const { return m_correctionVerdictProbe; }
    const CorrectionLandingProbe& correctionLandingProbe() const { return m_correctionLandingProbe; }

    // [T19, relocated task 79, split at item 85] LIFECYCLE CLEANUP, NOT
    // DIAGNOSTICS — called once from `SimulationNetSync::unregisterSimulatable`,
    // itself unchanged: step 4 of that fixed, load-bearing ordering, ALONGSIDE
    // `InputResolutionTelemetry::forgetOwner` (the PT half — see that class).
    // `m_relayArrivalProbe` holds an id-keyed map (the stale run), and without
    // this it would grow with every character that has ever existed in the
    // session — the unbounded-memo shape this codebase has already had to fix
    // once in the log throttles. The version-mismatch latch (below) is the
    // same shape and is forgotten here for the same reason.
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
        m_relayArrivalProbe.forgetOwner(id);
        m_versionMismatchLogged.erase(id);
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

    // [task 79] ONE-SHOT gate for the wire-version-mismatch log, id-keyed —
    // relocated from `RemoteInputCache::shouldLogVersionMismatchOnce` (see the
    // file banner and `emitRelayArrival` above for why it moved rather than
    // being handed in by reference). Returns true exactly once per id, i.e.
    // exactly once PER CHARACTER PER SESSION — the same cadence the fence
    // required when the latch lived on the store, because an incompatible peer
    // re-replicates its ring forever and an ungated log would fire on every
    // single replication. Forgotten in `forgetOwner` for the same
    // unbounded-memo reason `m_relayArrivalProbe` is.
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

    // [T19; SPLIT AT ITEM 85] THE CLIENT-SIDE RELAY ARRIVAL PROBE. Pure
    // telemetry: nothing in the resolution path reads it, and every consumer
    // is a SIMLOG. GAME thread only — its PHYSICS-thread sibling
    // (`RelayReadProbe`, formerly declared adjacent here) moved to
    // `InputResolutionTelemetry` at item 85; the two were always owned by a
    // DIFFERENT thread each (see that class's own note and
    // Network/RelayReadProbe.h's header banner for why they were ever two
    // objects rather than one — unchanged by the split, only their storage
    // location moved).
    RelayArrivalProbe m_relayArrivalProbe; // GAME thread only.

    // [T24] THE CORRECTION-VERDICT PROBE. Also pure telemetry, also client-side,
    // and — unlike `m_relayArrivalProbe` above — a SINGLE object, because it has
    // a single feeder: the OnRep-dispatched correction-state callback, on the
    // GAME thread. There is no physics-thread correction arrival, so there is
    // no second window to keep apart. It also carries no per-id state, which is
    // why `forgetOwner` above forgets `m_relayArrivalProbe` and not this one.
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
