#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <limits>
#include <optional>

#include "OGAssert.h"
#include "OGSimulation/Network/ConnectionTierTable.h"   // clampRelayDelayFloorTicks (T11)
#include "OGSimulation/Network/CorrectionRotation.h"    // correctionRotation::clampK (T39)
#include "OGSimulation/PCTimeManagement/ServerTickClock.h"
#include "OGSimulation/PCTimeManagement/NetworkTimeEstimator.h"
#include "OGSimulation/PCTimeManagement/ClientPredictionClock.h"
#include "OGSimulation/ResimGatePolicy.h"                // resimGate:: (item 45)
#include "OGSimulation/SimulationLog.h"
// ⛔ Concept declarations ONLY — the peers stay duck-typed template parameters, and
// `NetSyncT`'s two are siblings DEFINED IN THIS FILE, not in `SimulationNetSync.h`. §2
#include "OGSimulation/SimulationIntegrationExecutor.h"
#include "OGSimulation/SimulationReconciliation.h"
#include "OGSimulation/SimulationTimeContext.h"
// ⚠ THE STATED REASON FOR THIS INCLUDE IS STALE (2026-08-22):
// `preparePredictionSimulationStep` moved to `SimulationStepSequencing.h` and is
// duck-typed on both peers there — it does not name `SimulationInputResolution`.
// ⛔ Whether this include is still REQUIRED is
// unverified — do not remove it on the strength of this comment. §1
#include "OGSimulation/SimulationInputResolution.h"
// `preparePredictionSimulationStep` — the collect/allocate sequencing facade, and
// the one place its home is stated correctly: its own header, not either peer's. §2
#include "OGSimulation/SimulationStepSequencing.h"
// ⛔ PHYSICS-THREAD HALF ONLY. The game-thread half `CorrectionLandingProbe` is a
// member of `NetSyncTelemetry` — not of `SimulationNetSync` itself, and not here. §5
#include "OGSimulation/ResimGateProbe.h"
#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness across all build configs. OGSim-core
// convention. ⛔ CANONICAL STATEMENT: every other OGSim-core pragma site points here. §3
//
// ⛔ `OGSIM_OPTIMIZE_OFF`/`_ON`, NOT A RAW PRAGMA — defined once in `CompilerControl.h`;
// `OGSIM_FORCE_OPTIMIZED=1` (or Test/Shipping) expands both to nothing everywhere. §3
//
// ⛔ THE MEASUREMENT RULE: no gate-family cost number is quotable unless its record NAMES
// the optimize setting. §3
OGSIM_OPTIMIZE_OFF

// SimulationUpdateInfo — passed from the adapter's physics async callback into onGameSimulation.
class SimulationUpdateInfo
{
public:
    SimulationUpdateInfo() = delete;

    SimulationUpdateInfo(bool isResimulation, bool isFirstResimulationStep)
        : m_isResimulation(isResimulation)
        , m_isFirstResimulationStep(isFirstResimulationStep)
    {}

    bool isResimulation()          const { return m_isResimulation; }
    bool isFirstResimulationStep() const { return m_isFirstResimulationStep; }

private:
    bool m_isResimulation = false;
    bool m_isFirstResimulationStep = false;
};

// `SimulationManager`-facing splits of `SimulationNetSyncConcept`
// (`SimulationNetSync.h`).
//
// ⛔ PRESENCE-ONLY, AND NECESSARILY SO — the manager's parameters carry no `SimulatableTs` pack. §2
//
// ⛔ `SimulationNetSyncTickConcept`/`SimulationNetSyncPublishConcept` STAY HERE, not
// beside their peer: moving them is a `SimulationNetSync.h` edit with an include cycle. §2

// ⛔ NetSyncT's tick surface is ONE member, published BEFORE `onGameSimulationAuthority`'s
// own `collectInputAll`. §1
template <typename T>
concept SimulationNetSyncTickConcept = requires(
    T& t, uint32_t tick, int32_t rollbackWindow)
{
    { t.setAuthorityGuardContext(tick, rollbackWindow) };
};

template <typename T>
concept SimulationNetSyncPublishConcept = requires(
    T& t, const SimulationTimeStep& step, uint32_t tick, int32_t correctionRotationK)
{
    { t.sendCorrectionAll(step, correctionRotationK) };
    { t.sendLocalInputToAuthorityAll(tick, tick) };
};

// ⛔ The resolution tick group, presence-only for the same reason; `wipeAllForResync` is in it. §2
//
// ⛔ `wipeAllForResync` IS SHARED BY THREE PEER CONCEPTS — a wipe-only clause
// DISTINGUISHES NOTHING. Distinguishing: `collectInputAll`/`collectResimInputAll`
// (resolution), `postPredictionAll`/`consumeResimAnchorsAll`/`allocateFrontierSlotsAll`
// (reconciliation), `sendCorrectionAll` (netsync). §2
template <typename T>
concept SimulationInputResolutionTickConcept = requires(
    T& t, const SimulationTimeStep& step, uint32_t tick)
{
    { t.collectInputAll(step) };
    { t.collectResimInputAll(tick) };
    { t.wipeAllForResync(tick) };
};

// SimulationManager<IntegrationExecT, NetSyncT, InputResolutionT,
//                   ReconciliationT, SystemsExecT, StorageT, StaticDataT>
//
// Orchestrates the simulation loop. Holds references to five peers and to the
// externally-owned storage + static data; owns the `TimeConfig`, the three clocks
// and the resim-gate probe.
//
// ---------------------------------------------------------------------------
// ORIENTATION — WHAT THIS CLASS OWNS, WHICH ROLE ENGAGES WHAT, ON WHICH THREAD.
//
// Read this first. Every fence in this file states one invariant at the line it
// guards; none of them restates this map, and this map states no invariant.
//
//   * WHAT IT HOLDS. References to five peers it does NOT own — integration
//     executor, net sync, input resolution, reconciliation, systems executor —
//     plus references to the externally-owned storage and static data. It OWNS
//     only `m_timeConfig`, the three clocks, and `m_resimGateProbe`.
//
//   * ⛔ THE THREE CLOCKS ARE ROLE-EXCLUSIVE, AND ASKING FOR THE WRONG ONE IS
//     `std::terminate()` — an unrecoverable process abort, not an assert. The
//     ctor engages them on `shouldRunPrediction`, and never both ways:
//
//       role                engaged                terminates on
//       authority           m_serverClock          getClientClock,
//                                                  getNetworkEstimator
//       predicting client   m_networkEstimator     getServerClock
//                           m_clientClock
//
//     All six accessors are bare `has_value()` checks that abort. BRANCH ON
//     `runsPrediction()` BEFORE REACHING FOR A CLOCK — the viz tick/dt source
//     already had to be fixed for exactly this.
//
//   * THREADS. Everything in the dispatch group, and everything it calls, runs
//     on the PHYSICS thread from the adapter's physics callbacks. The three
//     config setters and `onPostSimulationGameThread` run on the GAME thread.
//     `m_resimGateProbe` is physics-thread-only, which is why it holds no
//     atomics.
//     ⭐ ONE ADAPTER'S BINDING FOR THE CALLBACK COLUMN BELOW: it names the
//     physics-callback ROLE, never an engine type. One adapter binds those
//     roles on `FSimulationManagerAsyncCallback` (`OGSimulationUnreal`,
//     `SimulationManagerUImpl.cpp`), where that one adapter's names are
//     `OnPreSimulate_Internal` (pre-simulate step), `OnPostSolve_Internal`
//     (post-solve step),
//     `TriggerRewindIfNeeded_Internal` (rewind request) and
//     `FirstPreResimStep_Internal` (first resim step). Another adapter
//     substitutes its own.
//
//       adapter callback             this class             on the authority
//       pre-simulate step            onGameSimulation       Authority branch
//       post-solve step              onPostGameSimulation   returns early
//       rewind request               onCheckIsSimilar       never reached
//       first resim step             prepareResimulation    never reached
//
//   * ⛔ THE `TimeConfig` RE-READ RULE, AND ITS SCOPE IS ALL THREE VALUES. This
//     class is the sole reader-and-hander-down of `TimeConfig` into the peers,
//     so the rule holds here or nowhere: the depth fold off `resimTriggerPolicy`,
//     `correctionRotationK` and `redundancyDepthTicks` are ALL read live on every
//     call and passed down as arguments. Caching any of them in a peer makes an
//     ini-driven setting silently ineffective. §4
//
//   * ⛔ THE RESIM TRIGGER POLICY IS **TWO SEPARATE FACTS**. They differ, and
//     conflating them is a defect this tree has repeated. Both read 2026-08-22:
//
//       CODE DEFAULT    `FrontierExact`      anchor: `TimeConfig.h`, at the
//                                            declaration of
//                                            `TimeConfig::resimTriggerPolicy`
//       SHIPPED CONFIG  `OnDisagreement`     anchor: the host project's netcode
//                                            config, at key `ResimTriggerPolicy`
//                                            under `[OGNetcode]`, uncommented —
//                                            one adapter's binding for that file
//                                            is `Config/DefaultEngine.ini`
//
//     A build with no ini override runs the CODE DEFAULT; every run of this
//     project runs the SHIPPED CONFIG. ⚠ Two consequences follow from the shipped
//     half and are routinely stated backwards:
//       - `resimGate::policyEnforcesDepthCeiling` is true ONLY for
//         `OnDisagreement`, so the depth ceiling is ARMED and `deepSkips` is NOT
//         structurally 0;
//       - `[ResimProbe.Gate]`'s `requested` NO LONGER tracks the landing probe's
//         `atFrontier` — that identity held under `FrontierExact` only.
//     ⛔ Do not restate either value elsewhere in this file: name which of the two
//     you mean and let this block carry the value. §4
//
//   * The resim cycle — its six phases, their order and their thread — is stated
//     once, in `SimulationReconciliation.h`'s own orientation block. Not
//     re-derived here. The frontier-pair contract likewise lives at
//     `SimulationReconciliation::allocateFrontierSlotsAll`.
// ---------------------------------------------------------------------------
//
// What runs on the authority vs under prediction, and which differences are role gates
// rather than separate code paths: `docs/Perspective-AuthorityVsPrediction.md` §4 (the
// three step functions) and §3 (the four kinds of role gate) — not re-derived here.
//
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
//
// Relocation history, retired rationale and archived measurement records:
// `docs/SimulationManager-rationale.md`.

template <typename IntegrationExecT, typename NetSyncT, typename InputResolutionT,
          typename ReconciliationT, typename SystemsExecT,
          typename StorageT, typename StaticDataT>
class SimulationManager
{
public:
    using LoggerFn = std::function<void(const char*)>;

    // ⛔ `ManagerType::Params` EXISTS SO THE COMPOSITION ROOT CANNOT SILENTLY transpose
    // ADJACENT DUCK-TYPED PEER REFS — such a swap COMPILES and misbehaves only at runtime. §8
    struct Params
    {
        IntegrationExecT&    integrationLayer;
        NetSyncT&            netSync;
        InputResolutionT&    inputResolution;
        ReconciliationT&     reconciliation;
        SystemsExecT&        systemsExec;
        StorageT&            storage;
        const StaticDataT&   staticData;
        LoggerFn             logger = nullptr;
    };

    SimulationManager(
        bool          shouldRunPrediction,
        double        tickFrequency,
        const Params& params)
        : m_runsPrediction(shouldRunPrediction)
        , m_integrationLayer(params.integrationLayer)
        , m_netSync(params.netSync)
        , m_inputResolution(params.inputResolution)
        , m_reconciliation(params.reconciliation)
        , m_systemsExec(params.systemsExec)
        , m_storage(params.storage)
        , m_staticData(params.staticData)
        , m_logger(params.logger)
    {
        m_timeConfig.tickFrequency = 1.0 / tickFrequency;

        // ⛔ THE CTOR IS DELIBERATELY UNCONSTRAINED — a clause built from `wipeAllForResync` alone
        // is false protection. `SimulationInputResolutionTickConcept` /
        // `SimulationReconciliationConcept` constrain the differing members instead. §2
        if (shouldRunPrediction)
        {
            m_networkEstimator.emplace(m_timeConfig, params.logger);
            m_clientClock.emplace(m_timeConfig, *m_networkEstimator, params.logger);
            m_clientClock->registerResyncCallback(
                [this](unsigned int newPredictionTick)
                {
                    SIMLOG(m_logger, "[TimeResync.Wipe] newPredictionTick=%u", newPredictionTick);
                    // ⛔ D4 ABSENCE FENCE — THE SLOT-PROVENANCE LOG'S SECOND CALL SITE IS NOT HERE; it is in
                    // `wipeAllForResync`, which reads the same caches in one pass. Here it would add a method
                    // to four mock reconciliation types. `logSlotProvenanceFor`. §4
                    m_reconciliation.wipeAllForResync(newPredictionTick);
                    m_inputResolution.wipeAllForResync(newPredictionTick);
                });
        }
        else
        {
            // ⛔ ANTI-SYMMETRY: `tickFrequency` is a dt in SECONDS, fed straight to `ServerTickClock`. §10
            m_serverClock.emplace(static_cast<float>(tickFrequency), params.logger);
        }
    }

    void setLogger(LoggerFn logger) { m_logger = std::move(logger); }

    bool runsPrediction() const { return m_runsPrediction; }

    // Read-only view of the manager's owned TimeConfig.
    //
    // ⛔ ONE CONFIG, ONE TIER POLICY, BY CONSTRUCTION — `ConnectionTierTable` and
    // `ServerInputDelayQueue` hold `const TimeConfig&` bound to THIS member, stable for life. §4
    const TimeConfig& getTimeConfig() const { return m_timeConfig; }

    // ⛔ THE one writable location for the session relay delay floor. §4
    //
    // ⛔ NARROW SETTER, NOT A MUTABLE `editTimeConfig()` — that re-opens every start-up constant. §4
    //
    // ⛔ CLAMPED HERE TOO — `clampRelayDelayFloorTicks` is idempotent and also runs at both intakes. §4
    //
    // ⛔ GAME THREAD ONLY; it crosses at the one `setClientEffectiveInputDelayTicks` atomic,
    // and on a client is written from the floor's replication callback. §4
    void setRelayDelayFloorTicks(int32_t requestedFloorTicks)
    {
        m_timeConfig.relayDelayFloorTicks =
            clampRelayDelayFloorTicks(requestedFloorTicks, m_timeConfig);
    }

    // ⛔ D4 ABSENCE FENCE — NO RELAY-RING-DEPTH SETTER HERE, and there must not be one again;
    // its old identifier is on record in RN-13. The stage capacity is the compile-time
    // `relayedInputRing::kMaxDepth`. §4

    // ⛔ THE one writable location for the correction-state rotation width — the
    // `[OGNetcode] CorrectionRotationK` ini door. §4
    //
    // ⛔ SERVER-ONLY AND NOT REPLICATED — only the authority runs `SimulationNetSync::sendCorrectionAll`. §4
    //
    // ⛔ ONE-SHOT AT COMPOSITION, AND IT MUST NOT ACQUIRE A `cvar` — probe readings must stay attributable. §4
    //
    // ⛔ CLAMPED HERE TOO, `correctionRotation::clampK`. 0 and negatives clamp UP to 1: a K
    // of 0 is a channel that never publishes — a permanent desync, not "off". §4
    void setCorrectionRotationK(int32_t requestedK)
    {
        m_timeConfig.correctionRotationK = correctionRotation::clampK(requestedK);
    }

    // ⛔ THE one writable location for the session resim-gate trigger policy — the
    // `[OGNetcode] ResimTriggerPolicy` ini door. §4
    //
    // ⛔ ONE-SHOT AT COMPOSITION, AND HERE THAT IS LOAD-BEARING — the value is read on the GAME
    // thread at the landing site with no synchronization, safe only because nothing writes it
    // after composition. A `cvar` would race it; there must not be one. §4
    //
    // ⛔ SECOND EFFECT, SO THIS STAYS THE ONLY DOOR — it fans out through
    // `SimulationReconciliation::setResimTriggerPolicy` to every `StateCorrectionCache`;
    // writing `TimeConfig` directly leaves the caches on the CODE DEFAULT. §4
    void setResimTriggerPolicy(TimeConfig::ResimTriggerPolicy policy)
        requires SimulationReconciliationConcept<ReconciliationT>
    {
        m_timeConfig.resimTriggerPolicy = policy;
        m_reconciliation.setResimTriggerPolicy(policy);
    }

    // ⛔ D4 ABSENCE FENCE — NO `setResimCooldownTicks`, AND THAT IS A RULING: a rate ceiling
    // defers acting on a correction already known to disagree. The throttle is structural —
    // `resimGate::policyEnforcesDepthCeiling`. §5

    ServerTickClock& editServerClock()
    {
        if (!m_serverClock.has_value()) { std::terminate(); }
        return *m_serverClock;
    }

    const ServerTickClock& getServerClock() const
    {
        if (!m_serverClock.has_value()) { std::terminate(); }
        return *m_serverClock;
    }

    NetworkTimeEstimator& editNetworkEstimator()
    {
        if (!m_networkEstimator.has_value()) { std::terminate(); }
        return *m_networkEstimator;
    }

    const NetworkTimeEstimator& getNetworkEstimator() const
    {
        if (!m_networkEstimator.has_value()) { std::terminate(); }
        return *m_networkEstimator;
    }

    ClientPredictionClock& editClientClock()
    {
        if (!m_clientClock.has_value()) { std::terminate(); }
        return *m_clientClock;
    }

    const ClientPredictionClock& getClientClock() const
    {
        if (!m_clientClock.has_value()) { std::terminate(); }
        return *m_clientClock;
    }

    // Simulation dispatch — called from the adapter's physics callback

    void onGameSimulation(const SimulationUpdateInfo& updateInfo)
    {
        if (m_runsPrediction)
        {
            if (updateInfo.isResimulation())
                onGameSimulationResimulation();
            else
                onGameSimulationPrediction();
        }
        else
        {
            onGameSimulationAuthority();
        }
    }

    // Runs from the post-solve step: captures post-solve state, detects the catch-up edge.
    // ⛔ Peer members this requires-clause covers: `captureBodyStatesAll`,
    // `postPredictionAll`/`postResimulationAll`/`applyResimAll`/`consumeResimAnchorsAll`. §2
    void onPostGameSimulation(const SimulationUpdateInfo& updateInfo)
        requires SimulationIntegrationExecutorConcept<IntegrationExecT> &&
                 SimulationReconciliationConcept<ReconciliationT>
    {
        if (!m_lastStep.has_value())
            return;

        m_integrationLayer.captureBodyStatesAll();

        if (m_runsPrediction)
        {
            if (updateInfo.isResimulation())
            {
                // ⛔ ALL THREE COUNTS RIDE THIS ONE SWEEP — `replayOverruns` plus the fresh/stale split; a
                // second sweep could describe different replay ticks. They ride the EXISTING
                // `[ResimProbe.Apply]` line; new `[Resim.`/`[ResimCheck.` lines are forbidden. §6
                //
                // ⛔ One `ResimSweepDiagnostics` BY VALUE, not a return plus two out-pointers; `auto` keeps it duck-typed. §6
                const auto resimDiagnostics = m_reconciliation.postResimulationAll(*m_lastStep);
                m_resimGateProbe.noteReplayOverruns(resimDiagnostics.discards);
                m_resimGateProbe.noteCorrectionProtections(
                    resimDiagnostics.freshProtections, resimDiagnostics.staleProtections);

                // Apply-resim edge: the physics engine still resimulating, clock already caught up
                // (`advanceResimulation`). ⛔ Unique to the last resim sub-step's `PostSolve`. §7
                const bool chaosIsResim = updateInfo.isResimulation();
                const bool clockIsResim = m_clientClock->isResimulating();
                if (chaosIsResim && !clockIsResim)
                {
                    SIMLOG(m_logger, "[Resim.Finish] predictionTick=%u",
                        m_clientClock->getPredictionTick());
                    // ⛔ `finishes` EQUALS the `[Resim.Finish]` occurrence count BY CONSTRUCTION. §6
                    m_resimGateProbe.noteFinish();
                    m_clientClock->finishResimulation();
                    m_reconciliation.applyResimAll();
                    // ⛔ THE CONSUME EDGE IS A PER-CHARACTER CAS — a correction landed mid-replay raises the
                    // anchor past the prepared value, so the CAS fails and the anchor SURVIVES to re-trigger.
                    // `StateCorrectionCache::consumeResimAnchor`. §5
                    //
                    // ⛔ ORDER: AFTER `applyResimAll`, and on THIS edge — ~20 % of prepares never reach here. §5
                    //
                    // ⛔ THE RETURN IS NOT DISCARDED — it feeds `m_resimGateProbe.noteSurvivingAnchors`.
                    // ⚠ Its old blocker ("reads 0") was retired by the SHIPPED CONFIG, not by item 46, which
                    // flips the CODE DEFAULT and is still open. §4
                    m_resimGateProbe.noteSurvivingAnchors(m_reconciliation.consumeResimAnchorsAll());
                    // ⛔ ONE Verbose line per character, ONCE PER COMPLETED RESIM, and on THIS edge. §6
                    //
                    // ⛔ VERBOSE-ONLY AND READ BY NO DECISION — hence `logSlotProvenanceAll` sits behind
                    // `getDiagnostics()` (`docs/DiagnosticsConventions.md` §2), with `SlotStateProvenance.h`
                    // carrying the rest. At the shipped `LogOGResimProbe=Warning` these lines do not exist. §6
                    m_reconciliation.getDiagnostics().logSlotProvenanceAll();
                }
            }
            else
            {
                m_reconciliation.postPredictionAll(*m_lastStep);
            }
        }
    }

    // ⛔ `checkDivergenceAll` — the one peer member this requires-clause covers. §2
    unsigned int onCheckIsSimilar()
        requires SimulationReconciliationConcept<ReconciliationT>
    {
        // ⛔ THE DEPTH POLICY IS READ LIVE FROM `TimeConfig` AND HANDED DOWN, never cached in
        // reconciliation — caching makes an ini-driven setting silently ineffective. Under the
        // CODE DEFAULT `policyEnforcesDepthCeiling` answers 0; under the SHIPPED CONFIG it is live.
        // ⛔ The caller short-circuits on `!runsPrediction()`. §4
        const uint32_t maxAnchorDepthTicks =
            resimGate::policyEnforcesDepthCeiling(m_timeConfig.resimTriggerPolicy)
                ? static_cast<uint32_t>(m_timeConfig.rollbackWindowTicks)
                : 0u;

        unsigned int diagnosticDeepAnchorSkips = 0u;
        const unsigned int correctionTick =
            m_reconciliation.checkDivergenceAll(maxAnchorDepthTicks, &diagnosticDeepAnchorSkips);

        // ⛔ NO RATE CEILING HERE, AND THAT IS A RULING — a `resimCooldownTicks` branch stood on
        // this line and was removed. The throttle is STRUCTURAL: at most one resim in flight and
        // one pending, mid-replay landings coalescing via CAS-max. `policyEnforcesDepthCeiling`. §5
        const bool requestingResim = correctionTick != 0u;

        // ⛔ THIS COUNTS INSTEAD OF LOGGING — `[ResimCheck.IsSimilar]` emits at `Log` under
        // `LogOGSimTick`, which ships at `Warning`, so it has ZERO occurrences in every archived
        // log. Delete the counter, read the line instead, and you read nothing. §6
        //
        // ⛔ `diagnosticDeepAnchorSkips` RIDES THIS CALL, as one field on `[ResimProbe.Gate]`.
        // ⚠ Structural 0 only under the CODE DEFAULT; under the SHIPPED CONFIG the ceiling is
        // armed and a nonzero is expected — read it against `requested`. The count is diagnostic,
        // `checkDivergenceAll`'s skip is production (`docs/DiagnosticsConventions.md` §4). §4
        m_resimGateProbe.noteDeepAnchorSkips(diagnosticDeepAnchorSkips);
        noteDivergenceCheck(requestingResim);

        if (!requestingResim)
        {
            SIMLOG(m_logger, "[ResimCheck.IsSimilar] correctionTick=0 result=noResim");
            return std::numeric_limits<unsigned int>::max();
        }

        SIMLOG(m_logger, "[ResimCheck.Divergence] correctionTick=%u", correctionTick);
        return correctionTick;
    }

    // `chaosStep` is the raw physics-engine step; `simTick` is the tick to resim
    // from. The parameter name spells one adapter's engine: read "chaos step" as
    // PHYSICS STEP here and at every call site.
    // ⛔ Peer members this requires-clause covers: `prepareResimAll`, `firstResimStepAll`. §2
    void prepareResimulation(int32_t chaosStep, uint32_t simTick)
        requires SimulationReconciliationConcept<ReconciliationT> &&
                 SimulationIntegrationExecutorConcept<IntegrationExecT>
    {
        if (!m_runsPrediction)
        {
            OG_CHECK(false, "prepareResimulation called on authority — not expected");
            return;
        }
        SIMLOG(m_logger, "[Resim.Prepare] chaosStep=%d simTick=%u", chaosStep, simTick);
        // ⛔ `prepares` EQUALS the `[Resim.Prepare]` occurrence count by construction. §6
        m_resimGateProbe.notePrepare(simTick);
        m_clientClock->startResimulation(simTick);
        m_reconciliation.prepareResimAll(simTick);
        m_integrationLayer.firstResimStepAll(chaosStep);
    }

    // THE RESIM-GATE PROBE — PHYSICS THREAD.
    //
    // ⛔ PUBLIC BECAUSE TWO OF ITS SIX INSTRUMENTS ARE FED BY THE ADAPTER'S RESIM-REQUEST
    // AND FIRST-RESIM-STEP CALLBACKS — both on the SAME physics thread as this class: hence
    // no atomics. (One adapter's binding: `FSimulationManagerAsyncCallback::
    // TriggerRewindIfNeeded_Internal` / `::FirstPreResimStep_Internal`.) §5
    //
    // ⛔ NOT A GENERAL MUTABLE HANDLE — a counter sink read by no gate, clock, cache or
    // integrator; a game-thread call corrupts a window (`ResimGateProbe.h`'s two-object rule).
    // The adapter uses two narrow passthroughs, as `requestInputDelayIncreaseStall` does. §5
    //
    // ⛔ `editResimGateProbe()` STAYS EXACTLY HERE — a production WRITE handle is not a
    // diagnostic read seam (`docs/DiagnosticsConventions.md` §2). §5
    ResimGateProbe&       editResimGateProbe()       { return m_resimGateProbe; }

    // THE RESIM-GATE PROBE'S DIAGNOSTIC VIEW — grouped per
    // `docs/DiagnosticsConventions.md` §2.
    //
    // ⛔ ONLY THIS READ-ONLY ACCESSOR IS A DIAGNOSTIC SEAM — `m_resimGateProbe`, every `note*`
    // feeder including the private `noteDivergenceCheck`, and `editResimGateProbe` are
    // PRODUCTION and untouched. §5
    //
    // ⛔ ZERO PRODUCTION CALLERS, AND KEPT ANYWAY — the only mechanism that can prove this
    // probe family is wired. Its one caller is the LLT case
    // `ResimGate.Policy.TheResimGateProbeAccessorObservesTheShippedFeed`
    // (`ResimGatePolicyTest.cpp`), and ⛔ THAT NAME IS THE ONLY GREP HANDLE TO THIS RULING:
    // nothing here names it, and there is no production caller to grep toward. §5
    //
    // Nested rather than free: same private access as any member, no friend declaration. §5
    class Diagnostics
    {
    public:
        explicit Diagnostics(const SimulationManager& manager) : m_manager(manager) {}

        const ResimGateProbe& resimGateProbe() const { return m_manager.m_resimGateProbe; }

    private:
        const SimulationManager& m_manager;
    };

    Diagnostics getDiagnostics() const { return Diagnostics(*this); }

    // Tick of the most recently integrated step — whichever step `integrateAll` just ran.
    // ⛔ Returns 0 before the first integrate; 0 is the reserved pre-sim tick. §7
    uint32_t currentIntegratedTick() const
    {
        return m_lastStep.has_value() ? m_lastStep->getTick() : 0u;
    }

    // ⛔ Peer members this requires-clause covers: `sendCorrectionAll` +
    // `sendLocalInputToAuthorityAll` — `SimulationNetSyncPublishConcept`. §2
    void onPostSimulationGameThread()
        requires SimulationNetSyncPublishConcept<NetSyncT>
    {
        const SimulationTimeStep step = currentStep();
        // ⛔ BOTH VALUES ARE READ LIVE FROM `TimeConfig` AND HANDED DOWN — the rotation width
        // `correctionRotationK`, and `redundancyDepthTicks` (3 at 60 Hz, 5 at 100 Hz). Caching
        // either in a peer makes an ini-driven setting silently ineffective. §4
        m_netSync.sendCorrectionAll(step, m_timeConfig.correctionRotationK);
        m_netSync.sendLocalInputToAuthorityAll(
            step.getTick(), static_cast<uint32>(m_timeConfig.redundancyDepthTicks));
    }

    // Character-lifecycle notifications — forwarded to the systems executor for its own
    // per-character bookkeeping, out of band from the integrate loop. Storage + static data are
    // supplied so a hook can read the just-(un)registered character.

    void notifyCharacterRegistered(unsigned int id)
    {
        m_systemsExec.notifyCharacterRegistered(id, m_storage, m_staticData);
    }

    void notifyCharacterUnregistered(unsigned int id)
    {
        m_systemsExec.notifyCharacterUnregistered(id, m_storage, m_staticData);
    }

private:
    // THE PER-WINDOW FLUSH — three Warning lines on `LogOGResimProbe`, only when a window
    // closed.
    //
    // ⛔ `Warning`, NOT `Log`, AND ON ITS OWN CATEGORY — four instruments have already been
    // lost to verbosity, and a line invisible at shipped verbosity would repeat that defect on
    // the instrument built to end it. §6
    //
    // ⛔ AND THE TAG DELIBERATELY DOES NOT BEGIN `[Resim.` OR `[ResimCheck.` — `[Resim.` routes
    // to `LogOGSim`, whose knob these lines must not share, and `[ResimCheck.` SPLITS across
    // `LogOGSim` and `LogOGSimTick`. `[ResimProbe.` has its own knob. §6
    //
    // ⛔ THREE LINES, NOT ONE, split by PIPELINE STAGE: `.Gate` = did we ask, the middle
    // one = did the physics engine accept, `.Apply` = did it land. That middle tag spells
    // one adapter's engine — `[ResimProbe.Chaos]` — and is a FROZEN spelling archived greps
    // key on, not a claim about this layer. With the game thread's two
    // `[ResimProbe.Landing]` lines that is 5 Warning lines per window, inside a budget of
    // 6. §6
    void noteDivergenceCheck(bool requestedResim)
    {
        ResimGateWindowSummary window;
        if (!m_resimGateProbe.noteCheck(requestedResim, window))
            return;

        // ⛔ FIELDS ARE APPENDED, NEVER INSERTED — `deepSkips` and `survivingAnchors` went on the
        // end so archived greps keying on `checks=`/`declined=`/`requested=` still match.
        // ⚠ `deepSkips` counts CHARACTER-FRAMES whose anchor sat deeper than `rollbackWindowTicks`
        // below its frontier and was SKIPPED, not clamped. §6
        //
        // ⛔ `survivingAnchors` comes from the `[Resim.Finish]` apply edge, NOT from this sweep —
        // `ResimGateWindowSummary::survivingAnchors`. §6
        SIMLOG(m_logger,
            "[Warning][ResimProbe.Gate] checks=%u declined=%u requested=%u requestedPerMille=%u "
            "deepSkips=%u survivingAnchors=%u",
            window.checks, window.declined, window.requested, window.requestRatePerMille,
            window.deepAnchorExclusions, window.survivingAnchors);

        // ⛔ `refused` is trustworthy because a refusal CONSUMES NOTHING — `needsResimulation()`
        // stays true and the request repeats; `repeat` is that signature.
        // ⛔ `clamped` reads a CONSTANT 0 live — a shallow clamp is structurally impossible, so a
        // nonzero is an engine-behaviour-change ALARM. `ResimGateWindowSummary::clampedGrants`. §6
        SIMLOG(m_logger,
            "[Warning][ResimProbe.Chaos] requests=%u grants=%u refused=%u refusedPerMille=%u "
            "repeat=%u clamped=%u depthMin=%u depthMax=%u",
            window.requests, window.grants, window.refusedFrames, window.refusedRatePerMille,
            window.repeatRequests, window.clampedGrants,
            window.minRequestDepth, window.maxRequestDepth);

        // ⛔ Prefer `stuck` to `abandoned` on any single window — `abandoned` has a ±1 boundary term. §6
        SIMLOG(m_logger,
            "[Warning][ResimProbe.Apply] prepares=%u finishes=%u abandoned=%u stuck=%u "
            "replayTicks=%u replayOverruns=%u freshProtects=%u staleProtects=%u",
            window.prepares, window.finishes, window.abandoned, window.stuckResimFrames,
            window.replayTicks, window.replayOverruns,
            // ⛔ `freshProtects` is the live defect rate — near-0 under the CODE DEFAULT, a real rate
            // under the SHIPPED CONFIG. ⛔ `staleProtects` MUST read 0 in any single-character session;
            // a nonzero there is a defect, not a reading. `ResimGateWindowSummary`. §4
            window.freshClobbersAvoided, window.staleClobbersAvoided);
    }

    // ⛔ THE ONE STEP FUNCTION CONSTRAINED ON BOTH PEER CONCEPTS — the only path reaching
    // `reconciliation.allocateFrontierSlotsAll`; `onGameSimulationAuthority` and
    // `onGameSimulationResimulation` draw only from the resolution group (`collectInputAll`). §2
    void onGameSimulationPrediction()
        requires SimulationInputResolutionTickConcept<InputResolutionT> &&
                 SimulationReconciliationConcept<ReconciliationT>
    {
        // ⛔ TAKEN BEFORE `advancePrediction`, so it reports what this frame INHERITED — entering
        // prediction while the clock still resimulates means the previous apply edge never ran, and
        // `currentStep()` then returns the stale resim step, so `sendLocalInputToAuthorityAll`
        // stamps a stale tick until `startResimulation`. §5
        //
        // ⛔ ONE-SHOT PER EPISODE, not per stuck frame — the state persists many frames by construction. §6
        if (m_clientClock->isResimulating())
        {
            StrandedResimEpisode episode;
            if (m_resimGateProbe.noteStuckResimFrame(m_clientClock->getPredictionTick(), episode))
            {
                SIMLOG(m_logger,
                    "[Verbose][ResimProbe.Stranded] anchorTick=%u predictionTick=%u "
                    "replayedTicks=%u catchUpDeficit=%u",
                    episode.anchorTick, episode.predictionTick,
                    episode.replayedTicks, episode.catchUpDeficit);
            }
        }

        const auto result   = m_clientClock->advancePrediction();
        const auto baseStep = m_clientClock->getPredictionStep();
        // ⛔ Preserve `baseStep`'s dt (from `ClientPredictionClock`) on the `Stall`/`Skip` wrappers,
        // or per-tick timers stop. §7
        const float stepDt = baseStep.getDeltaSeconds();

        const SimulationTimeStep step = [&]()
        {
            if (result == ClientPredictionClock::AdvanceResult::Stall)
                return SimulationTimeStep(baseStep.getTick(), false, StepKind::Stall, stepDt);
            if (result == ClientPredictionClock::AdvanceResult::Skip)
                return SimulationTimeStep(baseStep.getTick(), false, StepKind::Skip,  stepDt);
            return baseStep;
        }();

        // ⛔ `getStepKind()` COLLAPSES `HardResync` TO `Normal`; both are logged so it stays visible. §7
        const char* advanceName =
            result == ClientPredictionClock::AdvanceResult::HardResync ? "HardResync"
            : result == ClientPredictionClock::AdvanceResult::Skip     ? "Skip"
            : result == ClientPredictionClock::AdvanceResult::Stall    ? "Stall"
            : "Normal";
        SIMLOG(m_logger, "[PredictionSimulation] tick=%u kind=%s advance=%s",
            step.getTick(), stepKindName(step.getStepKind()), advanceName);

        // ⛔ Capture completes the frontier pair this call opens — contract at
        // `SimulationReconciliation::allocateFrontierSlotsAll`, not re-derived here. §7
        //
        // ⛔ THE PREDICTION-ONLY FACADE, by name and by design — the free function
        // `preparePredictionSimulationStep` (`SimulationStepSequencing.h`, NOT either peer's header)
        // pairs `collectInputAll` with `allocateFrontierSlotsAll`. `onGameSimulationAuthority` does not. §7
        auto inputs = preparePredictionSimulationStep(m_inputResolution, m_reconciliation, step);

        // ⛔ SEQUENCING: `preIntegrate` before `integrateAll`; `m_lastStep` saved before `postIntegrate`. §7
        auto&       storage    = m_storage;
        const auto& staticData = m_staticData;
        m_systemsExec.firePreIntegrate(step, storage, staticData);
        m_integrationLayer.integrateAll(step, inputs);
        m_lastStep = step;
        m_systemsExec.firePostIntegrate(step, storage, staticData);
    }

    // ⛔ THE ONE STEP FUNCTION NEEDING BOTH `SimulationNetSyncTickConcept` (for
    // `setAuthorityGuardContext`) and `SimulationInputResolutionTickConcept` — and NOT the
    // reconciliation concept, because it never calls `allocateFrontierSlotsAll`. §2
    void onGameSimulationAuthority()
        requires SimulationNetSyncTickConcept<NetSyncT> &&
                 SimulationInputResolutionTickConcept<InputResolutionT>
    {
        m_serverClock->advanceTick();
        const SimulationTimeStep step = m_serverClock->getSimulationStep();
        SIMLOG(m_logger, "[AuthoritySimulation] tick=%u", step.getTick());
        // ⛔ Publishes the authority tick + `TimeConfig::rollbackWindowTicks` for the queueMove guard —
        // no literal. §4
        m_netSync.setAuthorityGuardContext(step.getTick(), m_timeConfig.rollbackWindowTicks);
        // ⛔ ON THIS ROLE THE COLLECT OPENS NO FRONTIER PAIR — the server `registerSimulatable`
        // overload allocates no correction cache (`registerPredictionOwner` gets a null provider),
        // so every fully-registered id lands in the remote-queue branch. §7
        //
        // ⛔ AND THIS METHOD NEVER CALLS `reconciliation.allocateFrontierSlotsAll` AT ALL — the
        // sweep is cache-population-filtered and every id here has no cache. ⛔ Collect (this call),
        // allocate (never here) and capture (`postPredictionAll`, never reached) agree on the
        // authority BY ALL THREE BEING ABSENT, not by one completing another. §7
        //
        // ⛔ MUST NOT ROUTE THROUGH `preparePredictionSimulationStep` — it always pairs this call
        // with `allocateFrontierSlotsAll`, which this role must never call. Call `collectInputAll`
        // directly, as below. Its banner is in `SimulationStepSequencing.h`. §7
        auto inputs = m_inputResolution.collectInputAll(step);
        // Sequencing (see onGameSimulationPrediction).
        auto&       storage    = m_storage;
        const auto& staticData = m_staticData;
        m_systemsExec.firePreIntegrate(step, storage, staticData);
        m_integrationLayer.integrateAll(step, inputs);
        m_lastStep = step;
        m_systemsExec.firePostIntegrate(step, storage, staticData);
    }

    // ⛔ `collectResimInputAll` — the one `SimulationInputResolutionTickConcept` member this step needs. §2
    void onGameSimulationResimulation()
        requires SimulationInputResolutionTickConcept<InputResolutionT>
    {
        m_clientClock->advanceResimulation();
        const SimulationTimeStep step = m_clientClock->getResimulationStep();
        SIMLOG(m_logger, "[Resim.Pre] tick=%u", step.getTick());
        // ⛔ One replay tick, at the edge `[Resim.Pre]` reports; `replayTicks`/`finishes` is the mean span. §6
        m_resimGateProbe.noteReplayTick();
        // ⛔ THE RESOLUTION PEER, NOT RECONCILIATION — resim input comes from the delay lines /
        // relay stores / neutrals it owns. ⛔ This collect opens no frontier pair either:
        // `collectResimInputAll` resolves against slots that ALREADY exist
        // (`getAppliedCaptureTickRef`) and never calls `pushPredictionTick`. §7
        auto inputs = m_inputResolution.collectResimInputAll(step.getTick());
        // ⛔ Systems fire on every resim replay tick too, or rollback routing stops being deterministic (D4). §7
        auto&       storage    = m_storage;
        const auto& staticData = m_staticData;
        m_systemsExec.firePreIntegrate(step, storage, staticData);
        m_integrationLayer.integrateAll(step, inputs);
        m_lastStep = step;
        m_systemsExec.firePostIntegrate(step, storage, staticData);
    }

    SimulationTimeStep currentStep() const
    {
        if (m_runsPrediction)
        {
            return m_clientClock->isResimulating()
                ? m_clientClock->getResimulationStep()
                : m_clientClock->getPredictionStep();
        }
        return m_serverClock->getSimulationStep();
    }

    const bool m_runsPrediction;

    IntegrationExecT&  m_integrationLayer;
    NetSyncT&          m_netSync;
    // The resolution peer — a composition-root-constructed sibling, not owned by NetSyncT.
    InputResolutionT&  m_inputResolution;
    ReconciliationT&   m_reconciliation;
    SystemsExecT&      m_systemsExec;

    // ⛔ Storage + static data HELD BY REFERENCE from the composition root, so the hook
    // fan-out and the integration executor iterate the same storage — no bridge accessor. §7
    StorageT&          m_storage;
    const StaticDataT& m_staticData;

    TimeConfig                           m_timeConfig;
    std::optional<ServerTickClock>       m_serverClock;
    std::optional<NetworkTimeEstimator>  m_networkEstimator;
    std::optional<ClientPredictionClock> m_clientClock;
    LoggerFn                             m_logger;

    // ⛔ `m_lastStep` CROSSES THE PreSim → PostSolve BOUNDARY — the step `onGameSimulation` ran,
    // consumed by the matching `onPostGameSimulation`, so post-solve writes target the tick that
    // produced the captured state. §7
    std::optional<SimulationTimeStep>    m_lastStep;

    // ⛔ CONSTRUCTED UNCONDITIONALLY, ON THE AUTHORITY TOO, where `onCheckIsSimilar` is never
    // reached (`!runsPrediction`) so it never flushes — which is why "zero new lines on the
    // authority" needs no role test, and guarding construction would buy nothing. §5
    ResimGateProbe                       m_resimGateProbe;
};

// ⛔ COMPILE-CHECKED PROOF that this vocabulary rejects the transposition `Params` guards. §8
//
// Minimal local types, each shaped like exactly one peer. ⛔ THREE SHARE `wipeAllForResync`,
// so it distinguishes none of them — distinguishing: `collect*` (resolution),
// `postPredictionAll`/`consumeResimAnchorsAll` (reconciliation), `sendCorrectionAll`. §8
namespace simulationManagerConceptProof
{
    // Shaped like a NetSync peer: transport plus the receive-side guard.
    struct NetSyncShaped
    {
        void sendCorrectionAll(const SimulationTimeStep&, int32 /*correctionRotationK*/) {}
        void sendLocalInputToAuthorityAll(uint32 /*tick*/, uint32 /*redundancyDepth*/) {}
        void setAuthorityGuardContext(uint32 /*tick*/, int32 /*rollbackWindow*/) {}
    };

    // Shaped like the resolution peer — the three members that LEFT `NetSyncShaped`.
    struct InputResolutionShaped
    {
        int  collectInputAll(const SimulationTimeStep&) { return 0; }
        int  collectResimInputAll(uint32 /*tick*/) { return 0; }
        void wipeAllForResync(uint32 /*tick*/) {}
    };

    // Shaped like a Reconciliation peer.
    struct ReconciliationShaped
    {
        // `allocateFrontierSlotsAll` — the frontier pair's opening half, reconciliation-distinguishing. §1
        void allocateFrontierSlotsAll(const SimulationTimeStep&) {}
        void postPredictionAll(const SimulationTimeStep&) {}
        void postResimulationAll(const SimulationTimeStep&) {}
        unsigned int checkDivergenceAll(uint32 /*tick*/) { return 0u; }
        void wipeAllForResync(uint32 /*tick*/) {}
        void prepareResimAll(uint32 /*tick*/) {}
        void applyResimAll() {}
        unsigned int consumeResimAnchorsAll() { return 0u; }
        void setResimTriggerPolicy(TimeConfig::ResimTriggerPolicy) {}
    };

    // Shaped like an IntegrationExec peer. Added so this concept gets a positive control and
    // three transposition negatives, not only the `Empty` negative. §8
    struct IntegrationExecShaped
    {
        void firstResimStepAll(int32 /*physicsStep*/) {}
        void captureBodyStatesAll() {}
    };

    // --- POSITIVE CONTROLS — each shaped type satisfies its own concept. ---
    static_assert(SimulationNetSyncTickConcept<NetSyncShaped>,
        "NetSyncShaped must satisfy the manager's NetSync tick surface");
    static_assert(SimulationNetSyncPublishConcept<NetSyncShaped>,
        "NetSyncShaped must satisfy the manager's NetSync publish surface");
    static_assert(SimulationInputResolutionTickConcept<InputResolutionShaped>,
        "InputResolutionShaped must satisfy the manager's resolution tick surface");
    static_assert(SimulationReconciliationConcept<ReconciliationShaped>,
        "ReconciliationShaped must satisfy SimulationReconciliationConcept");
    static_assert(SimulationIntegrationExecutorConcept<IntegrationExecShaped>,
        "IntegrationExecShaped must satisfy SimulationIntegrationExecutorConcept");

    // ⛔ NEGATIVE CONTROLS — THE TRANSPOSITION `Params` guards, caught at compile time instead. §8
    static_assert(!SimulationReconciliationConcept<NetSyncShaped>,
        "a NetSync-shaped type must NOT satisfy SimulationReconciliationConcept"
        " -- this is the same-category transposition Params exists to guard");
    static_assert(!SimulationNetSyncTickConcept<ReconciliationShaped>,
        "a Reconciliation-shaped type must NOT satisfy the manager's NetSync"
        " tick surface -- the transposition's other direction");
    static_assert(!SimulationNetSyncPublishConcept<ReconciliationShaped>,
        "a Reconciliation-shaped type must NOT satisfy the manager's NetSync"
        " publish surface either");

    // The resolution peer's negatives, BOTH directions — against the two shapes the shared
    // `wipeAllForResync` name could be mistaken for distinguishing.
    static_assert(!SimulationInputResolutionTickConcept<NetSyncShaped>,
        "a NetSync-shaped type must NOT satisfy the manager's resolution"
        " tick surface -- it no longer has collectInputAll/collectResimInputAll");
    static_assert(!SimulationInputResolutionTickConcept<ReconciliationShaped>,
        "a Reconciliation-shaped type must NOT satisfy the manager's"
        " resolution tick surface -- sharing wipeAllForResync distinguishes"
        " nothing; it still lacks collectInputAll/collectResimInputAll");
    static_assert(!SimulationNetSyncTickConcept<InputResolutionShaped>,
        "a resolution-shaped type must NOT satisfy the manager's NetSync"
        " tick surface -- the transposition's other direction (it has no"
        " setAuthorityGuardContext)");
    static_assert(!SimulationNetSyncPublishConcept<InputResolutionShaped>,
        "a resolution-shaped type must NOT satisfy the manager's NetSync"
        " publish surface either");
    static_assert(!SimulationReconciliationConcept<InputResolutionShaped>,
        "a resolution-shaped type must NOT satisfy"
        " SimulationReconciliationConcept -- sharing wipeAllForResync"
        " distinguishes nothing; it still lacks postPredictionAll etc.");

    // The integration-executor coverage gap, closed: none of the three peer shapes has
    // `firstResimStepAll`/`captureBodyStatesAll`, asserted rather than claimed. §8
    static_assert(!SimulationIntegrationExecutorConcept<NetSyncShaped>,
        "a NetSync-shaped type must NOT satisfy"
        " SimulationIntegrationExecutorConcept");
    static_assert(!SimulationIntegrationExecutorConcept<InputResolutionShaped>,
        "a resolution-shaped type must NOT satisfy"
        " SimulationIntegrationExecutorConcept");
    static_assert(!SimulationIntegrationExecutorConcept<ReconciliationShaped>,
        "a Reconciliation-shaped type must NOT satisfy"
        " SimulationIntegrationExecutorConcept");

    // A plain nonconforming type satisfies none of the four constrained concepts.
    struct Empty {};
    static_assert(!SimulationIntegrationExecutorConcept<Empty>,
        "an empty type must NOT satisfy SimulationIntegrationExecutorConcept");
    static_assert(!SimulationReconciliationConcept<Empty>,
        "an empty type must NOT satisfy SimulationReconciliationConcept");
    static_assert(!SimulationNetSyncTickConcept<Empty>,
        "an empty type must NOT satisfy the manager's NetSync tick surface");
    static_assert(!SimulationInputResolutionTickConcept<Empty>,
        "an empty type must NOT satisfy the manager's resolution tick surface");
} // namespace simulationManagerConceptProof

OGSIM_OPTIMIZE_ON
// pragma optimize on — restore command-line optimization settings.
