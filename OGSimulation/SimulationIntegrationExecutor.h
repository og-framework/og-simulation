#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <tuple>
#include <unordered_map>
#include "OGSimulation/PhysicsBodyAdapter.h"
#include "OGSimulation/SpatialQueryAdapter.h"
#include "OGSimulation/SimulationLog.h"
#include "OGSimulation/SimulationObjectStorage.h"
#include "OGSimulation/SimulationTimeContext.h"

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// Adapter-dependent side of a simulatable: integrate() and firstResimStep().
// Required by SimulationIntegrationExecutor.
template <typename T, typename PhysAdapterT, typename QueryAdapterT, typename StaticDataT>
concept SimulatableIntegration =
    PhysicsBodyAdapter<PhysAdapterT> &&
    SpatialQueryAdapter<QueryAdapterT> &&
    requires(
        T& t,
        PhysAdapterT& phys,
        QueryAdapterT& query,
        const SimulationTimeStep& step,
        const typename T::InputType& input,
        const StaticDataT& sd,
        int32_t physStep)
    {
        typename T::InputType;
        { t.integrate(step, input, phys, query, sd) };
        { t.firstResimStep(phys, physStep) };
    };

// Storage and StaticData are EXTERNALLY OWNED (by the engine adapter's
// composition root); the executor holds only references to them. This makes the
// object storage a genuinely shared resource across all simulation peers
// (integration executor, net-sync, reconciliation, systems executor) rather than
// being owned by one peer and bridged to the others via accessors. See the
// adapter's m_staticData declaration for the StaticData internal-reference
// invariant that mandates the const& (never by-value) handling.
template <
    typename StaticDataT,
    PhysicsBodyAdapter PhysAdapterT,
    SpatialQueryAdapter QueryAdapterT,
    typename... SimulatableTs>
    requires (SimulatableIntegration<SimulatableTs, PhysAdapterT, QueryAdapterT, StaticDataT> && ...)
class SimulationIntegrationExecutor
{
public:
    // storage and staticData are externally owned; the executor stores references.
    // staticData MUST be constructed in place at its owner and never copied — its
    // nested fields hold references bound to its own members (see the adapter's
    // m_staticData doc-comment), so the const& binding is load-bearing, not
    // stylistic.
    SimulationIntegrationExecutor(
        SimulationObjectStorage<SimulatableTs...>& storage,
        const StaticDataT& staticData,
        PhysAdapterT& physAdapter,
        QueryAdapterT& queryAdapter)
        : m_storage(storage)
        , m_staticData(staticData)
        , m_physicsAdapter(physAdapter)
        , m_queryAdapter(queryAdapter)
    {}

    using ResolvedInputsType = ResolvedInputs<SimulatableTs...>;

    void integrateAll(const SimulationTimeStep& step, const ResolvedInputsType& inputs)
    {
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using SimulatableT = std::remove_reference_t<decltype(simulatable)>;
            const auto& map = std::get<
                std::unordered_map<unsigned int, typename SimulatableT::InputType>>(inputs);
            if (auto it = map.find(id); it != map.end())
            {
                // The ONE set site of the (id, tick) log context: see SimulationLog.h.
                // Never around firstResimStep, body-state capture or systems.
                simulationLog::IntegrateScope logScope(id, step.getTick());
                simulatable.integrate(step, it->second, m_physicsAdapter, m_queryAdapter, m_staticData);
            }
        });
    }

    void firstResimStepAll(int32_t physicsStep)
    {
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            simulatable.firstResimStep(m_physicsAdapter, physicsStep);
        });
    }

    // Writes each declaration's body state from og-sim state into the physics world, after a
    // rollback restored og-sim state from the correction cache. Called once per resim by
    // `SimulationManager::prepareResimulation`, after the host restored its physics world to the
    // anchor and after og-sim state was restored from the correction cache. Position and linear
    // velocity always; rotation and angular velocity only for a full `PhysicsBodyState`
    // declaration, because a `LinearBodyState` carries neither and its widening would write
    // identity and zero.
    //
    // THE RESTORED BODY is read with `captureStartOfStepBodyState` when the adapter models
    // `PhysicsBodyStartOfStepReader` (`if constexpr`), and with `captureBodyState` otherwise. It
    // must be the state the first replayed step starts from. On Jolt that is `captureBodyState`
    // after `restoreTick`. On Chaos `captureBodyState` reads the solved P/Q, which a rewind leaves
    // stale while it restores X/R/V/W, so the Chaos adapter supplies the start-of-step read. Both
    // reads below (the skip and a linear declaration's rotation) use this one value.
    //
    // RESTORE SKIP. A declaration whose state equals the adapter's capture of the restored body
    // (exact `operator==`: all four fields for `PhysicsBodyState`, position and linear velocity for
    // `LinearBodyState`) is NOT written. For a declaration nobody corrected, the cached anchor state
    // IS the capture the snapshot was saved with, so the skip fires exactly when there is nothing
    // to restore; an adopted correction still differs and is still written. Load-bearing for an
    // adapter whose seam write is lossy (Jolt's was, while the simulation ran in centimetres):
    // writing an unchanged body would move it by an ULP, the next correction would disagree, and
    // a torque-driven body (the weapon) would keep that error and start a resim storm. Jolt's write
    // of a shipped body is exact since the metre move and the origin-at-COM check. The guard lives
    // HERE, not in the adapter setters: the sims rewrite bodies from seam state every tick, and any
    // quantization must stay identical on every peer. The skip runs on every backend: for an exact
    // adapter it saves a write that changes nothing og-sim reads, so no "lossless" flag exists.
    //
    // POSE PATH. When the adapter models `PhysicsBodyPoseWriter`, the pose goes through
    // `setBodyPose` (position + quaternion), and a `LinearBodyState` declaration passes back the
    // body's own captured rotation. Otherwise the matrix path below is kept. Load-bearing: the
    // matrix path (`mat4_cast`, then the adapter's matrix -> quaternion) loses rotation bits, so an
    // adopted rotation would not restore exactly.
    void pushCorrectedBodyStatesAll()
    {
        m_storage.forEachSimulatable([&](unsigned int /*id*/, auto& sim) {
            const auto& state = sim.getAllState().getState();
            sim.getPhysicsComposite().forEach([&](const auto& decl) {
                using D = std::decay_t<decltype(decl)>;
                using S = typename D::StateType;
                using BodyStateT = std::remove_cvref_t<decltype(D::bodyStateOf(state.template get<S>()))>;
                const PhysicsBodyState body = static_cast<PhysicsBodyState>(D::bodyStateOf(state.template get<S>()));
                const BodyId bodyId = decl.bindings.ownBodyId;
                const PhysicsBodyState restored = captureRestoredBodyState(bodyId);
                // Restore skip, kept by the 2026-10-25 ruling (og-sim-metres-yup Q3): it saves a pose write, and on Jolt a broadphase update, for every body nobody corrected (og-simulation-jolt docs/JoltPhysicsBodyAdapter-rationale.md §9).
                if (bodyStateEqualsRestored<BodyStateT>(body, restored))
                {
                    return;
                }
                if constexpr (std::is_same_v<BodyStateT, PhysicsBodyState>)
                {
                    if constexpr (PhysicsBodyPoseWriter<PhysAdapterT>)
                    {
                        m_physicsAdapter.setBodyPose(bodyId, body.position, body.rotation);
                    }
                    else
                    {
                        glm::mat4 transform = glm::mat4_cast(body.rotation);
                        transform[3] = glm::vec4(body.position, 1.f);
                        m_physicsAdapter.setBodyTransform(bodyId, transform);
                    }
                    m_physicsAdapter.setBodyLinearVelocity(bodyId, body.linearVelocity);
                    m_physicsAdapter.setBodyAngularVelocity(bodyId, body.angularVelocity);
                }
                else
                {
                    if constexpr (PhysicsBodyPoseWriter<PhysAdapterT>)
                    {
                        m_physicsAdapter.setBodyPose(bodyId, body.position, restored.rotation);
                    }
                    else
                    {
                        glm::mat4 transform = m_physicsAdapter.getBodyTransform(bodyId);
                        transform[3] = glm::vec4(body.position, 1.f);
                        m_physicsAdapter.setBodyTransform(bodyId, transform);
                    }
                    m_physicsAdapter.setBodyLinearVelocity(bodyId, body.linearVelocity);
                }
            });
        });
    }

    void captureBodyStatesAll()
    {
        m_storage.forEachSimulatable([&](unsigned int /*id*/, auto& sim) {
            sim.editPhysicsComposite().forEach([&](auto& decl) {
                using D = std::decay_t<decltype(decl)>;
                using S = typename D::StateType;
                static_assert(
                    BodyStateLike<std::remove_cvref_t<decltype(
                        D::bodyStateOf(sim.editAllState().editState().template edit<S>()))>>,
                    "PhysicsDeclaration::bodyStateOf must return a BodyStateLike& — see PhysicsBodyState.h");
                D::bodyStateOf(sim.editAllState().editState().template edit<S>()) =
                    m_physicsAdapter.captureBodyState(decl.bindings.ownBodyId);
            });
        });
    }

private:
    // The restored body the push compares with and takes a linear declaration's rotation from (see
    // pushCorrectedBodyStatesAll).
    PhysicsBodyState captureRestoredBodyState(BodyId bodyId) const
    {
        if constexpr (PhysicsBodyStartOfStepReader<PhysAdapterT>)
        {
            return m_physicsAdapter.captureStartOfStepBodyState(bodyId);
        }
        else
        {
            return m_physicsAdapter.captureBodyState(bodyId);
        }
    }

    // The restore skip's comparison (see pushCorrectedBodyStatesAll). Exact float equality on the
    // fields the declaration's wire shape carries: a `LinearBodyState` has no rotation or angular
    // velocity, so the body's own values for those never decide.
    template <typename BodyStateT>
    static bool bodyStateEqualsRestored(const PhysicsBodyState& body, const PhysicsBodyState& restored)
    {
        if constexpr (std::is_same_v<BodyStateT, PhysicsBodyState>)
        {
            return body.position == restored.position && body.rotation == restored.rotation
                && body.linearVelocity == restored.linearVelocity && body.angularVelocity == restored.angularVelocity;
        }
        else
        {
            return body.position == restored.position && body.linearVelocity == restored.linearVelocity;
        }
    }

    // Externally-owned references (see class + ctor doc-comments). Declaration
    // order matches the ctor init-list to keep -Wreorder clean.
    SimulationObjectStorage<SimulatableTs...>& m_storage;
    const StaticDataT&                         m_staticData;
    PhysAdapterT&                              m_physicsAdapter;
    QueryAdapterT&                             m_queryAdapter;
};

// Concept for types that implement the SimulationIntegrationExecutor interface.
// Lives here, beside the class, so the composition root that owns an executor as a
// member can static_assert that member against it. One adapter does exactly that,
// in `OGBrawlerSimulationManagerConceptTest.cpp`.
template <typename T>
concept SimulationIntegrationExecutorConcept = requires(
    T& t, const SimulationTimeStep& step, int32_t physStep)
{
    { t.firstResimStepAll(physStep) };
    { t.captureBodyStatesAll() };
    { t.pushCorrectedBodyStatesAll() };
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
