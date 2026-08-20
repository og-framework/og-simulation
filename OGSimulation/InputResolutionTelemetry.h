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

// ---------------------------------------------------------------------------
// [og-netcode-v2-input-relay item 85 / step 1 of the input-resolution
// migration] InputResolutionTelemetry — the PHYSICS-THREAD half of the sibling
// task 79 created (`NetSyncTelemetry`), split out along that class's own
// two-thread banner exactly as design C.6 specifies. This is the cut, not a
// redesign: every member below moved here VERBATIM from `NetSyncTelemetry.h`;
// nothing was renamed, reshaped or re-derived.
//
// WHY THE SPLIT HAPPENS BEFORE THE STATE MOVES (item 79's own gate logic,
// restated at C.6): if the resolution peer's state and logic moved first,
// each of the sixteen `emit*` helpers would have to be assigned to the
// transport peer or the resolution peer WHILE the state was also moving —
// two hard decisions entangled in one diff. This step finishes the
// assignment while nothing else changes; step 2 (item 86) moves the state
// behind an unchanged public surface with this split already settled.
//
// WHAT MOVED HERE (verbatim): the PHYSICS-THREAD-ONLY group of `emit*`
// helpers — `emitLocalInputRead`, `emitRemoteQueueRead`,
// `emitPredictionInputRead`, the six `emitResim*`, `emitRelayReadWindowIfDue`
// (and, with it, its two now-private helpers `emitMissClassLine` /
// `emitDeltaLine` — see the B-5 visibility note below) — the `RelayReadProbe`
// member, and a `forgetOwner(id)` erasing the probe's id-keyed state.
// `NetSyncTelemetry` keeps the GAME-THREAD-ONLY group unmodified in shape;
// see that header for its own half of this same banner and design C.6 for
// the full cut-line rationale.
//
// ⚠ NETSYNC TEMPORARILY OWNS BOTH SIBLINGS. This class is not yet owned by an
// input-resolution peer — that peer does not exist until item 86 — so
// `SimulationNetSync` holds one of each sibling for now
// (`m_telemetry` / `m_inputResolutionTelemetry`) and calls straight into
// both, unconditionally, on the same call sites it always used. That is
// correct for this step, not a leftover.
//
// ⛔ THIS IS AN OWNER, NOT A DIAGNOSTICS VIEW — DiagnosticsConventions.md §2
// STILL APPLIES, UNCHANGED IN OUTCOME. `SimulationNetSync`'s production
// methods call straight into this class on every prediction / resim tick,
// unconditionally, on the physics thread. Nothing on this class is reachable
// from `SimulationNetSync::getDiagnostics()` except the one CONST probe
// accessor below, exactly as before the split — see
// `Diagnostics::relayReadProbe()` on `SimulationNetSync`, which now delegates
// into THIS object's const accessor instead of `NetSyncTelemetry`'s.
//
// THE ONE THING DELIBERATELY NOT HANDED IN: a reference into a production
// container — unchanged from task 79's own ruling (see `NetSyncTelemetry.h`'s
// file banner; nothing about that ruling was PT-specific, so it carries here
// unmodified).
//
// [B-5 CORRECTION, folded in at item 85] `emitMissClassLine` / `emitDeltaLine`
// were PUBLIC on `NetSyncTelemetry` (design C.6 called them private, which was
// wrong on the current-tree fact at design time — review finding B-5(i)).
// On THIS sibling they are made PRIVATE: their sole caller
// (`emitRelayReadWindowIfDue`) is a member of this same class, no test or
// production call site reaches them directly (grep-verified against
// `NetSyncTelemetryTest.cpp` and the og-brawler wiring tests before this
// change), and private is now reachable where it was not worth fighting for
// on the pre-split, single-sibling class. Decision recorded here and in this
// item's impl notes; `DiagnosticsConventions.md` updated to match.
//
// -----------------------------------------------------------------------------
// THE TWO-THREAD RULE, AS A CLASS PROPERTY — now trivially true rather than
// merely stated, because every method left on this class is PHYSICS THREAD
// ONLY:
//
//   PHYSICS THREAD ONLY — reached only from SimulationNetSync::collectInputAll
//   / collectResimInputAll and their per-character helpers:
//     emitLocalInputRead, emitRemoteQueueRead, emitPredictionInputRead,
//     emitResimNoSlot, emitResimSentinel, emitResimLocalRead, emitResimNoStore,
//     emitResimRefRead, emitResimScheduledRead, emitRelayReadWindowIfDue
//     (which is also the sole caller of the private emitMissClassLine /
//     emitDeltaLine).
//
//   EITHER THREAD, LIFECYCLE ONLY (never concurrent with the above by
//   construction — see `unregisterSimulatable`'s own ordering comment on
//   `SimulationNetSync`): forgetOwner.
//
//   CONST, either thread: the one probe accessor — read-only, and the reason
//   `SimulationNetSync::Diagnostics` may call it from wherever a test likes.
//
// This is the SAME method-level fence `NetSyncTelemetry.h`'s banner states for
// its own (now GAME-THREAD-ONLY) half — split along the banner, not rewritten.
// ---------------------------------------------------------------------------
class InputResolutionTelemetry
{
public:
    void setLogger(std::function<void(const char*)> logger)
    {
        m_logger = std::move(logger);
    }

    // [task 59, retargeted task 79, split here at item 85] THE PROBE
    // ACCESSOR — CONST-ONLY, same contract as before this split.
    // `SimulationNetSync::Diagnostics::relayReadProbe()` delegates into this
    // rather than reading its own member directly; the og-brawler wiring
    // tests that call it are unmodified in assertion content (task 59's
    // standard, re-verified at item 85 — see this item's impl notes).
    const RelayReadProbe& relayReadProbe() const { return m_relayReadProbe; }

    // [T19, relocated task 79, split here at item 85] LIFECYCLE CLEANUP, NOT
    // DIAGNOSTICS — called once from `SimulationNetSync::unregisterSimulatable`
    // (itself unchanged: step 4 of that fixed, load-bearing ordering), ALONGSIDE
    // `NetSyncTelemetry::forgetOwner` — both halves are called from that one
    // site; see `SimulationNetSync.h` for the paired call. Without this the
    // probe's id-keyed stale-run state would grow with every character that
    // has ever existed in the session — the same unbounded-memo shape the log
    // throttles already had to fix once.
    void forgetOwner(unsigned int id)
    {
        m_relayReadProbe.forgetOwner(id);
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

    // [T19, relocated task 79, split here at item 85] PROBES 1 + 3 — the
    // per-window summary. Called once per prediction tick from
    // `SimulationNetSync::collectInputAll`; silent unless a window both
    // CLOSED and carried at least one scheduled read, so the authority (no
    // relay stores, so neither call site is ever reached) and an idle client
    // never heartbeat a Warning line.
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

private:
    // [T20, relocated task 79, MADE PRIVATE at item 85 — B-5 correction] PROBE
    // B — the miss partition for ONE call site. Silent when that call site
    // missed nothing, so a healthy window costs no line. Sole caller is
    // `emitRelayReadWindowIfDue` above, in this same class; no test or
    // production call site reached it directly on `NetSyncTelemetry` either
    // (grep-verified before this change), so private is reachable here where
    // it was carried public on the pre-split class only because the sibling
    // it lived on had no reason to narrow it.
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

    // [T20, relocated task 79, MADE PRIVATE at item 85 — B-5 correction] PROBE
    // B — the signed `probeTick - newestResident` distribution for ONE call
    // site. At depth 1 this is the richer signal: it says WHERE the receiver
    // is asking relative to what it holds, continuously, rather than in three
    // buckets. A window whose p50 sits above 0 is a receiver reading ahead of
    // its data (no depth helps); one whose p50 sits below 0 while missing is
    // reading inside a span full of holes (depth does). Sole caller is
    // `emitRelayReadWindowIfDue` above, same reasoning as
    // `emitMissClassLine`'s visibility note.
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

    // [T19] THE CLIENT-SIDE RELAY READ PROBE. Pure telemetry: nothing in the
    // resolution path reads it, and every consumer is a SIMLOG. PHYSICS
    // thread only — see the two-thread rule at the top of this file. Its
    // GAME-thread sibling (`RelayArrivalProbe`) stays on `NetSyncTelemetry`;
    // see that header and `Network/RelayReadProbe.h`'s own banner for why
    // they were ever two objects rather than one.
    RelayReadProbe m_relayReadProbe;
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
