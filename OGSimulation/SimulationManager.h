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
// [item 80 / B6] The two peer concepts this file names directly, so the
// `requires` clauses below can use them. Nothing else in this file needed the
// concrete peer classes (NetSyncT / ReconciliationT / IntegrationExecT stay
// duck-typed template parameters) — these two includes exist ONLY to bring
// the concept declarations into scope. NetSyncT is constrained by two
// sibling concepts DEFINED IN THIS FILE, not by including SimulationNetSync.h
// — see the rationale beside SimulationNetSyncTickConcept below.
#include "OGSimulation/SimulationIntegrationExecutor.h"
#include "OGSimulation/SimulationReconciliation.h"
#include "OGSimulation/SimulationTimeContext.h"
// [og-netcode-v2-input-relay item 42] The resim-gate telemetry. Physics-thread
// half only — the game-thread half (CorrectionLandingProbe) is owned by
// SimulationNetSync, at the site that knows the character id and its class.
#include "OGSimulation/ResimGateProbe.h"
#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness across all build configs (breakpoints hit,
// locals visible, call-stack intact). OGSim-core convention; canonical statement — the
// other OGSim-core pragma sites point here.
//
// [og-netcode-v2-input-relay item 77] As of this item the pair below is
// OGSIM_OPTIMIZE_OFF/ON, not a raw pragma — every OGSim-core, OGBrawler-core,
// and UE-adapter site (43 files) uses the same two macros, defined once in
// OGSimulation/CompilerControl.h. That header is the mechanism; this comment
// is the contract:
//   - Default (Debug/Development, no switch defined): the macros expand to
//     exactly this pragma pair — daily debugging is untouched by item 77.
//   - Define OGSIM_FORCE_OPTIMIZED=1 (or build Test/Shipping) and both macros
//     expand to nothing everywhere — the whole core compiles at full
//     command-line optimization.
//   - Non-MSVC compilers get a no-op pair unconditionally — CompilerControl.h
//     is where the MSVC-only pragma is made portable, not just renamed.
//
// THE MEASUREMENT RULE: no gate-family cost number (items 43, 46, 78, ...) is
// quotable unless the log or record that produced it NAMES the optimize
// setting it was taken under. A number measured under the default (this
// pragma active, as every archived resim number to date was) and one measured
// with OGSIM_FORCE_OPTIMIZED=1 are not the same measurement — treating them as
// interchangeable without saying so is this initiative's signature defect (an
// instrument measuring the wrong quantity under the right name), applied to
// build configuration. Full rationale in OGSimulation/CompilerControl.h.
OGSIM_OPTIMIZE_OFF

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
// [og-netcode-v2-input-relay item 80 / B6] SimulationManager-facing splits of
// SimulationNetSyncConcept (SimulationNetSync.h).
//
// SimulationManager's own six template parameters never carry SimulatableTs —
// that pack lives inside NetSyncT's own instantiation
// (SimulationNetSync<SimulatableTs...>), never as one of the manager's type
// parameters — so SimulationNetSyncConcept's two SimulatableTs-typed return
// checks (prepareSimulationStep / collectResimInputAll ->
// convertible_to<ResolvedInputs<SimulatableTs...>>) cannot be expressed at
// the manager's generality: there is no pack for the manager to supply.
// These two sibling concepts check the SAME method names and argument
// shapes, presence-only (no return-type pin), split along the two groups
// SimulationManager's own member functions actually call together: the
// per-tick read side (onGameSimulationPrediction/Authority/Resimulation) and
// the once-per-tick publish side (onPostSimulationGameThread).
// SimulationNetSyncConcept itself is untouched by this task and stays the
// precise, SimulatableTs-exact check used ad hoc adapter-side where the
// concrete SimulatableTs are known (task 6, SimulationManagerUImplConceptTest.cpp).
//
// ⚠ [item 83 / 80 f2] PRESENCE-ONLY IS NOT THE STRONGEST CHECK REACHABLE HERE,
// AND THAT IS WORTH SAYING PLAINLY RATHER THAN LEAVING IMPLIED. A concept could
// still constrain the SHAPE of `prepareSimulationStep`/`collectResimInputAll`'s return
// (e.g. "convertible to some `std::tuple` of `std::unordered_map<unsigned int, X>`s")
// without knowing `SimulatableTs` — strictly tighter than presence-only, and
// expressible at this generality. That was not attempted here: the manager's own
// proof namespace below demonstrates just how weak presence-only is
// (`InputResolutionShaped::prepareSimulationStep` returns a bare `int` and still satisfies this
// concept), and a wrong return type at the manager still fails loudly downstream —
// at the real `ResolvedInputs` consumer (`SimulationIntegrationExecutor::integrateAll`) —
// rather than silently. Left as a residual for a future revisit of this concept,
// not attempted in this task.
//
// [item 83 / 80 f4] PLACEMENT, RECONSIDERED ON THE MERITS NOW THAT THE
// SCHEDULING CONTENTION IS OVER. These two concepts were originally defined here
// (rather than beside `SimulationNetSyncConcept` in `SimulationNetSync.h`, where
// `SimulationReconciliationConcept` and `SimulationIntegrationExecutorConcept`
// each live beside THEIR peer class) because item 79 owned `SimulationNetSync.h`
// concurrently. That is a scheduling reason, not a design one, and the
// "definition-at-call-site is better anyway" argument that accompanied it does
// not hold as a general principle — this file's other two peer concepts are both
// defined at their PEER's declaration site and pulled in by `#include`, not
// defined at their one call site (here). Item 79 has since landed, so the
// contention that forced this placement no longer exists.
// RULING: left here rather than moved, because this task (item 83) owns
// `SimulationManager.h`, `docs/ThreadingCrossings.md`, `CorrectionCache.h` and
// `NetSyncTelemetry.h` — not `SimulationNetSync.h` — and moving these two
// concepts is a `SimulationNetSync.h` edit with its own blast radius (a new
// include cycle to check, since `SimulationManager.h` deliberately does not
// include `SimulationNetSync.h` today). Recorded here as a decision, not an
// oversight: a follow-up task that already owns `SimulationNetSync.h` should
// relocate `SimulationNetSyncTickConcept` / `SimulationNetSyncPublishConcept`
// beside `SimulationNetSyncConcept` for consistency with the other two peers.
// ---------------------------------------------------------------------------

// [item 87 / design §C.7] `collectInputAll` (RENAMED `prepareSimulationStep`
// at item 90) / `collectResimInputAll` / `wipeAllForResync` LEFT this concept
// — they moved off `SimulationNetSync` onto the resolution peer at item 87,
// and with them the manager no longer calls any of the three ON `m_netSync`.
// This concept shrinks to the one member the manager still reaches on
// NetSyncT from a tick-group method: the receive-side guard, published once
// per authority tick immediately before `onGameSimulationAuthority`'s own
// `prepareSimulationStep` call (now on `InputResolutionT` — see
// `SimulationInputResolutionTickConcept` below).
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

// ---------------------------------------------------------------------------
// [item 87 / design §C.7] SimulationManager-facing split of
// SimulationInputResolutionConcept (SimulationInputResolution.h) — same
// SimulatableTs-invisibility reasoning as `SimulationNetSyncTickConcept`
// above (item 80/83's residual, inherited here): the manager's own template
// parameters never carry `SimulatableTs`, so the peer concept's two
// SimulatableTs-typed return checks cannot be expressed at the manager's
// generality. Presence-only, checking the group all three prediction/
// authority/resim step methods call: `prepareSimulationStep` (prediction,
// authority; named `collectInputAll` before item 90's rename),
// `collectResimInputAll` (resim) and `wipeAllForResync` (the
// resync-callback path, reachable from the same physics-thread call chain
// `onGameSimulationPrediction` drives — see that concept's own history on
// `SimulationNetSyncTickConcept` above for why a resync-reachable member
// belongs on a tick-group concept rather than a publish one).
//
// ⚠ `wipeAllForResync` IS NOW SHARED VERBATIM BY THREE PEER CONCEPTS —
// `SimulationReconciliationConcept`, this one, and (until item 87) formerly
// `SimulationNetSyncConcept` too, before it left NetSync entirely — so a
// wipe-only clause distinguishes NOTHING between a resolution-shaped peer and
// a reconciliation-shaped one. The DISTINGUISHING members are `collect*`
// (resolution, checked below), `postPredictionAll` /
// `consumeResimAnchorsAll` (reconciliation) and `sendCorrectionAll`
// (netsync). The manager's ctor stays deliberately unconstrained by any peer
// concept for exactly this reason (see the ctor's own comment) — a proof
// that leaned on `wipeAllForResync` alone would look like transposition
// protection while providing none, the same trap item 80's unconstrained
// ctor documented.
// ---------------------------------------------------------------------------
template <typename T>
concept SimulationInputResolutionTickConcept = requires(
    T& t, const SimulationTimeStep& step, uint32_t tick)
{
    { t.prepareSimulationStep(step) };
    { t.collectResimInputAll(tick) };
    { t.wipeAllForResync(tick) };
};

// ---------------------------------------------------------------------------
// SimulationManager<IntegrationExecT, NetSyncT, InputResolutionT,
//                   ReconciliationT, SystemsExecT, StorageT, StaticDataT>
//
// Orchestrates the simulation loop. Holds references to the five peers
// (integration executor, net sync, input resolution, reconciliation, systems
// executor), plus references to the externally-owned object storage and
// static data, and owns the clocks. Storage + static data live at the engine
// adapter's composition root (shared by all peers); the manager reaches them
// directly rather than bridging through the integration executor.
//
// [item 87 / design §A.3] InputResolutionT is the seventh template parameter,
// added when the resolution peer was promoted off `SimulationNetSync`'s
// scaffold to a real, composition-root-constructed peer of its own. The
// manager reads prediction/authority/resim input straight from it
// (`prepareSimulationStep` / `collectResimInputAll`) and drives its resync wipe —
// NetSyncT's own tick-group surface shrank to the receive-side guard alone.
//
// The SystemsExecT peer (the SimulationSystemsExecutor — the ogsim-system-api
// "fourth peer") fires cross-simulatable system hooks around integrateAll:
// firePreIntegrate BEFORE integration, firePostIntegrate AFTER (see the
// sequencing note in onGameSimulation* below). Character-lifecycle
// notifications are forwarded through notifyCharacterRegistered/Unregistered.
// The manager depends only on the peer's duck-typed method surface — it never
// names a concrete system type.
//
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
// ---------------------------------------------------------------------------

template <typename IntegrationExecT, typename NetSyncT, typename InputResolutionT,
          typename ReconciliationT, typename SystemsExecT,
          typename StorageT, typename StaticDataT>
class SimulationManager
{
public:
    using LoggerFn = std::function<void(const char*)>;

    // Grouped dependency refs for the ctor. Bundles the five
    // peer refs + storage + staticData + logger into one named aggregate so the
    // two composition-root emplace() sites cannot silently transpose the adjacent
    // duck-typed peer refs (a transposition of two same-category refs COMPILES —
    // they are duck-typed — and misbehaves only at runtime). Nested so it inherits
    // the enclosing template params: call sites write ManagerType::Params{ ... }
    // without re-naming the seven type arguments. Only the scalar loop config
    // (shouldRunPrediction / tickFrequency) stays positional.
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

        // [item 80 / B6, updated item 87] The ctor itself stays UNCONSTRAINED
        // by any peer concept, deliberately: `wipeAllForResync(uint32)` is now
        // shared verbatim by THREE peer concepts —
        // SimulationInputResolutionTickConcept, SimulationReconciliationConcept,
        // and (until item 87) formerly SimulationNetSyncConcept too — so a
        // requires-clause built only from it could not distinguish a
        // resolution-shaped peer from a reconciliation-shaped one — it would
        // give the false impression of transposition protection while
        // providing none. The methods that actually differ between the peers
        // are constrained below, at the member functions that call them.
        if (shouldRunPrediction)
        {
            m_networkEstimator.emplace(m_timeConfig, params.logger);
            m_clientClock.emplace(m_timeConfig, *m_networkEstimator, params.logger);
            m_clientClock->registerResyncCallback(
                [this](unsigned int newPredictionTick)
                {
                    SIMLOG(m_logger, "[TimeResync.Wipe] newPredictionTick=%u", newPredictionTick);
                    // [item 48] THE SLOT-PROVENANCE LOG's SECOND CALL SITE IS NOT
                    // HERE — it is inside `wipeAllForResync`, one line down, and
                    // that placement is deliberate rather than incidental. Putting
                    // it here would have added a method to the four mock
                    // `Reconciliation` types the LLTs instantiate this ctor with,
                    // for a line that belongs to the wipe sweep itself and reads
                    // the same caches in the same pass. See
                    // `SimulationReconciliation::logSlotProvenanceFor`.
                    m_reconciliation.wipeAllForResync(newPredictionTick);
                    // [item 87] Re-targeted off `m_netSync` — the resolution
                    // peer now owns this call, matching its own container
                    // lifecycle (design §C.4/§C.7).
                    m_inputResolution.wipeAllForResync(newPredictionTick);
                });
        }
        else
        {
            // SimulationManager's `tickFrequency` ctor param is actually the fixed
            // physics dt in seconds (it's passed in from solver->GetAsyncDeltaTime()).
            // Feed that directly to ServerTickClock so the steps it produces carry
            // the right dt through to integrate().
            m_serverClock.emplace(static_cast<float>(tickFrequency), params.logger);
        }
    }

    void setLogger(LoggerFn logger) { m_logger = std::move(logger); }

    bool runsPrediction() const { return m_runsPrediction; }

    // Read-only view of the manager's owned TimeConfig.
    //
    // [C.2 / T10] Exists so an engine-side composition root can bind
    // per-connection structures (ConnectionTierTable, ServerInputDelayQueue)
    // to the SAME config instance the clocks and the authority guard already
    // read. Those structures hold `const TimeConfig&`, so handing out a
    // reference to a manager-owned member — rather than letting the adapter
    // keep a second TimeConfig — is what makes "one config, one tier policy"
    // true by construction instead of by convention. The reference is stable
    // for the manager's lifetime (m_timeConfig is a by-value member).
    const TimeConfig& getTimeConfig() const { return m_timeConfig; }

    // [T11 / og-netcode-v2-input-relay] THE one writable location for the session
    // relay delay floor. (RelayDelaySpectrumDesign.md §6, §11 Q1/Q6.)
    //
    // Deliberately a single narrow setter rather than a mutable `editTimeConfig()`:
    // the rest of TimeConfig is a start-up constant that the clocks, the tier
    // table and the delay queue all hold by reference, and handing out a mutable
    // handle would make every one of those fields silently re-settable mid-session.
    // The floor is the one field that legitimately changes after construction —
    // it is server-owned and REPLICATED, so a client learns it from an OnRep, and
    // the deferred dynamic-floor policy (§11 Q6) publishes through this same door.
    //
    // CLAMPED HERE TOO. `clampRelayDelayFloorTicks` also runs at both intake
    // points; repeating it at the single write site means no future caller can
    // store an out-of-range floor by forgetting, and the clamp is idempotent.
    //
    // THREADING. Game thread only, and the value it feeds crosses to the physics
    // thread exactly where it always did — through the one
    // `setClientEffectiveInputDelayTicks` atomic. On the server the floor is
    // written once at composition, before any connection exists; on a client it
    // is written from the floor OnRep, a game-thread UObject callback, and read
    // back on the same thread by the recompute.
    void setRelayDelayFloorTicks(int32_t requestedFloorTicks)
    {
        m_timeConfig.relayDelayFloorTicks =
            clampRelayDelayFloorTicks(requestedFloorTicks, m_timeConfig);
    }

    // ⛔ RETIRED (og-netcode-v2-input-relay item 63 / RN-13, 2026-08-16): there is
    // deliberately no relay-ring-depth setter here any more (its old identifier
    // is on record in RN-13, ReviewNotes.md). It wrote a session-configurable
    // retention depth into the outbound relay ring's replace-latest write path;
    // item 34 replaced that write path with bare-C1 flush-on-poll, whose stage
    // capacity is `relayedInputRing::kMaxDepth` — a compile-time constant with no
    // setter to receive a configured value — and item 63 removed the now-inert
    // setter along with the field and its ini intake chain.

    // [T39 / og-netcode-v2-input-relay] THE one writable location for the session
    // correction-state rotation width — the door the composition root's
    // `[OGNetcode] CorrectionRotationK` ini override writes through.
    //
    // SERVER-ONLY and NOT replicated, for the same reason as the delay floor's
    // ONE-SHOT siblings: only the authority runs
    // `SimulationNetSync::sendCorrectionAll`, so a client's copy of this value
    // would have no reader, and a receiver reconciles against whatever
    // corrections arrive without needing to know the sender's cadence. So this
    // setter has no OnRep counterpart and no client-side intake point.
    //
    // ONE-SHOT AT COMPOSITION. Not a hard requirement the way the depth's is (K
    // holds no allocated state, so changing it mid-session would merely re-phase
    // the schedule), but it is deliberately kept to the same discipline: the
    // cadence is a DESIGNED number that a session's probe output is read against,
    // and a value that can move mid-run makes those readings unattributable.
    // It must not acquire a cvar.
    //
    // CLAMPED HERE TOO, with the same shared idempotent guard the intake and the
    // selection predicate call (`correctionRotation::clampK`). 0 and negatives
    // clamp UP to 1: a K of 0 is a correction channel that never publishes, which
    // is a permanent desync, not "off".
    void setCorrectionRotationK(int32_t requestedK)
    {
        m_timeConfig.correctionRotationK = correctionRotation::clampK(requestedK);
    }

    // [item 45 / og-netcode-v2-input-relay] THE one writable location for the
    // session RESIM-GATE TRIGGER POLICY — the door the composition root's
    // `[OGNetcode] ResimTriggerPolicy` ini override writes through.
    //
    // ROLE-AGNOSTIC, and unlike the two knobs above that is the simplest correct
    // answer rather than a considered exception: the gate only exists on a
    // predicting client (an authority allocates no correction caches and never
    // rewinds), so applying the value on a server is inert. Writing it on both keeps
    // one TimeConfig shape for both roles and keeps the intake in the composition
    // root's shared section, where a reader looking for "what is this session's
    // policy" finds one answer.
    //
    // ONE-SHOT AT COMPOSITION, and here that IS load-bearing. The trigger policy is
    // pushed down into every `StateCorrectionCache` and consulted on the GAME thread
    // at the correction-landing site, with no synchronization — safe precisely
    // because it is written once, before any correction can land. A cvar or console
    // command would make it writable while landings are in flight, which is a data
    // race on the policy word AND a run whose trigger regime changed mid-measurement.
    // There must not be one.
    //
    // ⚠ THE POLICY SETTER HAS A SECOND EFFECT and must therefore stay the only door:
    // it fans the value out through `SimulationReconciliation::setResimTriggerPolicy`
    // to every allocated cache. A caller that wrote `TimeConfig` directly would leave
    // the caches on the compiled default and the proof line reporting a policy
    // nothing implements.
    // [item 80 / B6] setResimTriggerPolicy (SimulationReconciliationConcept).
    void setResimTriggerPolicy(TimeConfig::ResimTriggerPolicy policy)
        requires SimulationReconciliationConcept<ReconciliationT>
    {
        m_timeConfig.resimTriggerPolicy = policy;
        m_reconciliation.setResimTriggerPolicy(policy);
    }

    // ⛔ THERE IS NO `setResimCooldownTicks`, AND THAT IS A RULING. A trigger-rate
    // ceiling defers acting on a correction already known to disagree, which is the
    // defect item 45 repairs; the throttle is structural instead (one resim in
    // flight, one pending, mid-replay landings coalesce). See the ruling block on
    // `TimeConfig::resimTriggerPolicy` and the argument at
    // `resimGate::policyEnforcesDepthCeiling`.

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
    // [item 80 / B6] captureBodyStatesAll (SimulationIntegrationExecutorConcept)
    // plus postPredictionAll / postResimulationAll / applyResimAll /
    // consumeResimAnchorsAll (SimulationReconciliationConcept) — every one of
    // this method's peer calls below.
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
                // [item 42 / I6] The sweep's discard count feeds `replayOverruns`.
                // The call, the sweep and the cache's own per-discard Warning line
                // are all exactly as they were.
                //
                // [item 47] AND the same sweep now reports how many corrected
                // slots it was refused permission to overwrite, split fresh/stale.
                // All three counts ride this ONE call rather than a second sweep —
                // they are derived from the same per-character pass, and
                // separating them would let them describe different replay ticks.
                // No new log line: they appear as fields on the existing
                // `[ResimProbe.Apply]` line (T19 volume discipline; item 47
                // forbids new `[Resim.` / `[ResimCheck.` lines outright).
                //
                // [item 55] The sweep returns one `ResimSweepDiagnostics` by value
                // (RN-3, option B) instead of a return value plus two out-pointers
                // — `auto` here keeps this call site duck-typed on the
                // reconciliation peer, exactly as
                // `m_inputResolution.prepareSimulationStep`'s return is
                // captured a few lines up.
                const auto resimDiagnostics = m_reconciliation.postResimulationAll(*m_lastStep);
                m_resimGateProbe.noteReplayOverruns(resimDiagnostics.discards);
                m_resimGateProbe.noteCorrectionProtections(
                    resimDiagnostics.freshProtections, resimDiagnostics.staleProtections);

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
                    // [item 42 / I5] THE APPLY EDGE, counted at the edge itself —
                    // the same condition the existing `[Resim.Finish]` line reports,
                    // so `finishes` and that line's occurrence count are the same
                    // number by construction. That identity is item 42's I5
                    // self-validation: over any archived protocol run the counter
                    // and the grep must agree, and a disagreement means the
                    // instrument is measuring something other than the emitter.
                    m_resimGateProbe.noteFinish();
                    m_clientClock->finishResimulation();
                    m_reconciliation.applyResimAll();
                    // [item 45] W2 — THE CONSUME EDGE. The resim that just completed
                    // consumes the anchor it was PREPARED with, per character, as a
                    // CAS: a correction that landed on the game thread mid-replay has
                    // raised that character's anchor past the prepared value, so its
                    // CAS fails and the anchor SURVIVES to re-trigger next frame.
                    // That is the designed behaviour and the reason termination is
                    // structural rather than timing-dependent — see
                    // `StateCorrectionCache::consumeResimAnchor`.
                    //
                    // ⚠ ORDER: after `applyResimAll`, so the gate is closed only once
                    // the replayed state has actually been published into live state.
                    // And on THIS edge rather than at prepare, because ~20 % of
                    // prepares never reach here (item 42's stranded class) and an
                    // anchor consumed at prepare would take its correction with it.
                    //
                    // [item 57 / RN-6] THE RETURN VALUE IS NO LONGER DISCARDED. It is
                    // fed to `m_resimGateProbe.noteSurvivingAnchors`, surfaced as a
                    // field on the `[ResimProbe.Gate]` line — see
                    // `SimulationReconciliation::consumeResimAnchorsAll`'s comment for
                    // why that promotion's old blocker ("reads 0 until item 46")
                    // expired the day item 46 shipped.
                    m_resimGateProbe.noteSurvivingAnchors(m_reconciliation.consumeResimAnchorsAll());
                    // [item 48] THE SLOT-PROVENANCE LOG — one Verbose line per
                    // character, ONCE PER COMPLETED RESIM. This edge is chosen
                    // because it is the only point at which a replay's whole
                    // effect on the cache is finished and visible: the span has
                    // been written, the protections have been taken, and the
                    // frontier slot has been published. Emitting at prepare would
                    // show the map the resim was ABOUT to change.
                    //
                    // ⛔ It is VERBOSE-ONLY and reads nothing any decision uses —
                    // see `SimulationReconciliation::getDiagnostics().
                    // logSlotProvenanceAll` and SlotStateProvenance.h. At the
                    // shipped `LogOGResimProbe=Warning` these lines do not exist,
                    // which is why a per-resim 60-character-per-character line is
                    // affordable at all.
                    m_reconciliation.getDiagnostics().logSlotProvenanceAll();
                }
            }
            else
            {
                m_reconciliation.postPredictionAll(*m_lastStep);
            }
        }
    }

    // [item 80 / B6] checkDivergenceAll (SimulationReconciliationConcept).
    unsigned int onCheckIsSimilar()
        requires SimulationReconciliationConcept<ReconciliationT>
    {
        // Caller (FSimulationManagerAsyncCallback::TriggerRewindIfNeeded_Internal)
        // short-circuits on !runsPrediction() so this is only reached on the
        // predicting client — the authority is the truth, never rewinds. That
        // short-circuit is ALSO why item 42's "zero new lines on the authority"
        // criterion holds without a role test here: the window is driven by this
        // method, so on a server it never advances and never flushes.
        // [item 45] THE DEPTH POLICY, read LIVE from TimeConfig and handed down —
        // the `sendCorrectionAll(step, correctionRotationK)` shape, for the same
        // reason: caching it in reconciliation would make an ini-driven setting
        // silently ineffective. 0 means "no depth policy", which is what the legacy
        // trigger regime gets (`policyEnforcesDepthCeiling`), so on today's default
        // this fold is byte-for-byte the pre-item-45 one.
        const uint32_t maxAnchorDepthTicks =
            resimGate::policyEnforcesDepthCeiling(m_timeConfig.resimTriggerPolicy)
                ? static_cast<uint32_t>(m_timeConfig.rollbackWindowTicks)
                : 0u;

        unsigned int diagnosticDeepAnchorSkips = 0u;
        const unsigned int correctionTick =
            m_reconciliation.checkDivergenceAll(maxAnchorDepthTicks, &diagnosticDeepAnchorSkips);

        // ⛔ [item 45] THERE IS NO RATE CEILING HERE, AND THAT IS A RULING RATHER THAN
        // AN OMISSION — a `resimCooldownTicks` suppression branch stood on this line
        // and was removed. A ceiling defers acting on a correction already KNOWN to
        // disagree with prediction, which is the defect this gate was rebuilt to
        // repair. What throttles the rate instead is STRUCTURAL: Chaos consults this
        // method only on non-resim advances (never inside its own rewind loop) and the
        // anchor is consumed only on the completion edge, so AT MOST ONE RESIM IS IN
        // FLIGHT AND AT MOST ONE MORE IS PENDING, and corrections landing mid-replay
        // coalesce into the pending anchor via its CAS-max and fire once as a single
        // deeper replay. A correction ahead of a running resim RE-ANCHORS; it never
        // restarts the replay and never waits. Full argument at
        // `resimGate::policyEnforcesDepthCeiling`.
        const bool requestingResim = correctionTick != 0u;

        // [item 42 / I1] THE DENOMINATOR, and the first thing that task exists to
        // fix. Both branches below already existed; what did not exist was any way
        // to see the declining one. Its log line — `[ResimCheck.IsSimilar]`, one
        // line down — emits at `Log` under `LogOGSimTick`, which ships at `Warning`,
        // so it has ZERO occurrences in every archived log and trigger counts have
        // always been read against nothing. This counts instead of logging, and the
        // per-window flush below is at Warning on its own category.
        //
        // [item 45] `diagnosticDeepAnchorSkips` rides the same call rather than a new
        // log line: T19 volume discipline, and item 45 forbids new `[Resim.` /
        // `[ResimCheck.` lines outright. It appears as one extra field on the
        // existing `[ResimProbe.Gate]` line, and it reads a structural 0 under the
        // shipped legacy policy (which enforces no depth policy for the anchor to
        // fail). The count is diagnostic; `checkDivergenceAll`'s underlying skip is
        // production (see its declaration, `docs/DiagnosticsConventions.md` §4).
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

    // chaosStep is the raw Chaos physics step; simTick is the simulation tick to resim from.
    // [item 80 / B6] prepareResimAll (SimulationReconciliationConcept) +
    // firstResimStepAll (SimulationIntegrationExecutorConcept).
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
        // [item 42 / I5] Counted at the same edge the existing `[Resim.Prepare]`
        // line reports, so `prepares` equals that line's occurrence count by
        // construction — the other half of I5's self-validation equality.
        m_resimGateProbe.notePrepare(simTick);
        m_clientClock->startResimulation(simTick);
        m_reconciliation.prepareResimAll(simTick);
        m_integrationLayer.firstResimStepAll(chaosStep);
    }

    // -----------------------------------------------------------------------
    // [og-netcode-v2-input-relay item 42] THE RESIM-GATE PROBE — PHYSICS THREAD.
    //
    // Public because two of its six instruments are fed from the ADAPTER side:
    // `FSimulationManagerAsyncCallback::TriggerRewindIfNeeded_Internal` (I3's
    // `requests`, I4's requested depth) and `::FirstPreResimStep_Internal` (I3's
    // `grants`, I4's `clampedGrants`). Those two hooks straddle the core/adapter
    // boundary but run on the SAME physics thread as every other feeder here, which
    // is the whole reason one object can serve them all with no atomics.
    //
    // ⛔ THIS IS NOT A GENERAL MUTABLE HANDLE. It is a counter sink; nothing it
    // holds is read by any gate, clock, cache or integrator, and calling any of its
    // methods from the game thread would corrupt a window (see the two-object rule
    // in ResimGateProbe.h). The adapter reaches it through two NARROW named
    // passthroughs on ASimulationManagerUImpl, not through this accessor, for the
    // same reason `requestInputDelayIncreaseStall` is narrow.
    //
    // [T53 / task 59] `editResimGateProbe()` STAYS exactly here — it is a
    // production write handle (two adapter-side feeders, above), not a
    // diagnostic read seam, and `getDiagnostics()` / `editDiagnostics()` group
    // read seams only (see `docs/DiagnosticsConventions.md` §2). Only the
    // read-only probe accessor that used to sit beside it moves — see the
    // `Diagnostics` view directly below.
    ResimGateProbe&       editResimGateProbe()       { return m_resimGateProbe; }

    // =======================================================================
    // [og-netcode-v2-input-relay task 59] THE RESIM-GATE PROBE'S DIAGNOSTIC
    // VIEW — RN-9 + amendment, grouped per `docs/DiagnosticsConventions.md`. A
    // sibling ruling groups `SimulationNetSync`'s four probe accessors the
    // same way, in the same task.
    //
    // ⛔ ONLY THIS READ-ONLY ACCESSOR MOVES. `m_resimGateProbe` itself, every
    // `note*` call site that feeds it (this class's own `noteDivergenceCheck`
    // included — private, one production caller every frame, feeds this probe
    // directly) and `editResimGateProbe()` above are production and are
    // UNTOUCHED. See `docs/DiagnosticsConventions.md` §2.
    //
    // ⚠ [task 59 RULING] Unlike the other four probe accessors this task
    // groups, this class's pre-existing read-only accessor had ZERO callers
    // anywhere — no production, no test — before this task. That made the
    // resim gate probe the one
    // instrument in the family whose SHIPPED wiring was unproven, on the
    // family that carried every measurement the `OnDisagreement` ship decision
    // rested on. RULING: wire a test rather than delete it — see
    // `ResimGate.Policy.TheResimGateProbeAccessorObservesTheShippedFeed` in
    // `ResimGatePolicyTest.cpp`, which drives the real `onCheckIsSimilar()`
    // through a duck-typed manager rig and asserts this view's probe observed
    // it. Deleting the accessor because "production never calls it" would have
    // removed the only mechanism that could ever prove this family is wired.
    //
    // Nested class, not a free function: it has the same access to
    // SimulationManager's private members as any other member function — no
    // friend declaration needed, and the view holds nothing but a reference.
    // =======================================================================
    class Diagnostics
    {
    public:
        explicit Diagnostics(const SimulationManager& manager) : m_manager(manager) {}

        const ResimGateProbe& resimGateProbe() const { return m_manager.m_resimGateProbe; }

    private:
        const SimulationManager& m_manager;
    };

    Diagnostics getDiagnostics() const { return Diagnostics(*this); }

    // Tick of the most recently integrated step — authority tick / prediction tick /
    // resim tick, i.e. whichever step integrateAll just ran. Consumed by the manager's
    // post-integrate inbound-hit routing pass to one-shot projectile slots that ended
    // THIS tick. Returns 0 before the first integrate (0 is the reserved pre-sim tick).
    uint32_t currentIntegratedTick() const
    {
        return m_lastStep.has_value() ? m_lastStep->getTick() : 0u;
    }

    // [item 80 / B6] sendCorrectionAll + sendLocalInputToAuthorityAll — the
    // NetSync "publish" surface (SimulationNetSyncPublishConcept), the other
    // half of the NetSync method inventory the tick methods above don't call.
    void onPostSimulationGameThread()
        requires SimulationNetSyncPublishConcept<NetSyncT>
    {
        const SimulationTimeStep step = currentStep();
        // [T39] The state-rotation width — how many characters' correction buffers
        // are written this tick. Read live from TimeConfig for the same reason the
        // redundancy depth below is: it is the configured session value, and
        // caching it here would make an ini-driven setting silently ineffective.
        m_netSync.sendCorrectionAll(step, m_timeConfig.correctionRotationK);
        // redundancy depth tracks the runtime tick rate via
        // TimeConfig::redundancyDepthTicks (5 @ 100 Hz interim / 3 @ 60 Hz target).
        m_netSync.sendLocalInputToAuthorityAll(
            step.getTick(), static_cast<uint32>(m_timeConfig.redundancyDepthTicks));
    }

    // -----------------------------------------------------------------------
    // Character-lifecycle notifications — forwarded to the systems executor so
    // each system can maintain its own per-character bookkeeping (e.g. the hit-
    // routing system's rootBodyId map). Called by the engine adapter on
    // register/unregister, out of band from the integrate loop. The storage +
    // static data are supplied from the integration layer so a system's hook
    // can read the just-(un)registered character out of storage (§3.11 timing).
    // -----------------------------------------------------------------------

    void notifyCharacterRegistered(unsigned int id)
    {
        m_systemsExec.notifyCharacterRegistered(id, m_storage, m_staticData);
    }

    void notifyCharacterUnregistered(unsigned int id)
    {
        m_systemsExec.notifyCharacterUnregistered(id, m_storage, m_staticData);
    }

private:
    // -----------------------------------------------------------------------
    // [og-netcode-v2-input-relay item 42] THE PER-WINDOW FLUSH — three Warning
    // lines on LogOGResimProbe, emitted only when a window closed.
    //
    // ⚠ WARNING, NOT Log, AND ON ITS OWN CATEGORY. This initiative has lost FOUR
    // instruments to verbosity — item 36's `Log`-level line invisible on a
    // dedicated server, T33's `[RelayDepth]`, T35's proof line, and item 31's own
    // missing denominator `[ResimCheck.IsSimilar]`, which has zero occurrences in
    // every log on disk because it emits at `Log` under a category that ships at
    // `Warning`. A per-window line here that could not be seen at shipped verbosity
    // would repeat that defect on the instrument built to end it.
    //
    // ⛔ AND THE TAG DELIBERATELY DOES NOT BEGIN `[Resim.` OR `[ResimCheck.`.
    // `[Resim.` inherits `LogOGSim=Verbose` (T19's 10 MB defect) and `[ResimCheck.`
    // splits across two categories. `[ResimProbe.` is its own family with its own
    // knob: `LogOGResimProbe=Warning` keeps these three, `=Verbose` adds the
    // per-event detail, `=NoLogging` drops both, none of which disturbs any
    // neighbour.
    //
    // THREE LINES, NOT ONE, and the split is by PIPELINE STAGE so each is readable
    // alone: Gate = did we ask, Chaos = did the engine accept, Apply = did it land.
    // Together with the game thread's two `[ResimProbe.Landing]` lines that is 5
    // Warning lines per window per category, inside item 42's budget of 6.
    void noteDivergenceCheck(bool requestedResim)
    {
        ResimGateWindowSummary window;
        if (!m_resimGateProbe.noteCheck(requestedResim, window))
            return;

        // I1 — the denominator. `checks` always equals the window length (the
        // window is driven by it) and is printed as the denominator of the rate,
        // exactly as `[DivergenceProbe.Window]` prints `samples`.
        // [item 45] `deepSkips` APPENDED, not inserted, so every archived grep that
        // keys on `checks=` / `declined=` / `requested=` still matches and the item
        // 43 baseline stays comparable field-for-field. It counts CHARACTER-FRAMES on
        // which a pending anchor sat deeper than `rollbackWindowTicks` below its
        // character's frontier and was skipped rather than clamped — structurally 0
        // under the shipped legacy policy, which enforces no depth policy at all.
        // A nonzero reading after item 46's flip means anchors are being stranded and
        // is read against `requested`, not against `checks`.
        //
        // [item 57 / RN-6] `survivingAnchors` APPENDED, not inserted, for the same
        // grep-stability reason `deepSkips` was appended rather than inserted: every
        // archived line keying on `checks=`/`declined=`/`requested=`/`deepSkips=`
        // still matches. It is fed from the `[Resim.Finish]` apply edge, not from
        // this call's own `checkDivergenceAll` sweep — see
        // `ResimGateWindowSummary::survivingAnchors` for why it still lands in this
        // window, and why it is on THIS line rather than `[ResimProbe.Apply]`.
        SIMLOG(m_logger,
            "[Warning][ResimProbe.Gate] checks=%u declined=%u requested=%u requestedPerMille=%u "
            "deepSkips=%u survivingAnchors=%u",
            window.checks, window.declined, window.requested, window.requestRatePerMille,
            window.deepAnchorExclusions, window.survivingAnchors);

        // I3 + I4 — the second gate. `refused` is trustworthy as a refusal count
        // because a refusal leaves needsResimulation() true and the request repeats
        // (finding §4a — and [item 45] the property SURVIVES the redesign by a
        // stronger argument: a refusal consumes nothing, so the pending anchor is
        // still pending on the next frame. It is now impossible for a refusal to
        // silently clear the gate, where previously it relied on no bit having
        // moved); `repeat` is that run's own signature; `clamped` counts
        // requested-vs-granted mismatches, which on this wiring can only mean the
        // grant was DEEPENED (engine-side FMath::Min requester, or validation
        // walking down) — a shallow clamp is structurally impossible (downward
        // walk + Min merge + PhysicsStep == ResimStep; item 42 review §2), so it
        // reads a constant 0 live and a nonzero is an engine-behaviour-change
        // alarm. See ResimGateWindowSummary::clampedGrants.
        SIMLOG(m_logger,
            "[Warning][ResimProbe.Chaos] requests=%u grants=%u refused=%u refusedPerMille=%u "
            "repeat=%u clamped=%u depthMin=%u depthMax=%u",
            window.requests, window.grants, window.refusedFrames, window.refusedRatePerMille,
            window.repeatRequests, window.clampedGrants,
            window.minRequestDepth, window.maxRequestDepth);

        // I5 + I6 — the apply edge and the replay span. Read `stuck` in preference
        // to `abandoned` on any single window: `abandoned` carries a ±1 boundary
        // term (see ResimGateWindowSummary::abandoned) and `stuck` does not.
        SIMLOG(m_logger,
            "[Warning][ResimProbe.Apply] prepares=%u finishes=%u abandoned=%u stuck=%u "
            "replayTicks=%u replayOverruns=%u freshProtects=%u staleProtects=%u",
            window.prepares, window.finishes, window.abandoned, window.stuckResimFrames,
            window.replayTicks, window.replayOverruns,
            // [item 47] The hollow-anchor ledger, on the EXISTING line rather than
            // a new one. `freshProtects` is the live defect rate (near-0 under the
            // shipped `FrontierExact`, a rate after item 46's flip);
            // `staleProtects` MUST read 0 in any single-character session — see
            // ResimGateWindowSummary for both readings.
            window.freshClobbersAvoided, window.staleClobbersAvoided);
    }

    // [item 80 / B6, re-targeted item 87] Constrained on the resolution
    // peer's tick surface — the group this method, onGameSimulationAuthority
    // and onGameSimulationResimulation all draw from (prepareSimulationStep
    // here, named collectInputAll before item 90's rename; NetSyncT no
    // longer has one).
    void onGameSimulationPrediction()
        requires SimulationInputResolutionTickConcept<InputResolutionT>
    {
        // [item 42 / I5] THE STRANDED-CURSOR OBSERVATION, taken BEFORE
        // advancePrediction so it reports the state this frame INHERITED rather
        // than one this frame created. A normal prediction frame entered while the
        // clock still believes it is resimulating means the previous resim's apply
        // edge never ran — finding §4b, ~20 % of granted resims in every archived
        // run. Its side effect is real and measurable: `currentStep()` keeps
        // returning the stale resim step on the game thread, so
        // `sendLocalInputToAuthorityAll` stamps a stale tick until the next
        // `startResimulation` resets the cursor.
        //
        // The Verbose line is ONE-SHOT PER EPISODE (the probe gates it), not per
        // stuck frame: the stuck state can persist for many consecutive frames by
        // construction, and a per-frame line here is exactly the volume class T19
        // was filed to stop.
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

        // [og-netcode-v2-input-relay item 84] Capture completes the frontier
        // pair opened by collect — see the frontier-pair contract
        // (Reconciliation::pushPredictionTick's header; CorrectionCache.h's
        // m_frontierSlotAwaitingState). Capture itself runs later, from
        // onPostGameSimulation's non-resim branch.
        // [item 87] Re-targeted off `m_netSync` onto the resolution peer,
        // which now owns prepareSimulationStep (named collectInputAll before
        // item 90's rename).
        auto inputs = m_inputResolution.prepareSimulationStep(step);
        // Sequencing: fire preIntegrate BEFORE integrateAll; save m_lastStep
        // (so currentIntegratedTick() == step.getTick() for postIntegrate hooks)
        // BEFORE firing postIntegrate. See §7 "Sequencing consideration".
        auto&       storage    = m_storage;
        const auto& staticData = m_staticData;
        m_systemsExec.firePreIntegrate(step, storage, staticData);
        m_integrationLayer.integrateAll(step, inputs);
        m_lastStep = step;
        m_systemsExec.firePostIntegrate(step, storage, staticData);
    }

    // [item 80 / B6, re-targeted item 87] setAuthorityGuardContext is now the
    // sole member of NetSyncT's shrunk tick surface
    // (SimulationNetSyncTickConcept); prepareSimulationStep (named
    // collectInputAll before item 90's rename) moved to the resolution
    // peer's tick surface (SimulationInputResolutionTickConcept) — this
    // method is the one step function that needs BOTH.
    void onGameSimulationAuthority()
        requires SimulationNetSyncTickConcept<NetSyncT> &&
                 SimulationInputResolutionTickConcept<InputResolutionT>
    {
        m_serverClock->advanceTick();
        const SimulationTimeStep step = m_serverClock->getSimulationStep();
        SIMLOG(m_logger, "[AuthoritySimulation] tick=%u", step.getTick());
        // publish the current authority tick + rollback window so the
        // RPC-arrival queueMove path can reject too-far-future capture ticks. The window
        // is TimeConfig::rollbackWindowTicks (no hardcoded literal).
        m_netSync.setAuthorityGuardContext(step.getTick(), m_timeConfig.rollbackWindowTicks);
        // [og-netcode-v2-input-relay item 84, re-scoped item 90] Unlike
        // onGameSimulationPrediction, this collect call does NOT open the
        // frontier-pair contract (see Reconciliation::pushPredictionTick's
        // header): the server `registerSimulatable` overload allocates no
        // correction cache at all (registerPredictionOwner is called with a
        // null provider, so collectInputForCharacter's provider-present
        // branch never matches here; every FULLY-REGISTERED authority id
        // lands in the remote-queue branch instead). [item 90] Sweep 2
        // (`allocateFrontierSlotForCharacter`) makes the same fact explicit
        // from the other side: it looks every id up in the remote-move-queue
        // map first and returns immediately on a hit.
        // [item 92] ⚠ THIS IS NOT AN UNCONDITIONAL "the whole sweep is a
        // no-op" CLAIM — item 90's original wording overstated it as one, and
        // a crash proved the overstatement false. It holds only once
        // registration has completed for a given id: between
        // `storage.add` exposing the id to `forEachSimulatable` and
        // `registerAuthorityOwner` populating `queueMap` (the server
        // overload's ordering invariant, SimulationNetSync.h), that id has
        // NEITHER a queueMap entry NOR a correction cache, sweep 2's guard
        // does not skip it, and it would fall through to a cache lookup that
        // throws. The ordering fix closes the window; the loud-failure guard
        // in `allocateFrontierSlotForCharacter` is the independent backstop
        // if it ever reopens. Capture (onPostGameSimulation's postPredictionAll
        // call) is correspondingly never reached on this role
        // (`m_runsPrediction == false`) for any id whose registration HAS
        // completed — collect and capture agree on authority by both being
        // absent, not by one completing the other.
        // [item 87] Re-targeted off `m_netSync` onto the resolution peer.
        // [item 90] `collectInputAll` -> `prepareSimulationStep`.
        auto inputs = m_inputResolution.prepareSimulationStep(step);
        // Sequencing (see onGameSimulationPrediction).
        auto&       storage    = m_storage;
        const auto& staticData = m_staticData;
        m_systemsExec.firePreIntegrate(step, storage, staticData);
        m_integrationLayer.integrateAll(step, inputs);
        m_lastStep = step;
        m_systemsExec.firePostIntegrate(step, storage, staticData);
    }

    // [item 80 / B6, re-targeted item 87] collectResimInputAll — the third
    // member of the resolution peer's tick surface
    // (SimulationInputResolutionTickConcept); NetSyncT contributes nothing to
    // this step.
    void onGameSimulationResimulation()
        requires SimulationInputResolutionTickConcept<InputResolutionT>
    {
        m_clientClock->advanceResimulation();
        const SimulationTimeStep step = m_clientClock->getResimulationStep();
        SIMLOG(m_logger, "[Resim.Pre] tick=%u", step.getTick());
        // [item 42 / I6] One replay tick. Counted at the same edge the existing
        // `[Resim.Pre]` line reports — the pairing item 31 step 0 derived by hand
        // (69 replay ticks over 41 passes, mean span ~1.7) and which this makes
        // permanent. `replayTicks / finishes` is the mean span; a value in the
        // 12-20 range would put rollback-window coalescing back on the table.
        m_resimGateProbe.noteReplayTick();
        // [og-netcode-v2-input-relay T6, re-targeted item 87] The resolution
        // peer, not reconciliation. The resim input is resolved from the
        // delay lines / relay stores / neutrals the resolution peer owns;
        // reconciliation now contributes only the per-tick applied-capture
        // reference, which the resolution peer asks it for internally.
        // [og-netcode-v2-input-relay item 84] This collect call does NOT open
        // the frontier-pair contract either — collectResimInputAll resolves
        // input against slots that already exist (getAppliedCaptureTickRef)
        // and never calls pushPredictionTick. A resim tick's state write
        // (postResimulationAll -> tryInsertingResimulatedState, called from
        // onPostGameSimulation's resim branch) is a REPLAY over an already-
        // allocated slot, not the pair's completing half — see the frontier-
        // pair contract at Reconciliation::pushPredictionTick's header for
        // what that half actually is.
        auto inputs = m_inputResolution.collectResimInputAll(step.getTick());
        // Sequencing (see onGameSimulationPrediction). Systems fire on every
        // resim replay tick too — routing stays deterministic across rollback (D4).
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
    // [item 87 / design §A.3] The resolution peer — a real
    // composition-root-constructed sibling, not owned by NetSyncT. See
    // SimulationInputResolutionTickConcept above for what the manager reads
    // from it and why.
    InputResolutionT&  m_inputResolution;
    ReconciliationT&   m_reconciliation;
    SystemsExecT&      m_systemsExec;

    // Externally-owned object storage + static data (owned at the engine adapter's
    // composition root, shared by all peers). Held by reference here so the
    // manager's system-hook fan-out and lifecycle forwarders read/mutate the same
    // storage the integration executor iterates — no bridge accessor.
    StorageT&          m_storage;
    const StaticDataT& m_staticData;

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

    // [og-netcode-v2-input-relay item 42] THE RESIM-GATE PROBE. PHYSICS THREAD
    // ONLY, and unconditional: it is constructed on the authority too, where
    // `onCheckIsSimilar` is never reached (the adapter short-circuits on
    // !runsPrediction) so it never advances and never flushes. That is why the
    // "zero new lines on the authority" criterion needs no role test anywhere —
    // the counters simply have no feeder there. Sixteen `uint32`s and four flags;
    // guarding its construction on the role would buy nothing and would add a
    // second place for the role to be got wrong.
    ResimGateProbe                       m_resimGateProbe;
};

// ---------------------------------------------------------------------------
// [og-netcode-v2-input-relay item 80 / B6, extended item 87] COMPILE-CHECKED
// PROOF that the concept vocabulary applied above actually rejects a
// same-category transposition — the exact scenario `Params`' own comment
// (top of this class) says compiles today for a duck-typed, unconstrained
// peer reference: "a transposition of two same-category refs COMPILES —
// they are duck-typed — and misbehaves only at runtime."
//
// Minimal local types, each shaped like exactly one peer. Three of them —
// NetSync, InputResolution, Reconciliation — now share `wipeAllForResync`
// verbatim (item 87 added the resolution peer as a third sharer; NetSync's
// own copy of the method left with the other two collect* members it moved
// off — see `SimulationNetSyncTickConcept`'s own history comment). A
// wipe-only clause could not distinguish any of the three from another; the
// DISTINGUISHING members are `collect*` (resolution), `postPredictionAll` /
// `consumeResimAnchorsAll` (reconciliation) and `sendCorrectionAll`
// (netsync) — exactly what the positive/negative controls below check.
// Swapping a shaped type into another concept's slot is the transposition
// itself, and it is now a hard compile-time rejection rather than a silent
// duck-typed pass-through.
// ---------------------------------------------------------------------------
namespace simulationManagerConceptProof
{
    // Shaped like a NetSync peer. Satisfies every SimulationNetSyncConcept
    // member — [item 87] `collectInputAll` (RENAMED `prepareSimulationStep`
    // at item 90) / `collectResimInputAll` / `wipeAllForResync` left this
    // shape along with the concept; what remains is transport + the
    // receive-side guard.
    struct NetSyncShaped
    {
        void sendCorrectionAll(const SimulationTimeStep&, int32 /*correctionRotationK*/) {}
        void sendLocalInputToAuthorityAll(uint32 /*tick*/, uint32 /*redundancyDepth*/) {}
        void setAuthorityGuardContext(uint32 /*tick*/, int32 /*rollbackWindow*/) {}
    };

    // [item 87] Shaped like the resolution peer. Satisfies every
    // SimulationInputResolutionTickConcept member — exactly the three
    // methods that LEFT NetSyncShaped above for this shape.
    struct InputResolutionShaped
    {
        int  prepareSimulationStep(const SimulationTimeStep&) { return 0; }
        int  collectResimInputAll(uint32 /*tick*/) { return 0; }
        void wipeAllForResync(uint32 /*tick*/) {}
    };

    // Shaped like a Reconciliation peer. Satisfies every
    // SimulationReconciliationConcept member.
    struct ReconciliationShaped
    {
        void postPredictionAll(const SimulationTimeStep&) {}
        void postResimulationAll(const SimulationTimeStep&) {}
        unsigned int checkDivergenceAll(uint32 /*tick*/) { return 0u; }
        void wipeAllForResync(uint32 /*tick*/) {}
        void prepareResimAll(uint32 /*tick*/) {}
        void applyResimAll() {}
        unsigned int consumeResimAnchorsAll() { return 0u; }
        void setResimTriggerPolicy(TimeConfig::ResimTriggerPolicy) {}
    };

    // [item 83 / 80 f3] Shaped like an IntegrationExec peer. Satisfies every
    // SimulationIntegrationExecutorConcept member. Added to LEVEL the proof
    // coverage: unlike the NetSync/Reconciliation pair above,
    // SimulationIntegrationExecutorConcept previously got no positive shaped
    // control and no transposition negative in this block — only the Empty-type
    // negative below — even though NetSyncShaped/ReconciliationShaped would both
    // (correctly) fail it, a claim nothing here proved before this addition.
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

    // --- NEGATIVE CONTROLS — THE TRANSPOSITION. A shaped ref used where a
    // different peer's typed slot is expected. This is the failure the
    // Params aggregate exists to guard against structurally; these lines are
    // the concept vocabulary catching it at compile time instead. ---
    static_assert(!SimulationReconciliationConcept<NetSyncShaped>,
        "a NetSync-shaped type must NOT satisfy SimulationReconciliationConcept"
        " -- this is the same-category transposition Params exists to guard");
    static_assert(!SimulationNetSyncTickConcept<ReconciliationShaped>,
        "a Reconciliation-shaped type must NOT satisfy the manager's NetSync"
        " tick surface -- the transposition's other direction");
    static_assert(!SimulationNetSyncPublishConcept<ReconciliationShaped>,
        "a Reconciliation-shaped type must NOT satisfy the manager's NetSync"
        " publish surface either");

    // [item 87] THE RESOLUTION PEER'S TRANSPOSITION NEGATIVES, BOTH
    // DIRECTIONS — against BOTH the NetSync- and Reconciliation-shaped
    // types, exactly the two the shared `wipeAllForResync` name could be
    // mistaken for distinguishing.
    static_assert(!SimulationInputResolutionTickConcept<NetSyncShaped>,
        "a NetSync-shaped type must NOT satisfy the manager's resolution"
        " tick surface -- it no longer has prepareSimulationStep/collectResimInputAll");
    static_assert(!SimulationInputResolutionTickConcept<ReconciliationShaped>,
        "a Reconciliation-shaped type must NOT satisfy the manager's"
        " resolution tick surface -- sharing wipeAllForResync distinguishes"
        " nothing; it still lacks prepareSimulationStep/collectResimInputAll");
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

    // [item 83 / 80 f3, extended item 87] THE COVERAGE GAP THE REVIEW NAMED,
    // CLOSED: none of the three peer shapes above has
    // `firstResimStepAll`/`captureBodyStatesAll`, so all three would fail
    // SimulationIntegrationExecutorConcept — asserted here instead of left
    // as an unproven claim.
    static_assert(!SimulationIntegrationExecutorConcept<NetSyncShaped>,
        "a NetSync-shaped type must NOT satisfy"
        " SimulationIntegrationExecutorConcept");
    static_assert(!SimulationIntegrationExecutorConcept<InputResolutionShaped>,
        "a resolution-shaped type must NOT satisfy"
        " SimulationIntegrationExecutorConcept");
    static_assert(!SimulationIntegrationExecutorConcept<ReconciliationShaped>,
        "a Reconciliation-shaped type must NOT satisfy"
        " SimulationIntegrationExecutorConcept");

    // A plain nonconforming type (no members at all) satisfies none of the
    // four constrained concepts, including the ones with no SimulatableTs
    // wrinkle to work around.
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
