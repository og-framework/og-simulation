#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include <cstdint>
#include <functional>

#include "OGSimulation/Network/RelayReadProbe.h"
#include "OGSimulation/SimulationLog.h"
#include "OGSimulation/SimulationReconciliation.h"
#include "OGSimulation/SimulationTimeContext.h"

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// InputResolutionTelemetry — `SimulationInputResolution`'s PHYSICS-THREAD
// instrument: it owns every probe write and every shipped log line on the
// input-resolution path.
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
//
// Relocation history, retired rationale and archived measurement records:
// `docs/Telemetry-rationale.md` — the `§N` marks below are its sections.
//
// ---------------------------------------------------------------------------
// ORIENTATION — WHO OWNS THIS, WHO CALLS IT, ON WHICH THREAD, AND HOW LOUD.
//
// Read this first. Every fence in this file states one invariant at the line
// it guards; none of them restates this map, and this map states no invariant.
//
//   * OWNERSHIP. `SimulationInputResolution` owns this object
//     (`m_inputResolutionTelemetry`). The GAME-thread sibling
//     `NetSyncTelemetry` is owned by `SimulationNetSync` (`m_telemetry`).
//     Neither sibling holds a reference to the other, and `SimulationNetSync`
//     does not reach this class at all.
//
//   * EVERY METHOD HERE IS PHYSICS THREAD ONLY. Every call site is a method
//     of `SimulationInputResolution`:
//
//       emitRelayReadWindowIfDue                    collectInputAll
//       emitLocalInputRead                          collectInputForCharacter
//       emitRemoteQueueRead                         collectInputForCharacter
//       emitPredictionInputRead                     collectInputForCharacter
//       emitResimNoSlot / Sentinel / LocalRead /    collectResimInput-
//         NoStore / RefRead / ScheduledRead           ForCharacter
//       forgetOwner                                 forgetOwner, lifecycle
//
//     `emitMissClassLine` / `emitDeltaLine` are private and reached only from
//     `emitRelayReadWindowIfDue`.
//
//   * THE READ SEAM IS NOT ON THIS CLASS.
//     `SimulationInputResolution::Diagnostics` exposes ONE const probe,
//     `relayReadProbe()`, delegating in here; it is reached as
//     `inputResolution.getDiagnostics().relayReadProbe()`.
//
//   * VOLUME ROSTER — WHAT EACH HELPER PUTS ON THE LOG IN THE STEADY STATE
//     (no rare event, no window closing). This is the contract T19 exists to
//     hold; the fences below defend the individual gates that keep it.
//
//       helper                     steady state             rare / on close
//       emitLocalInputRead         1 line PER TICK          —
//       emitRemoteQueueRead        1 line PER TICK          —
//       emitPredictionInputRead    1 line PER TICK          +1 Verbose on
//                                                           VerifyFail or
//                                                           BelowOldest
//       the six emitResim*         1 line per RESIM tick    +1 Verbose on
//                                  (resim only — not the    VerifyFail
//                                  steady state at all)
//       emitRelayReadWindowIfDue   NOTHING                  2 Warning, plus up
//                                                           to 4 gated Warning,
//                                                           plus 1 if stale
//       forgetOwner                NOTHING                  never logs
//
//     ⇒ The three `[CollectInput]` classification lines are the ONLY per-tick
//       output of this class; every `[RelayProbe.*]` line it emits is a
//       per-WINDOW or a rare-event line. That asymmetry is the whole point,
//       and the per-method fences below are what hold it in place.
//     ⇒ ON THE AUTHORITY THIS CLASS IS SILENT: neither collect path is reached
//       there, so no window ever closes and no line is ever emitted.
//
//   * SEVERITY, AND WHY `Log` IS NOT AN OPTION HERE. Every per-window summary
//     ships at Warning and every per-event detail at Verbose. The adapter
//     routes on the tag prefix, and the host project's log configuration sets
//     every category it can route to at Warning, so a `Log`-severity line DOES
//     NOT EXIST in a shipped build. One adapter's binding: `RouteOGMessage`
//     (`SimulationManagerUImpl.cpp`) reading `Config/DefaultEngine.ini`:
//
//       [RelayProbe.*]    LogOGRelayProbe=Warning
//       [CollectInput]    LogOGSimTick=Warning
//       [Resim.Input]     LogOGSimTick=Warning
//
//     ⚠ THIS IS A CROSS-REPO JOIN. That configuration lives in the host game
//     project; this header ships standalone in the engine-free submodule, so
//     neither half can check the other. Re-read the live configuration before
//     trusting a severity here.
//
// The vocabulary these names follow — `note*` / `emit*` / `log*`, and why an
// instrument owner is not a diagnostics view — is stated once in
// `docs/DiagnosticsConventions.md` §2/§3 and is not re-derived here.
// ---------------------------------------------------------------------------
//
// ⛔ TWO SIBLINGS, TWO OWNERS, NO LINK — `SimulationNetSync` owns `NetSyncTelemetry`; neither references the other, and merging them re-crosses a thread boundary. §1
//
// ⛔ AN OWNER, NOT A DIAGNOSTICS VIEW — `SimulationInputResolution` calls into this class on every prediction and every resim tick, unconditionally. §2
// ⛔ Nothing here groups behind a `getDiagnostics()` except the one const probe accessor its own view delegates into. Rule: `docs/DiagnosticsConventions.md` §2.
//
// ⛔ NO REFERENCE INTO A PRODUCTION CONTAINER IS EVER HANDED IN — the same ruling the GAME-thread sibling carries; nothing about it was thread-specific. §2
//
// ⛔ `emitMissClassLine` / `emitDeltaLine` ARE PRIVATE AND MUST STAY SO — their sole caller `emitRelayReadWindowIfDue` is a member of this same class. §10
//
// ⛔ EVERY METHOD ON THIS CLASS IS PHYSICS THREAD ONLY, so no member here is atomic and none may be made concurrent. §3
// ⛔ `forgetOwner` is the one either-thread member and is never concurrent by construction — reached from `SimulationNetSync::unregisterSimulatable` via `SimulationInputResolution::forgetOwner`. §3
// ⛔ The const probe accessor is the exception, callable from either thread — which is why `SimulationInputResolution::Diagnostics` may reach it from wherever a test likes. §11
class InputResolutionTelemetry
{
public:
    void setLogger(std::function<void(const char*)> logger)
    {
        m_logger = std::move(logger);
    }

    // ⛔ CONST-ONLY — `SimulationInputResolution::Diagnostics::relayReadProbe()` delegates into this; there is no mutable counterpart. §11
    const RelayReadProbe& relayReadProbe() const { return m_relayReadProbe; }

    // ⛔ LIFECYCLE CLEANUP, NOT DIAGNOSTICS — one call, from `SimulationNetSync::unregisterSimulatable`'s fixed step 4, alongside `NetSyncTelemetry::forgetOwner`. §1
    // ⛔ Without it the probe's id-keyed stale-run state grows with every character the session has ever had. §11
    void forgetOwner(unsigned int id)
    {
        m_relayReadProbe.forgetOwner(id);
    }

    void emitLocalInputRead(unsigned int id, uint32 tick, StepKind stepKind, int32 effectiveDelay)
    {
        SIMLOG(m_logger,
            "[CollectInput] id=%u tick=%u source=Provider kind=%s delay=%d",
            id, tick, stepKindName(stepKind), effectiveDelay);
    }

    void emitRemoteQueueRead(unsigned int id, uint32 tick, uint32 queuedTick)
    {
        SIMLOG(m_logger, "[CollectInput] id=%u tick=%u source=RemoteQueue queuedTick=%u",
            id, tick, queuedTick);
    }

    // ⛔ `hasStore` IS A BIT, NOT A POINTER — this method never dereferenced the store, only null-checked it, so no reference into `SimulationInputResolution`'s `RemoteInputCache` map crosses in here. §2
    void emitPredictionInputRead(unsigned int id, uint32 tick,
                                 bool hasStore,
                                 const ScheduledRelayedReadReport& readReport)
    {
        if (hasStore)
        {
            // ⛔ The WHOLE report, not just the outcome — the miss class and the signed probe-to-newest delta are tallied here too. §10
            m_relayReadProbe.notePredictionRead(id, readReport);

            // ⛔ VERBOSE ONLY FOR THE OUTCOME THAT IS SILENT IN THE STEADY STATE — a verify-fail means the delay regime moved under this reader, which does not happen while the schedule is stable. §9
            // ⛔ Hits and misses are RATES and are reported by the per-window summary, never per event. §9
            if (readReport.outcome == ScheduledRelayedReadOutcome::VerifyFail)
            {
                SIMLOG(m_logger,
                    "[Verbose][RelayProbe.Read] id=%u tick=%u VERIFY-FAIL probeTick=%u "
                    "candidateDA=%u dLatest=%u src=Prediction",
                    id, tick, readReport.probeTick,
                    static_cast<unsigned int>(readReport.candidateDA),
                    static_cast<unsigned int>(readReport.dLatest));
            }

            // ⛔ THE OTHER OUTCOME THAT IS SILENT IN THE STEADY STATE. `missInSpan` / `missAboveNewest` are the two expected classes and are RATES. §9
            // ⛔ A read landing below the oldest resident entry is different IN KIND — the clock has drifted out of the store's reach — so it gets the same rare-event treatment. §9
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

    void emitResimNoSlot(unsigned int id, uint32 tick)
    {
        SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=NoSlot", id, tick);
    }

    void emitResimSentinel(unsigned int id, uint32 tick)
    {
        SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=Sentinel", id, tick);
    }

    void emitResimLocalRead(unsigned int id, uint32 tick, AppliedCaptureRefKind refKind, int32 captureTick)
    {
        SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=%s src=LocalInputCache capture=%d",
            id, tick, refKind == AppliedCaptureRefKind::Ref ? "Ref" : "NoRef", captureTick);
    }

    void emitResimNoStore(unsigned int id, uint32 tick)
    {
        SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=Remote src=NoStore", id, tick);
    }

    void emitResimRefRead(unsigned int id, uint32 tick, uint32 captureTick, bool hit)
    {
        SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=Ref src=RemoteInputCache ref=%u hit=%d",
            id, tick, captureTick, hit ? 1 : 0);
    }

    // ⛔ THE ONE RUNG WITH A PROBE WRITE (`noteResimRead`) — it folds because nothing downstream in `collectResimInputForCharacter` branches on whether the probe fired. §9
    void emitResimScheduledRead(unsigned int id, uint32 tick, const ScheduledRelayedReadReport& readReport)
    {
        // ⛔ The whole report — same contract as the prediction site. §10
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

    // ⛔ SILENT UNLESS A WINDOW BOTH CLOSED AND CARRIED A SCHEDULED READ. Called once per prediction tick, from `SimulationInputResolution::collectInputAll`. §10
    // ⛔ The authority reaches neither collect path, so neither it nor an idle client ever heartbeats a Warning line. §10
    //
    // ⛔ TWO LINES, ONE PER CALL SITE, NEVER CONCATENATED — one line carrying both blocks runs close to `SIMLOG`'s 256-byte buffer at five-digit counts, and a silent truncation is worse than no line. §10
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

        // ⛔ SEPARATE LINES, for the same 256-byte `SIMLOG` reason — ten more five-digit counters on the two window lines would truncate exactly when the counts get interesting. §10
        // ⛔ Each is gated, so a call site that carried nothing is silent rather than zero. §10
        emitMissClassLine(summary, summary.prediction, "Prediction");
        emitMissClassLine(summary, summary.resim,      "Resim");
        emitDeltaLine(summary, summary.prediction, "Prediction");
        emitDeltaLine(summary, summary.resim,      "Resim");

        // ⛔ SILENT WHEN NOTHING WENT STALE, which is the healthy state; `rung-0` serves are DELIBERATELY excluded from the run, so a join window produces none. §10
        if (summary.maxConsecutiveFallbackRun > 0u)
        {
            SIMLOG(m_logger,
                "[Warning][RelayProbe.Stale] window=[%u,%u] maxConsecutiveFallbackRun=%u id=%u",
                summary.windowStartTick, summary.windowEndTick,
                summary.maxConsecutiveFallbackRun, summary.maxConsecutiveFallbackId);
        }
    }

private:
    // ⛔ PRIVATE; sole caller is `emitRelayReadWindowIfDue`, in this class. Silent when that call site missed nothing. §10
    // ⛔ THE FOUR COUNTERS ANSWER FOUR DIFFERENT QUESTIONS AND MUST VISIBLY SUM TO `miss`. §10
    // ⛔ `inSpan` is the coverage hole raising depth would close, `aboveNewest` the delay deficit depth cannot touch, `belowOldest` a clock or capacity fault, `noProbeTick` the early-session underflow guard. §10
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

    // ⛔ PRIVATE; sole caller is `emitRelayReadWindowIfDue`, same as `emitMissClassLine`. §10
    // ⛔ The signed `probeTick - newestResident` distribution — a p50 above 0 is a receiver reading ahead of its data, which no depth helps; a p50 below 0 while missing is a holed span, which depth does. §10
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

    std::function<void(const char*)> m_logger;

    // ⛔ PHYSICS THREAD ONLY, and pure telemetry — nothing in the resolution path reads it, and every consumer is a SIMLOG. §11
    // ⛔ Its GAME-thread sibling `RelayArrivalProbe` is on `NetSyncTelemetry` — two objects because two threads. §3
    RelayReadProbe m_relayReadProbe;
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
