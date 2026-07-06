#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <limits>
#include <optional>

#include "OGAssert.h"
#include "OGSimulation/PCTimeManagement/ServerTickClock.h"
#include "OGSimulation/PCTimeManagement/NetworkTimeEstimator.h"
#include "OGSimulation/PCTimeManagement/ClientPredictionClock.h"
#include "OGSimulation/SimulationLog.h"
#include "OGSimulation/SimulationTimeContext.h"

#pragma optimize( "", off )

// SimulationUpdateInfo — passed from the Chaos async callback into onGameSimulation.
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

// ---------------------------------------------------------------------------
// SimulationManager<IntegrationExecT, NetSyncT, ReconciliationT>
//
// Orchestrates the simulation loop. Holds references to the three peers
// (integration executor, net sync, reconciliation) and owns the clocks.
//
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
// ---------------------------------------------------------------------------

template <typename IntegrationExecT, typename NetSyncT, typename ReconciliationT>
class SimulationManager
{
public:
    using LoggerFn = std::function<void(const char*)>;

    SimulationManager(
        bool            shouldRunPrediction,
        double          tickFrequency,
        IntegrationExecT& integrationLayer,
        NetSyncT&         netSync,
        ReconciliationT&  reconciliation,
        LoggerFn          logger = nullptr)
        : m_runsPrediction(shouldRunPrediction)
        , m_integrationLayer(integrationLayer)
        , m_netSync(netSync)
        , m_reconciliation(reconciliation)
        , m_logger(logger)
    {
        m_timeConfig.tickFrequency = 1.0 / tickFrequency;

        if (shouldRunPrediction)
        {
            m_networkEstimator.emplace(m_timeConfig, logger);
            m_clientClock.emplace(m_timeConfig, *m_networkEstimator, logger);
            m_clientClock->registerResyncCallback(
                [this](unsigned int newPredictionTick)
                {
                    SIMLOG(m_logger, "[TimeResync.Wipe] newPredictionTick=%u", newPredictionTick);
                    m_reconciliation.wipeAllForResync(newPredictionTick);
                    m_netSync.wipeAllForResync(newPredictionTick);
                });
        }
        else
        {
            // SimulationManager's `tickFrequency` ctor param is actually the fixed
            // physics dt in seconds (it's passed in from solver->GetAsyncDeltaTime()).
            // Feed that directly to ServerTickClock so the steps it produces carry
            // the right dt through to integrate().
            m_serverClock.emplace(static_cast<float>(tickFrequency), logger);
        }
    }

    void setLogger(LoggerFn logger) { m_logger = std::move(logger); }

    bool runsPrediction() const { return m_runsPrediction; }

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

    // -----------------------------------------------------------------------
    // Simulation dispatch — called from FSimulationManagerAsyncCallback
    // -----------------------------------------------------------------------

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

    // Called from FSimulationManagerAsyncCallback::OnPostSolve_Internal.
    // Runs after Chaos has integrated and solved physics for this sub-step.
    // Captures post-solve state into the cache and detects the resim-batch
    // catch-up edge to apply resim results.
    void onPostGameSimulation(const SimulationUpdateInfo& updateInfo)
    {
        if (!m_lastStep.has_value())
            return;

        m_integrationLayer.captureBodyStatesAll();

        if (m_runsPrediction)
        {
            if (updateInfo.isResimulation())
            {
                m_reconciliation.postResimulationAll(*m_lastStep);

                // Apply-resim edge: Chaos still in resim mode but our clock has
                // caught up (advanceResimulation brought m_resimulationTick up
                // to m_predictionTick during this sub-step's PreSim). Unique to
                // the last resim sub-step's PostSolve.
                const bool chaosIsResim = updateInfo.isResimulation();
                const bool clockIsResim = m_clientClock->isResimulating();
                if (chaosIsResim && !clockIsResim)
                {
                    SIMLOG(m_logger, "[Resim.Finish] predictionTick=%u",
                        m_clientClock->getPredictionTick());
                    m_clientClock->finishResimulation();
                    m_reconciliation.applyResimAll();
                }
            }
            else
            {
                m_reconciliation.postPredictionAll(*m_lastStep);
            }
        }
    }

    unsigned int onCheckIsSimilar()
    {
        // Caller (FSimulationManagerAsyncCallback::TriggerRewindIfNeeded_Internal)
        // short-circuits on !runsPrediction() so this is only reached on the
        // predicting client — the authority is the truth, never rewinds.
        const unsigned int correctionTick = m_reconciliation.checkDivergenceAll();
        if (correctionTick == 0u)
        {
            SIMLOG(m_logger, "[ResimCheck.IsSimilar] correctionTick=0 result=noResim");
            return std::numeric_limits<unsigned int>::max();
        }

        SIMLOG(m_logger, "[ResimCheck.Divergence] correctionTick=%u", correctionTick);
        return correctionTick;
    }

    // chaosStep is the raw Chaos physics step; simTick is the simulation tick to resim from.
    void prepareResimulation(int32_t chaosStep, uint32_t simTick)
    {
        if (!m_runsPrediction)
        {
            OG_CHECK(false, "prepareResimulation called on authority — not expected");
            return;
        }
        SIMLOG(m_logger, "[Resim.Prepare] chaosStep=%d simTick=%u", chaosStep, simTick);
        m_clientClock->startResimulation(simTick);
        m_reconciliation.prepareResimAll(simTick);
        m_integrationLayer.firstResimStepAll(chaosStep);
    }

    // Tick of the most recently integrated step — authority tick / prediction tick /
    // resim tick, i.e. whichever step integrateAll just ran. Consumed by the manager's
    // post-integrate inbound-hit routing pass to one-shot projectile slots that ended
    // THIS tick. Returns 0 before the first integrate (0 is the reserved pre-sim tick).
    uint32_t currentIntegratedTick() const
    {
        return m_lastStep.has_value() ? m_lastStep->getTick() : 0u;
    }

    void onPostSimulationGameThread()
    {
        const SimulationTimeStep step = currentStep();
        m_netSync.sendCorrectionAll(step);
        // redundancy depth tracks the runtime tick rate via
        // TimeConfig::redundancyDepthTicks (5 @ 100 Hz interim / 3 @ 60 Hz target).
        m_netSync.sendLocalInputToAuthorityAll(
            step.getTick(), static_cast<uint32>(m_timeConfig.redundancyDepthTicks));
    }

private:
    void onGameSimulationPrediction()
    {
        const auto result   = m_clientClock->advancePrediction();
        const auto baseStep = m_clientClock->getPredictionStep();
        // baseStep already carries dt from ClientPredictionClock; preserve it on
        // the synthesized Stall/Skip wrappers so per-tick timers advance correctly.
        const float stepDt = baseStep.getDeltaSeconds();

        const SimulationTimeStep step = [&]()
        {
            if (result == ClientPredictionClock::AdvanceResult::Stall)
                return SimulationTimeStep(baseStep.getTick(), false, StepKind::Stall, stepDt);
            if (result == ClientPredictionClock::AdvanceResult::Skip)
                return SimulationTimeStep(baseStep.getTick(), false, StepKind::Skip,  stepDt);
            return baseStep;
        }();

        // result captures HardResync distinctly; step.getStepKind() collapses it
        // to Normal (treated like Normal for the integrate step). Log both so a
        // HardResync advance is visible in the trace.
        const char* advanceName =
            result == ClientPredictionClock::AdvanceResult::HardResync ? "HardResync"
            : result == ClientPredictionClock::AdvanceResult::Skip     ? "Skip"
            : result == ClientPredictionClock::AdvanceResult::Stall    ? "Stall"
            : "Normal";
        SIMLOG(m_logger, "[PredictionSimulation] tick=%u kind=%s advance=%s",
            step.getTick(), stepKindName(step.getStepKind()), advanceName);

        auto inputs = m_netSync.collectInputAll(step);
        m_integrationLayer.integrateAll(step, inputs);
        m_lastStep = step;
    }

    void onGameSimulationAuthority()
    {
        m_serverClock->advanceTick();
        const SimulationTimeStep step = m_serverClock->getSimulationStep();
        SIMLOG(m_logger, "[AuthoritySimulation] tick=%u", step.getTick());
        // publish the current authority tick + rollback window so the
        // RPC-arrival queueMove path can reject too-far-future capture ticks. The window
        // is TimeConfig::rollbackWindowTicks (no hardcoded literal — R-P1 clean).
        m_netSync.setAuthorityGuardContext(step.getTick(), m_timeConfig.rollbackWindowTicks);
        auto inputs = m_netSync.collectInputAll(step);
        m_integrationLayer.integrateAll(step, inputs);
        m_lastStep = step;
    }

    void onGameSimulationResimulation()
    {
        m_clientClock->advanceResimulation();
        const SimulationTimeStep step = m_clientClock->getResimulationStep();
        SIMLOG(m_logger, "[Resim.Pre] tick=%u", step.getTick());
        auto inputs = m_reconciliation.collectResimInputAll(step.getTick());
        m_integrationLayer.integrateAll(step, inputs);
        m_lastStep = step;
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

    IntegrationExecT& m_integrationLayer;
    NetSyncT&         m_netSync;
    ReconciliationT&  m_reconciliation;

    TimeConfig                           m_timeConfig;
    std::optional<ServerTickClock>       m_serverClock;
    std::optional<NetworkTimeEstimator>  m_networkEstimator;
    std::optional<ClientPredictionClock> m_clientClock;
    LoggerFn                             m_logger;

    // Step from the most recent onGameSimulation call, consumed by the
    // matching onPostGameSimulation. Crosses the PreSim → PostSolve boundary
    // so the post-solve cache writes target the same tick / StepKind as the
    // integrate step that produced the state being captured.
    std::optional<SimulationTimeStep>    m_lastStep;
};

#pragma optimize( "", on )
