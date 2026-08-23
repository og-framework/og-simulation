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

// NetSyncTelemetry — `SimulationNetSync`'s GAME-THREAD instrument: it owns every
// probe count and every shipped log line that the transport peer's decision and
// report structs are projected into.
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
//   * OWNERSHIP. `SimulationNetSync` owns exactly one telemetry object —
//     `m_telemetry`, this class. The PHYSICS-thread sibling
//     `InputResolutionTelemetry` is owned by `SimulationInputResolution`
//     (`m_inputResolutionTelemetry`), not by NetSync and not by this class.
//     Neither sibling holds a reference to the other.
//
//   * EVERY METHOD HERE IS GAME THREAD ONLY. Three call sites, all methods
//     of `SimulationNetSync`:
//
//       emitRelayArrival        onRelayedInputReceived    replication-dispatched
//       emitCorrectionArrival   onCorrectionReceived      replication-dispatched
//       forgetOwner             unregisterSimulatable     step 4, lifecycle
//
//     `emitCorrectionVerdictClassLine` / `emitCorrectionLandingClassLine` are
//     reached only from `emitCorrectionArrival`, and
//     `shouldLogVersionMismatchOnce` only from `emitRelayArrival`.
//
//   * THE READ SEAM IS NOT ON THIS CLASS. `SimulationNetSync::Diagnostics`
//     exposes THREE probes, const, each delegating in here:
//     `relayArrivalProbe()`, `correctionVerdictProbe()`, `correctionLandingProbe()`.
//     The fourth probe of that set, `relayReadProbe()`, is NOT on that view:
//     it is reached as `inputResolution.getDiagnostics().relayReadProbe()`.
//
//   * VOLUME ROSTER — WHAT EACH HELPER PUTS ON THE LOG IN THE STEADY STATE
//     (no rare event, no window closing). This is the contract T19 exists to
//     hold; the fences below defend the individual gates that keep it.
//
//       helper                           steady state   rare / on window close
//       emitRelayArrival                 NOTHING        Verbose when gap > 1,
//                                                       Warning per window
//       emitCorrectionArrival            1 Verbose per  Warning per class,
//                                        ARRIVING       per window
//                                        correction
//       emitCorrectionVerdictClassLine   NOTHING        1 Warning, non-empty class
//       emitCorrectionLandingClassLine   NOTHING        1 Warning, non-empty class
//       shouldLogVersionMismatchOnce     NOTHING        1 Warning per id, ever
//       forgetOwner                      NOTHING        never logs
//
//     ⇒ ONE line per ARRIVING CORRECTION is this class's entire per-event cost
//       on a steady client, and a relay arrival at the healthy depth-1 cadence
//       costs NOTHING at all.
//
//   * SEVERITY, AND WHY `Log` IS NOT AN OPTION HERE. Every per-window summary
//     in this file ships at Warning. The adapter routes on the tag prefix and
//     the host project's log configuration sets every category it can route to
//     at Warning, so a `Log`-severity line DOES NOT EXIST in a shipped build.
//     One adapter's binding: `RouteOGMessage` (`SimulationManagerUImpl.cpp`)
//     reading `Config/DefaultEngine.ini`:
//
//       [RelayProbe.*]       LogOGRelayProbe=Warning
//       [DivergenceProbe.*]  LogOGDivergenceProbe=Warning
//       [ResimProbe.*]       LogOGResimProbe=Warning
//       [RelayedInput]       unrouted, falls back to LogOG=Warning
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
// ⛔ TWO SIBLINGS, TWO OWNERS, NO LINK — `SimulationInputResolution` owns `InputResolutionTelemetry`; neither references the other, and merging them re-crosses a thread boundary. §1
//
// ⛔ AN OWNER, NOT A DIAGNOSTICS VIEW — production calls into every public method here on the game thread, so nothing on this class may be grouped behind a `getDiagnostics()`. §2
// ⛔ Nothing here is reachable from `SimulationNetSync::getDiagnostics()`. The read-seam / write-site rule is stated once in `docs/DiagnosticsConventions.md` §2.
//
// ⛔ NO REFERENCE INTO A PRODUCTION CONTAINER IS EVER HANDED IN — `emitRelayArrival` takes a report, not the arriving `RemoteInputCache<InputT>&`. §2
// ⛔ `shouldLogVersionMismatchOnce`'s latch therefore lives here, id-keyed, rather than on the store. §7
//
// ⛔ EVERY METHOD ON THIS CLASS IS GAME THREAD ONLY, so no member here is atomic and none may be made concurrent. §3
// ⛔ `forgetOwner` is the one either-thread member and is never concurrent by construction — see `SimulationNetSync::unregisterSimulatable`'s ordering. §3
// ⛔ `shouldLogVersionMismatchOnce` is public, but production reaches it only from `emitRelayArrival`; `emitCorrectionArrival` is the sole caller of the two class-line helpers.

// CorrectionArrivalDecision — what `SimulationNetSync::decideCorrectionArrival`
// returns and `emitCorrectionArrival` consumes.
//
// ⛔ FILE SCOPE, NOT NESTED IN EITHER CLASS — the pure half (`decideCorrectionArrival`) and the projection half (`emitCorrectionArrival`) both need this name. §4
//
// ⛔ `landed` IS A FIELD, NOT A MID-BLOCK `return` — `noteLanding` always runs, `noteCorrection` must NOT run on a discard. §4
// ⛔ Folding the probing into one helper would run the verdict probe on discarded corrections and silently corrupt item 41's `aboveNewest` population. §4
// ⛔ `characterClass` / `landingSite` are meaningful even when `!landed`; the landing probe reads them unconditionally.
struct CorrectionArrivalDecision
{
    PredictedCharacterClass characterClass = PredictedCharacterClass::LocallyPredicted;
    CorrectionLandingSite   landingSite    = CorrectionLandingSite::Discarded;

    // ⛔ `landed` is its own field, never re-derived from `landingSite == Discarded` — the two notions must not be able to diverge. §4
    bool landed = false;

    // ⛔ Set on BOTH paths — `emitCorrectionArrival` prints `decision.tick` BEFORE the `!landed` gate, so a discarded correction still carries the wire tick. §4
    uint32 tick = 0u;

    // ⛔ Meaningless when `!landed` — no comparison happened, so this must never be read as either an agreement or a disagreement. §4
    bool predictionWasCorrect = false;
};

class NetSyncTelemetry
{
public:
    void setLogger(std::function<void(const char*)> logger)
    {
        m_logger = std::move(logger);
    }

    // ⛔ CONST-ONLY, and there are THREE — `SimulationNetSync::Diagnostics` delegates into these rather than reading the members; no mutable counterpart, no fourth accessor here. §8
    const RelayArrivalProbe& relayArrivalProbe() const { return m_relayArrivalProbe; }
    const CorrectionVerdictProbe& correctionVerdictProbe() const { return m_correctionVerdictProbe; }
    const CorrectionLandingProbe& correctionLandingProbe() const { return m_correctionLandingProbe; }

    // ⛔ LIFECYCLE CLEANUP, NOT DIAGNOSTICS — one call, from `SimulationNetSync::unregisterSimulatable`'s fixed step 4. §1
    // ⛔ Without it `m_relayArrivalProbe`'s id-keyed map and the version-mismatch latch grow with every character the session has ever had. §8
    //
    // ⛔ `m_correctionVerdictProbe` / `m_correctionLandingProbe` are DELIBERATELY NOT forgotten here — neither keeps any per-id state, so there is nothing to erase and the class totals stay correct. §8
    void forgetOwner(unsigned int id)
    {
        m_relayArrivalProbe.forgetOwner(id);
        m_versionMismatchLogged.erase(id);
    }

    // ⚠ NO `RemoteInputCache&` PARAMETER HERE, AND DO NOT ADD ONE — the store was only ever needed for the version-mismatch latch, which is here instead, id-keyed (`m_versionMismatchLogged`). §7
    // ⚠ `RemoteInputCache::shouldLogVersionMismatchOnce` no longer exists. §7
    void emitRelayArrival(unsigned int id, const RelayedInputIngestReport& report)
    {
        // ⛔ PROBE 2 — the GAME-THREAD replication-cadence window, measured in CAPTURE TICKS; a different object from the physics-side read probe, and they must not be merged. §5
        // ⛔ `report.newestCaptureTick` is the newest tick THIS RING carried, not the newest the store holds.
        if (report.newestCaptureTickValid)
        {
            RelayArrivalWindowSummary arrival;
            std::uint32_t gapCaptureTicks = 0u;
            // ⛔ `newCaptureTicksIngested`, NOT `entriesIngested` AND NOT a hard-coded 1 — it is what makes `lostCaptureTicksX1000` a measure of loss rather than of the burst rate. §5
            // ⛔ `entriesIngested` counts re-delivered ticks as new coverage; a hard-coded 1 reported ~120 per mille on a working flush. §5
            const bool windowClosed = m_relayArrivalProbe.noteArrival(
                id,
                report.newestCaptureTick,
                static_cast<std::uint32_t>(report.newCaptureTicksIngested),
                arrival,
                &gapCaptureTicks);

            // ⛔ VERBOSE ONLY WHEN THE CADENCE HICCUPED — a gap of exactly 1 is the healthy depth-1 steady state, and logging it would make this a per-tick line. §5
            if (gapCaptureTicks > 1u)
            {
                SIMLOG(m_logger,
                    "[Verbose][RelayProbe.Arrival] id=%u newestCapture=%u "
                    "gapCaptureTicks=%u",
                    id, report.newestCaptureTick, gapCaptureTicks);
            }

            if (windowClosed)
            {
                // ⛔ PER-WINDOW SUMMARY AT WARNING. `p99` is what the `depth >= gap_p99 + margin` rule reads; the mean is DELIBERATELY ABSENT because it hides the tail that sets depth. §5
                // ⛔ WARNING, NEVER `Log` — the host project's log configuration sets this family's category at Warning, so a `Log` line does not exist in a shipped build. Table: the ORIENTATION block. §5
                // ⛔ `lostCaptureTicksX1000` IS THE R = 0 LOSS INSTRUMENT; the raw numerator and denominator ride along so a window can be re-derived rather than trusted. §5
                // ⛔ `discont=` IS PART OF THE GATE, not garnish — a window reporting `discont=` > 0 was interrupted (the threshold is `kRelayArrivalDiscontinuityTicks`) and MUST BE DISCARDED, never averaged in. §5
                // ⛔ `discontMax=` is the largest excluded gap, exact, so discarding a window never hides how bad it was.
                // ⛔ `delivered=` MAKES THIS LINE SELF-CHECKING — `lost + delivered == expected` must hold on every window, or the delivered count is not being plumbed. §5
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
            // ⛔ ONCE per component per session — an incompatible peer re-replicates its ring forever, so an ungated line fires on every replication. §7
            SIMLOG(m_logger,
                "[Warning][RelayedInput] DROP wire-version mismatch id=%u onWire=%u expected=%u",
                id,
                static_cast<unsigned int>(report.versionOnWire),
                static_cast<unsigned int>(relayedInputRing::kWireFormatVersion));
        }
    }

    // ⛔ THE LANDING PROBE FIRES UNCONDITIONALLY — a discard IS an observation — and the verdict probe only when `decision.landed`. §6
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

        // ⛔ A discard is NOT counted here: no comparison happened, so counting it would put a denominator under a verdict that was never reached. §6
        // ⛔ THE LANDING PROBE DELIBERATELY SITS ON THE OTHER SIDE OF THIS `return` — its `discarded` bucket is where the two probes' sample sets must differ, and moving this gate up empties it. §6
        if (!decision.landed)
            return;

        // ⛔ VERBOSE ON EVERY LANDED CORRECTION, not on disagreements only — a disagreement-only line cannot distinguish "predicted correctly" from "no correction arrived". §6
        // ⛔ It adds no new volume CLASS: this site already logs twice per correction. §6
        SIMLOG(m_logger,
            "[Verbose][DivergenceProbe.Correction] id=%u tick=%u class=%s correct=%u",
            id, decision.tick,
            predictedCharacterClassName(decision.characterClass),
            decision.predictionWasCorrect ? 1u : 0u);

        CorrectionVerdictWindowSummary window;
        if (!m_correctionVerdictProbe.noteCorrection(
                decision.characterClass, decision.predictionWasCorrect, window))
            return;

        // ⛔ ONE LINE PER CLASS, NEVER ONE POOLED LINE — only the remote half can move with the relay delay floor, and pooling dilutes exactly that signal. §6
        // ⛔ A class with no corrections in the window is SKIPPED, never printed as `rate=0`, which would read as a perfect record instead of as no observation. §6
        emitCorrectionVerdictClassLine(
            PredictedCharacterClass::LocallyPredicted, window.local, window.samples);
        emitCorrectionVerdictClassLine(
            PredictedCharacterClass::RemoteProxy, window.remote, window.samples);
    }

    // ⛔ ONE per-window class block; called twice, once per class, from `emitCorrectionArrival`, and ONLY when a window closed. §6
    // ⛔ SILENT ON AN EMPTY CLASS — `corrections == 0` is no observation, and `disagreed=0 ratePerMille=0` would assert a perfect record instead. §6
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

    // ⛔ ONE per-window class block; called twice, once per class, from `emitCorrectionArrival`, and ONLY when a window closed. §6
    // ⛔ SILENT ON AN EMPTY CLASS, same rule and same reason as the verdict line. §6
    // ⛔ THIS LINE AND `[ResimProbe.Gate]` CANNOT BE MERGED — different threads, one probe per thread, no sharing. `atFrontier`, `behind` and `discarded` are the three populations it keeps apart. §6
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

    // ⛔ TRUE EXACTLY ONCE PER ID PER SESSION — an incompatible peer re-replicates its ring forever. Forgotten in `forgetOwner`, same unbounded-memo reason as `m_relayArrivalProbe`. §7
    // ⛔ PUBLIC ON PURPOSE — `NetSyncTelemetryTest.cpp` exercises this latch directly, not only through `emitRelayArrival`'s no-logger path. §7
    bool shouldLogVersionMismatchOnce(unsigned int id)
    {
        return m_versionMismatchLogged.try_emplace(id, true).second;
    }

private:
    std::function<void(const char*)> m_logger;

    // ⛔ GAME THREAD ONLY, and pure telemetry — nothing in the resolution path reads it, and every consumer is a SIMLOG. §8
    // ⛔ Its PHYSICS-thread sibling `RelayReadProbe` is on `InputResolutionTelemetry` — two objects because two threads. §3
    RelayArrivalProbe m_relayArrivalProbe; // GAME thread only.

    // ⛔ ONE object because ONE feeder, the GT correction callback; it carries no per-id state, which is why `forgetOwner` forgets `m_relayArrivalProbe` and not this one. §8
    CorrectionVerdictProbe m_correctionVerdictProbe;

    // ⛔ A SECOND OBJECT ON THE SAME SITE, DELIBERATELY — a DISCARDED correction is a non-event for the verdict and a first-class observation for the landing site, so merging forces one to adopt the other's sample set. §8
    // ⛔ Its PHYSICS-thread sibling is `ResimGateProbe`, on `SimulationManager` — never shared, never atomic. §8
    CorrectionLandingProbe m_correctionLandingProbe;

    // See `shouldLogVersionMismatchOnce`.
    std::unordered_map<unsigned int, bool> m_versionMismatchLogged;
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
