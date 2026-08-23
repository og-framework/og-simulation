#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include <atomic>
#include <concepts>
#include <functional>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_map>

#include "OGSimulation/CorrectionStateBufferCodec.h"
#include "OGSimulation/Network/LocalInputCache.h"
#include "OGSimulation/Network/CorrectionRotation.h"
#include "OGSimulation/Network/CorrectionVerdictProbe.h"
// Game-thread half of the resim-gate telemetry, fed from the same
// OnRep-bound callback as the verdict probe.
#include "OGSimulation/ResimGateProbe.h"
#include "OGSimulation/Network/RelayReadProbe.h"
#include "OGSimulation/Network/RemoteInputCache.h"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/SimulationInputResolution.h"
#include "OGSimulation/NetSyncTelemetry.h"
#include "OGSimulation/SimulationLog.h"
#include "OGSimulation/SimulationObjectStorage.h"
#include "OGSimulation/SimulationQueues.h"
#include "OGSimulation/SimulationReconciliation.h"
#include "OGSimulation/SimulationTimeContext.h"

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// SimulatableOwnerTraits<SimulatableT>
//
// ⛔ INTENTIONALLY UNDEFINED — a missing specialization must be a clean
// compile error. Each declares PredictionOwnerType + AuthorityOwnerType. §1
template <typename SimulatableT>
struct SimulatableOwnerTraits;

template <typename T>
using PredictionOwnerFor = typename SimulatableOwnerTraits<T>::PredictionOwnerType;

template <typename T>
using AuthorityOwnerFor = typename SimulatableOwnerTraits<T>::AuthorityOwnerType;

// ⛔ ONE owner pointer: no std::function, no per-id heap allocation. Per-tick
// state is looked up by id in the call body, NEVER captured here. §1
// ⛔ sizeof MUST equal sizeof(void*) — held by a static_assert in the test files. §1

template <typename T>
struct AuthorityWriter
{
    AuthorityOwnerFor<T>* owner;
};

template <typename T>
struct LocalInputSender
{
    PredictionOwnerFor<T>* owner;
};

// ⛔ THE FIVE INPUT CONTAINER FAMILIES ARE NOT HERE, nor the provider map, the
// join-key map or the ladder — all on SimulationInputResolution.h.
// What stays: these two maps, the owner concepts, SimulatableOwnerTraits and the
// registration bindings. Full roster in ORIENTATION below. §1

template <typename T>
using AuthorityWriterMapFor = std::unordered_map<unsigned int, AuthorityWriter<T>>;

template <typename T>
using LocalInputSenderMapFor = std::unordered_map<unsigned int, LocalInputSender<T>>;

// Concept helpers, referenced by the concepts below.
//
// ⛔ THE WRITE/READ MIRROR PAIR — breaking write() or readInto() must be a compile
// error at the registerSimulatable call site, never runtime wire corruption. Read
// side: SimulationReconciliation::injectCorrectionState + registerAuthorityOwner. §2

template <typename BufferT, typename CompositeT>
concept CompositeSyncedBufferConcept =
    requires(std::remove_reference_t<BufferT>& b,
             const std::remove_reference_t<BufferT>& cb,
             const CompositeT& in,
             CompositeT& out,
             uint32 tick)
    {
        { b.write(in, tick) };
        { cb.readInto(out) } -> std::same_as<uint32_t>;
    };

// ⛔ THE applied-capture-tick ref REFINES this contract, it does not extend it —
// requiring it on every tick-stamped buffer puts a meaningless field on the
// input buffer. §2
//
// ⛔ Both halves in one place: writer sendCorrectionAll and reader
// injectCorrectionState must stay in lockstep. §2
template <typename BufferT, typename StateT>
concept CorrectionStateSyncedBufferConcept =
    CompositeSyncedBufferConcept<BufferT, StateT> &&
    requires(std::remove_reference_t<BufferT>& b,
             const std::remove_reference_t<BufferT>& cb,
             const StateT& in,
             uint32 tick,
             uint32 appliedCaptureTick)
    {
        // ⛔ ONE call — so a publish can never pair this tick's state with the
        // previous tick's ref. §2
        { b.write(in, tick, appliedCaptureTick) };
        { cb.getAppliedCaptureTick() } -> std::same_as<uint32>;
    };

// ⛔ BOTH HALVES REQUIRED — registerPredictionOwner binds the arrival callback AND
// reads the ring at bind. An owner with one fails at the registerSimulatable call
// site instead of leaving a proxy with a dead store. §2
//
// ⛔ FRelayedInputRing is NEVER named in this layer — it arrives as
// OwnerT::RelayedInputRingType, consumed only through the codec's concept. §2
template <typename OwnerT, typename StateT, typename InputT>
concept PredictionSyncedBufferOwnerConcept =
    requires(OwnerT& owner,
             const OwnerT& constOwner,
             std::function<void(const typename OwnerT::SyncedCorrectionBufferType&)> corrFn,
             std::function<void(const typename OwnerT::RelayedInputRingType&)> relayFn,
             const PendingInputQueue<InputT>& pendingQueue,
             uint32 currentTick,
             uint32 redundancyDepth)
    {
        typename OwnerT::SyncedCorrectionBufferType;
        typename OwnerT::SyncedRemoteInputBufferType;
        typename OwnerT::RelayedInputRingType;
        requires CorrectionStateSyncedBufferConcept<typename OwnerT::SyncedCorrectionBufferType, StateT>;
        // ⛔ SyncedRemoteInputBufferType survives the SERVER->CLIENT CHANNEL'S RETIREMENT
        // in its CLIENT->SERVER role only, as getClientToServerInputSyncedBuffer's
        // return. Its server->client role is gone. §3
        requires CompositeSyncedBufferConcept<typename OwnerT::SyncedRemoteInputBufferType, InputT>;
        { owner.setOnCorrectionStateReceivedCallback(corrFn) };
        { owner.clearOnCorrectionStateReceivedCallback() };
        { owner.setOnRelayedInputReceivedCallback(relayFn) };
        { owner.clearOnRelayedInputReceivedCallback() };
        { constOwner.getRelayedInputRing() } -> std::same_as<const typename OwnerT::RelayedInputRingType&>;
        { owner.getClientToServerInputSyncedBuffer() } -> std::same_as<typename OwnerT::SyncedRemoteInputBufferType*>;
        // ⛔ FInputRedundancyBundle NEVER appears in this layer — only the core
        // PendingInputQueue and the redundancyDepth scalar cross this line. §2
        { owner.sendLocalInputToAuthority(pendingQueue, currentTick, redundancyDepth) };
    };

// ⛔ THE AUTHORITY OWNER OWNS NO OUTBOUND INPUT BUFFER AND MUST NOT REGROW ONE.
// getSyncedCorrectionInputBuffer and the SyncedRemoteInputBufferType requirement
// were removed, not left dangling: a demand nothing serves costs every future
// authority owner a member. §3
template <typename OwnerT, typename StateT, typename InputT>
concept AuthoritySyncedBufferOwnerConcept =
    requires(OwnerT& owner,
             std::function<void(uint32, const InputT&)> fn)
    {
        { owner.getSyncedCorrectionStateBuffer() } -> CorrectionStateSyncedBufferConcept<StateT>;
        // ⛔ PER-SLOT, not per-bundle — the owner unpacks the inbound
        // redundancy bundle and invokes this once per (capture_tick, input). §2
        { owner.setOnRemoteMoveReceivedCallback(fn) };
        { owner.clearOnRemoteMoveReceivedCallback() };
    };

// SimulationNetSync<SimulatableTs...>
//
// SimulationNetSync<SimulatableTs...>
//
// Owns the owner-binding maps, the registration bindings and callbacks, both
// outbound transport paths, and the receive-side guard context. Holds refs to
// SimulationObjectStorage, SimulationReconciliation and SimulationInputResolution.
//
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
//
// Relocation history, retired rationale and archived measurement records:
// `docs/SimulationNetSync-rationale.md`.
//
// ---------------------------------------------------------------------------
// ORIENTATION — WHAT THIS CLASS STILL OWNS, WHO CALLS IT, ON WHICH THREAD.
//
// Read this first. Every fence in this file states one invariant at the line
// it guards; none of them restates this map, and this map states no invariant.
//
//   * WHAT NETSYNC OWNS, after four decompositions — this list and no more:
//
//       the two owner-BINDING maps      m_authorityWriters, m_localInputSenders
//       the owner CONCEPTS + traits     SimulatableOwnerTraits and the four
//                                       concepts above
//       the registration BINDINGS       which callback is bound to which owner
//       TRANSPORT, both directions      sendCorrectionAll (state out),
//                                       sendLocalInputToAuthorityAll (input out),
//                                       and the two inbound callbacks
//       the RECEIVE-SIDE GUARD context  m_currentAuthorityTick + the window
//       its GAME-THREAD telemetry       m_telemetry (NetSyncTelemetry)
//
//   * WHERE EVERYTHING ELSE WENT. Reached through a by-id door on the
//     resolution peer; NONE of it is forwarded by this class any more, so a
//     caller wanting any row below must hold the peer, not this object.
//
//       the five input container families   SimulationInputResolution
//       the provider map + the join-key map SimulationInputResolution
//       collectInputAll                     SimulationInputResolution
//       collectResimInputAll                SimulationInputResolution
//       wipeAllForResync                    SimulationInputResolution
//       the RelayReadProbe MEMBER           InputResolutionTelemetry
//       its relayReadProbe() ACCESSOR       SimulationInputResolution::Diagnostics
//       the physics-thread emit* helpers    InputResolutionTelemetry
//       frontier-slot allocation            SimulationReconciliation
//
//     THE ELEVEN BY-ID DOORS this class calls on the peer, at twelve sites,
//     and nothing else — this is the whole coupling:
//
//       registerLocalCharacter   registerRemoteCharacter  registerAuthorityCharacter
//       unregisterCharacter      forgetOwner              hasNeutralInput
//       ingestRelayRing          queueRemoteMove          findPendingInputQueue
//       getLastUsedCaptureTick   isLocallyControlled
//
//   * THREADS. Almost everything here is GAME thread, and the exception is the
//     one that matters:
//
//       GAME, arrival     onRelayedInputReceived, onCorrectionReceived and the
//                         remote-move lambda — OnRep- or RPC-dispatched
//       GAME, per tick    sendCorrectionAll, sendLocalInputToAuthorityAll, from
//                         SimulationManager::onPostSimulationGameThread
//       GAME, lifecycle   registerPredictionOwner, registerAuthorityOwner,
//                         unregisterSimulatable, from the facades below
//       PHYSICS           setAuthorityGuardContext ALONE, from
//                         SimulationManager::onGameSimulationAuthority
//
//     ⚠ THAT LAST ROW IS A REAL CROSSING AND THE ONLY WRITE-SIDE ONE ON THIS
//     CLASS: setAuthorityGuardContext writes m_currentAuthorityTick and
//     m_rollbackWindowTicks on the PHYSICS thread, and the RPC-arrival lambda
//     reads them on the GAME thread. Both members are PLAIN, deliberately —
//     fenced at their own declaration, and inventoried in
//     docs/ThreadingCrossings.md under m_currentAuthorityTick.
//
//     The file's OTHER admitted race is a READ: the post-insert frontier read
//     in decideCorrectionArrival, ±1 per sample and non-accumulating, fenced
//     there, and inventoried in the same doc under decideCorrectionArrival.
//
//   * ROLES — which half of this class is even reachable where:
//
//       AUTHORITY ONLY   sendCorrectionAll, the remote-move callback, the
//                        neutral-injection guard, setAuthorityGuardContext
//       CLIENT ONLY      onCorrectionReceived and onRelayedInputReceived —
//                        both OnRep-dispatched, so neither can fire on a
//                        server world at all
//       BOTH             registration, unregistration, and the guard members
//
//   * CALLBACK LIFECYCLE — what can fire outside a fully-registered window.
//     registerPredictionOwner binds TWO inbound callbacks and makes ONE
//     bind-time read; registerAuthorityOwner binds a third.
//
//       the bind-time ring read   runs INSIDE registration, synchronously. It
//                                 exists precisely to close the not-yet-bound
//                                 window, and no-ops on an unwritten ring.
//       relay-ring arrival        cannot precede its own store: the peer's
//       correction-state arrival  registerRemoteCharacter runs before the bind,
//                                 and both are cleared in unregister STEP 1,
//                                 before any lifecycle erase.
//       remote-move RPC           may fire LATE, after unregistration — safe
//                                 because it routes by id and degrades to a
//                                 lookup miss, never a dangling reference.
//
//   * ⛔ THE TWO ORDERING INVARIANTS THIS FILE EXISTS TO HOLD, both landed to
//     fix live crashes, and both re-checked when frontier-slot allocation
//     moved to SimulationReconciliation:
//     PUBLISH LAST on the way in — storage.add is the last call of either
//     register overload — and UNPUBLISH FIRST on the way out. Stated in full
//     at the two facades at the bottom of this file. §14
//
// The resim CYCLE this class feeds — its phase order, its threads and its call
// sites — is stated once at SimulationReconciliation.h's own orientation block,
// not re-derived here. What actually SHIPS as the resim trigger policy is stated
// once at PCTimeManagement/TimeConfig.h's resimTriggerPolicy field, which keeps
// the compiled default and the ini-shipped value apart, not re-derived here.
// ---------------------------------------------------------------------------

template <typename... SimulatableTs>
class SimulationNetSync
{
public:
    SimulationNetSync(
        SimulationObjectStorage<SimulatableTs...>& storage,
        SimulationReconciliation<SimulatableTs...>& reconciliation,
        SimulationInputResolution<SimulatableTs...>& inputResolution)
        : m_storage(storage)
        , m_reconciliation(reconciliation)
        , m_inputResolution(inputResolution)
    {}

    void setLogger(std::function<void(const char*)> logger)
    {
        // ⛔ m_telemetry is this class's OWN sibling; the peer's InputResolutionTelemetry
        // is seeded via SimulationInputResolution::setLogger. §4
        // ⛔ m_logger STAYS here — the SIMLOG sites outside the sixteen emit* helpers
        // use it directly. §4
        m_telemetry.setLogger(logger);
        m_logger = std::move(logger);
    }

    // ⛔ NO HARDCODED LITERAL HERE — the window is TimeConfig::rollbackWindowTicks. §5
    // ⛔ m_rollbackWindowTicks = -1 DISABLES the future guard, so an unconfigured
    // instance keeps accept-all plus the capture-tick dedup. §5
    void setAuthorityGuardContext(uint32 currentAuthorityTick, int32 rollbackWindowTicks)
    {
        m_currentAuthorityTick = currentAuthorityTick;
        m_rollbackWindowTicks  = rollbackWindowTicks;
    }

    // THE PROBE ACCESSORS' DIAGNOSTIC VIEW. The read-seam-vs-write-site rule these
    // views follow is centralised in `docs/DiagnosticsConventions.md` §2/§3 — not
    // re-derived here.
    //
    // ⛔ THREE READ-ONLY ACCESSORS AND NOTHING ELSE — the probe MEMBERS, the note*
    // sites and the emit* helpers are production instrumentation, not this view's. §4
    // ⛔ relayReadProbe() IS NOT HERE and must not be re-added: it is reached at
    // inputResolution.getDiagnostics().relayReadProbe(). §4
    //
    // ⛔ CONST-ONLY, DELIBERATELY — a mutable handle here invites a physics-thread
    // counter to be incremented from the game thread. §4
    // ⛔ THERE IS NO editDiagnostics() / MutableDiagnostics ON THIS CLASS, unlike
    // StateCorrectionCache's pair. The absence is a ruling, not an omission. §4
    //
    // ⛔ Nested class, not a free function — it already has private access, so the
    // alternative needs a friend declaration and buys nothing. §4
    class Diagnostics
    {
    public:
        explicit Diagnostics(const SimulationNetSync& netSync) : m_netSync(netSync) {}

        const RelayArrivalProbe& relayArrivalProbe() const { return m_netSync.m_telemetry.relayArrivalProbe(); }

        // ⛔ PUBLIC against narrowing — only an og-brawler-tests suite with
        // SimulatableOwnerTraits bound to concrete owners reaches it. §4
        const CorrectionVerdictProbe& correctionVerdictProbe() const
        { return m_netSync.m_telemetry.correctionVerdictProbe(); }

        // ⛔ Same, and the only place a real local plus a real remote character can show a
        // DISCARD reaching the discard bucket rather than the verdict probe's return;
        // the arithmetic is swept in og-simulation-tests. §4
        const CorrectionLandingProbe& correctionLandingProbe() const
        { return m_netSync.m_telemetry.correctionLandingProbe(); }

    private:
        const SimulationNetSync& m_netSync;
    };

    Diagnostics getDiagnostics() const { return Diagnostics(*this); }

    // Registration — free functions delegate here; not called directly by users

    // Client overload helper — called from the registerSimulatable free function.
    //
    // ⛔ THE CROSS-PEER SET INVARIANT — sender-map keys are a SUBSET of the peer's
    // provider set, maintained SOLELY here and in registerAuthorityOwner, one fixed
    // sequence. Enforced loudly at sendLocalInputToAuthorityAll
    // (findPendingInputQueue + OG_CHECK). §5
    template <typename SimulatableT>
        requires PredictionSyncedBufferOwnerConcept<
            PredictionOwnerFor<SimulatableT>,
            typename SimulatableT::StateType,
            typename SimulatableT::InputType>
    void registerPredictionOwner(
        unsigned int id,
        PredictionOwnerFor<SimulatableT>& predictionOwner,
        std::function<typename SimulatableT::InputType(
            const SimulationTimeStep&,
            const LocalInputCache<typename SimulatableT::InputType>&)> inputProvider,
        SimulationInputResolution<SimulatableTs...>& inputResolution)
    {
        // ⛔ PROVIDER PRESENCE IS THE IDENTITY TEST — present iff this owner drives a
        // locally-controlled simulatable. Not a convenience null-check. §5
        if (inputProvider)
        {
            inputResolution.template registerLocalCharacter<SimulatableT>(id, std::move(inputProvider));

            std::get<LocalInputSenderMapFor<SimulatableT>>(m_localInputSenders)
                .emplace(id, LocalInputSender<SimulatableT>{ &predictionOwner });
        }
        else
        {
            // ⛔ REMOTE branch — registerRemoteCharacter creates the neutral-seeded store on
            // the peer; this class binds both callbacks BY ID ONLY, never a captured
            // container reference. §5
            inputResolution.template registerRemoteCharacter<SimulatableT>(id);

            // ⛔ CALL SITE 1 — arrival, on the GAME thread; the store is read on the
            // physics thread. Rule in Network/RemoteInputCache.h. §6
            predictionOwner.setOnRelayedInputReceivedCallback(
                [this, id](const typename PredictionOwnerFor<SimulatableT>::RelayedInputRingType& ring) {
                    onRelayedInputReceived<SimulatableT>(id, ring);
                });

            // ⛔ CALL SITE 2 — POPULATE ONCE AT BIND, through the same by-id door the OnRep
            // callback uses. Without it a ring that replicated before registration leaves the
            // store empty until the next relay write. §6
            //
            // ⛔ DO NOT COPY THE TIER/FLOOR LATCH-AND-REPLAY PATTERN HERE. Tier and floor are
            // notification-only scalars and must be latched; the relay ring is a PERSISTENT
            // PROPERTY and is therefore its own latch. §6
            //
            // ⛔ On the authority this reads an unwritten ring and no-ops on version 0, and
            // the callback can NEVER fire there — OnRep is a client-only notification. §6
            //
            // ⛔ DELIBERATELY NOT FED TO THE CADENCE PROBE — a bind-time catch-up read is not
            // an arrival, and on the authority it would put a phantom sample in the histogram
            // on the one role with no relay traffic. ingestRelayRing states the same rule. §6
            inputResolution.template ingestRelayRing<SimulatableT>(
                id, predictionOwner.getRelayedInputRing());
        }

        predictionOwner.setOnCorrectionStateReceivedCallback(
            [this, id](const typename PredictionOwnerFor<SimulatableT>::SyncedCorrectionBufferType& buffer) {
                onCorrectionReceived<SimulatableT>(id, buffer);
            });

        // ⛔ A PREDICTION OWNER BINDS EXACTLY TWO INBOUND CHANNELS: correction state, and
        // the relay ring for remote characters only. There is no third — a proxy's input
        // comes from the relay ring by capture tick, never a per-tick server echo. §3
    }

    // Server overload registration helper.
    template <typename SimulatableT>
        requires AuthoritySyncedBufferOwnerConcept<
            AuthorityOwnerFor<SimulatableT>,
            typename SimulatableT::StateType,
            typename SimulatableT::InputType>
    void registerAuthorityOwner(
        unsigned int id,
        AuthorityOwnerFor<SimulatableT>& authorityOwner,
        SimulationInputResolution<SimulatableTs...>& inputResolution)
    {
        // ⛔ THE CROSS-PEER SET INVARIANT'S OTHER HALF — stated in full at
        // registerPredictionOwner. The m_authorityWriters insert below is the matching
        // step of the same fixed sequence. §5
        inputResolution.template registerAuthorityCharacter<SimulatableT>(id);

        // THE NEUTRAL-INJECTION ROLE GUARD.
        //
        // ⛔ The next authority tick underruns and substitutes the neutral, so injecting
        // on the client role only integrates (0,0,0) forwards for a whole join window.
        // ⛔ Deliberately NOT a hard failure — the degraded value is a legal input, and
        // aborting a live session would be worse than the defect. §7
        //
        // ⛔ THE WARNING IS KEPT after the m_lastUsedInputs seed beside it was removed — it
        // guards the underrun substitute alone, still real and otherwise silent. §7
        // ⛔ It says NOT-INJECTED-YET, not permanent damage: a late setNeutralInput does
        // fix subsequent ticks. §7
        if (!inputResolution.template hasNeutralInput<SimulatableT>())
        {
            SIMLOG(m_logger,
                "[Warning][NeutralInput] registerAuthorityOwner id=%u ran BEFORE setNeutralInput"
                " - every underrun substitute until injection falls back to a"
                " value-initialised input", id);
        }

        // ⛔ Routed through the peer's by-id queueRemoteMove door, never a captured queue
        // reference — a late fire after unregistration is then a benign lookup miss. §5
        // ⛔ Guard context crosses BY VALUE. The queue dedups by capture_tick
        // (first-writer-wins) and rejects ticks beyond what setAuthorityGuardContext
        // published; the drop is warned here so the queue stays logger-free. §5
        authorityOwner.setOnRemoteMoveReceivedCallback(
            [this, id](uint32 tick, const typename SimulatableT::InputType& input) {
                SIMLOG(m_logger, "[ReceiveLocalInput] id=%u tick=%u", id, tick);
                const QueueMoveResult result = m_inputResolution.template queueRemoteMove<SimulatableT>(
                    id, tick, input, m_currentAuthorityTick, m_rollbackWindowTicks);
                if (result == QueueMoveResult::TooFarFutureDiscarded)
                {
                    SIMLOG(m_logger,
                        "[ReceiveLocalInput] DISCARD too-far-future id=%u tick=%u authorityTick=%u",
                        id, tick, m_currentAuthorityTick);
                }
            });

        std::get<AuthorityWriterMapFor<SimulatableT>>(m_authorityWriters)
            .emplace(id, AuthorityWriter<SimulatableT>{ &authorityOwner });
    }

    // ⛔ CENTRALIZED UNREGISTER, FIXED ORDER — and the order is load-bearing. §8
    // ⛔ Step 1 MUST precede step 3: clearing callbacks before any lifecycle erase is
    // the contract every owner is bound under, KEPT UNCHANGED rather than loosened. §8
    template <typename T>
    void unregisterSimulatable(
        unsigned int id,
        PredictionOwnerFor<T>* predictionOwner,
        SimulationInputResolution<SimulatableTs...>& inputResolution,
        AuthorityOwnerFor<T>* authorityOwner = nullptr)
    {
        // ⛔ Step 1: clear RPC-inbound callbacks — before any data-map erase.
        if (predictionOwner)
        {
            predictionOwner->clearOnCorrectionStateReceivedCallback();
            // ⛔ clearOnCorrectionInputReceivedCallback IS GONE — the channel it unbound is
            // retired, so there is nothing to clear and nothing to re-add. §3
            predictionOwner->clearOnRelayedInputReceivedCallback();
        }
        if (authorityOwner)
        {
            authorityOwner->clearOnRemoteMoveReceivedCallback();
        }

        // Step 2: erase writer structs.
        std::get<AuthorityWriterMapFor<T>>(m_authorityWriters).erase(id);
        std::get<LocalInputSenderMapFor<T>>(m_localInputSenders).erase(id);

        // Step 3: the container-lifecycle erase, on the peer —
        // SimulationInputResolution::unregisterCharacter.
        inputResolution.template unregisterCharacter<T>(id);

        // ⛔ Step 4: drop BOTH telemetry siblings' per-id state, one call each, as a fixed
        // step of this ordering — NetSyncTelemetry::forgetOwner and, through the peer,
        // InputResolutionTelemetry::forgetOwner (relay probe maps + the version-mismatch
        // latch). Why the two CORRECTION probes are deliberately NOT forgotten is
        // fenced on those two methods, not here. §8
        m_telemetry.forgetOwner(id);
        inputResolution.forgetOwner(id);
    }

    // Outbound — authority STATE replication (game thread)
    //
    // ⛔ THE SERVER->CLIENT CORRECTION-INPUT CHANNEL IS RETIRED AND MUST NOT BE
    // REBUILT — its seven core symbols and five adapter-side members are listed in the
    // doc, and none exists anywhere today. §3
    //
    // What replaced it: a remote character's input reaches peers on the RELAY RING
    // keyed by CAPTURE tick, and the state carries a 4-byte applied-capture-tick ref.
    //
    // ⛔ OWN-CHARACTER DROP — the relay ring DELIBERATELY does not replace the echo's
    // other half. Relay stores exist for provider-ABSENT ids only, so there is NO
    // server-applied-input truth channel for the local character at all. §9
    //
    // ⛔ A sparse-state increment MUST RE-EXAMINE THIS DROP, NOT ASSUME IT — what made
    // it safe was that divergence is masked by the STATE within ~1 RTT, which is the
    // every-frame-correction argument. §9
    //
    // ⚠ AND THAT INCREMENT HAS LANDED — the correctionRotationK rotation made state
    // non-every-frame, so the block above is a RECORD of a priced trade, not a
    // live warning. §9
    //
    // correctionRotationK characters' states are written per tick, round-robin, so
    // each replicates at tickFrequency * K / N Hz.
    //
    // ⛔ THE RE-EXAMINATION, PERFORMED — masking WEAKENS but does not invert: a
    // skipped tick costs repair LATENCY, never repair ABILITY, bounded at
    // ceil(N/K)-1 ticks. Measured by [DivergenceProbe.Window], not asserted. §9
    // ⛔ THE ONE THING THAT WOULD RE-OPEN IT: driving K low enough that ceil(N/K)
    // approaches the rollback window. Nothing does today. §9

    // ⛔ correctionRotationK IS NOT DEFAULTED, ON PURPOSE, AND MUST NOT ACQUIRE A
    // DEFAULT — a default lets a call site acquire a cadence by accident. Production
    // passes TimeConfig::correctionRotationK. §9
    void sendCorrectionAll(const SimulationTimeStep& step, int32 correctionRotationK)
    {
        // ⛔ Wire format is entirely the buffer's write(composite, tick, ref) and MUST stay
        // in lockstep with readInto() in SimulationReconciliation::injectCorrectionState. §2
        const uint32 tick = step.getTick();

        // ⛔ Read ONCE for the whole send, advance ONCE at the end — advancing per type
        // lets one type's registration count re-phase the other's schedule. §9
        const std::size_t roundBase = m_correctionRotationRound;

        forEachTypeMap(m_authorityWriters, [&]<typename T>(auto& perTypeMap) {
            // ⛔ The round base is monotonic and unwrapped PRECISELY SO it can be wrapped per
            // type here — see Network/CorrectionRotation.h. §9
            const std::size_t writerCount = perTypeMap.size();
            std::size_t position = 0u;

            for (auto& [id, w] : perTypeMap)
            {
                const std::size_t thisPosition = position++;

                // ⛔ THE ROTATION GATE — a character not written is not dirty, and the
                // replication system rolls back a clean object's batch header, so this
                // continue costs ZERO wire bytes. §9
                //
                // ⛔ POSITION, NOT ID, deliberately — the coverage guarantee is a property of
                // POSITIONS, so an id-order container buys identity determinism nothing reads at
                // the cost of a second structure to keep in sync. §9
                //
                // ⛔ NO PER-SKIP LOG, AND DO NOT ADD ONE — at 6 characters and K=2 that is 240
                // formatted lines/s for what [DivergenceProbe.Window] already reports. §9
                if (!correctionRotation::isInRound(
                        thisPosition, roundBase, writerCount, correctionRotationK))
                {
                    continue;
                }

                auto& stored = m_storage.template get<T>(id);
                // ⛔ The join key travels WITH the state, or kNoInputCaptureTick if the authority
                // substituted one. §9
                // ⛔ Read through the accessor, NOT .at(): an id in m_authorityWriters but not yet
                // tracked must answer the sentinel, not throw on the send path. §9
                const uint32 appliedCaptureTick =
                    m_inputResolution.template getLastUsedCaptureTick<T>(id);
                SIMLOG(m_logger, "[SendCorrectionStateToClients] id=%u tick=%u appliedCaptureTick=%u",
                    id, tick, appliedCaptureTick);
                w.owner->getSyncedCorrectionStateBuffer().write(
                    stored.getAllState().getState(), tick, appliedCaptureTick);
                // ⛔ The second write and its [SendRemoteInputToClients] trace sat here; retired. §3
            }
        });

        m_correctionRotationRound =
            correctionRotation::advanceRound(m_correctionRotationRound, correctionRotationK);
    }

    // Outbound — local input RPC to authority (game thread)

    void sendLocalInputToAuthorityAll(uint32 currentTick, uint32 redundancyDepth)
    {
        // ⛔ ONE unreliable RPC carrying a redundancy bundle, NOT one reliable RPC per
        // entry — a dropped datagram then self-heals on the next frame's overlapping
        // bundle. The owner builds the wire bundle; only the queue and
        // scalars cross this line. §10
        //
        // ⛔ findPendingInputQueue + OG_CHECK is LOUD ON PURPOSE, not defensive — the
        // cross-peer set invariant means a queue MUST exist for every id here, so a
        // missing one IS the violation. Do not soften it to a silent skip. §5
        forEachTypeMap(m_localInputSenders, [&]<typename T>(auto& perTypeMap) {
            for (auto& [id, s] : perTypeMap)
            {
                auto* pendingQueue = m_inputResolution.template findPendingInputQueue<T>(id);
                OG_CHECK(pendingQueue != nullptr,
                    "sendLocalInputToAuthorityAll: no pending queue for a registered local sender id "
                    "- the cross-peer sender/provider set invariant is broken");
                SIMLOG(m_logger, "[SendLocalInputToServer] id=%u tick=%u depth=%u",
                    id, currentTick, redundancyDepth);
                s.owner->sendLocalInputToAuthority(*pendingQueue, currentTick, redundancyDepth);
                pendingQueue->releaseAllButRecent(static_cast<size_t>(redundancyDepth));
            }
        });
    }

private:
    // Variadic helper: calls fn<SimulatableT>(perTypeMap) for each type in the pack.
    template <typename TupleT, typename Fn, std::size_t... Is>
    static void forEachTypeMapImpl(TupleT& tup, Fn&& fn, std::index_sequence<Is...>)
    {
        using TypeList = std::tuple<SimulatableTs...>;
        (fn.template operator()<std::tuple_element_t<Is, TypeList>>(std::get<Is>(tup)), ...);
    }

    template <typename TupleT, typename Fn>
    void forEachTypeMap(TupleT& tup, Fn&& fn)
    {
        forEachTypeMapImpl(tup, std::forward<Fn>(fn), std::index_sequence_for<SimulatableTs...>{});
    }

    // registerPredictionOwner's two named callbacks.
    //
    // ⛔ THE SECOND CALLBACK CANNOT TAKE THE FIRST'S LIFT-AND-FOLD — its probing has
    // an early return interleaved between two probe calls, so folding it would move
    // what that return returns from. It uses decide-then-project instead:
    // decideCorrectionArrival + CorrectionArrivalDecision. §11

    // CALL SITE 1's callback — ingest routes through the peer's by-id ingestRelayRing
    // door; this method projects the report through emitRelayArrival.
    //
    // By-id lookup cost, worst case ~180/s per character — off the per-tick path.
    template <typename SimulatableT>
    void onRelayedInputReceived(
        unsigned int id,
        const typename PredictionOwnerFor<SimulatableT>::RelayedInputRingType& ring)
    {
        const RelayedInputIngestReport report =
            m_inputResolution.template ingestRelayRing<SimulatableT>(id, ring);
        m_telemetry.emitRelayArrival(id, report);
    }

    // ⛔ NO STORE ARGUMENT CROSSES THIS CALL — the one-shot version-mismatch latch
    // moved INTO the telemetry object, id-keyed, rather than taking a container
    // reference this class no longer owns. emitRelayArrival carries the ruling. §4

    // CALL SITE 2's callback.
    template <typename SimulatableT>
    void onCorrectionReceived(
        unsigned int id,
        const typename PredictionOwnerFor<SimulatableT>::SyncedCorrectionBufferType& buffer)
    {
        // THE DIVERGENCE VERDICT, SURFACED AND ATTRIBUTED.
        //
        // ⛔ THE COMPARISON ITSELF IS NOT HERE and does not move here — it stays in
        // StateCorrectionCache::tryInsertingCorrectState's isSimilarTo. THIS site adds the
        // two facts the cache cannot supply: the character id and its CLASS. §11
        CorrectionInsertVerdict verdict;
        m_reconciliation.template injectCorrectionState<SimulatableT>(id, buffer, &verdict);

        const CorrectionArrivalDecision decision =
            decideCorrectionArrival<SimulatableT>(id, verdict);
        m_telemetry.emitCorrectionArrival(id, decision);
    }

    // THE CORRECTION-ARRIVAL DECISION.
    //
    // ⛔ noteLanding always runs; noteCorrection MUST NOT run on a discard — folding
    // the probing would corrupt the aboveNewest population CorrectionLandingProbe
    // counts. §11
    // ⛔ landed is therefore a FIELD, never a mid-block return, and characterClass /
    // landingSite stay meaningful when !landed. §11
    //
    // ⛔ CorrectionArrivalDecision is NOT nested here, deliberately — this class
    // constructs it and the telemetry sibling consumes it; neither owns the other. §11

    // ⛔ THE PURE HALF: touches no probe, calls no note*/emit*/log*. §11
    // ⛔ characterClass and landingSite are computed UNCONDITIONALLY, before landed is
    // consulted — the landing probe needs them on the path the verdict probe returns
    // early from. §11
    template <typename SimulatableT>
    CorrectionArrivalDecision decideCorrectionArrival(
        unsigned int id, const CorrectionInsertVerdict& verdict)
    {
        // ⛔ THE CLASS TEST IS PROVIDER-PRESENCE, via the peer's isLocallyControlled — THE
        // identity test, the SAME lookup registerPredictionOwner and collectInputAll fork
        // on, never a second notion of remote. §5
        // ⛔ Read LIVE, not captured at bind, so it cannot drift from the map that decides
        // behaviour. §5
        const bool hasProvider =
            m_inputResolution.template isLocallyControlled<SimulatableT>(id);
        const PredictedCharacterClass characterClass = hasProvider
            ? PredictedCharacterClass::LocallyPredicted
            : PredictedCharacterClass::RemoteProxy;

        // WHERE DID IT LAND?
        //
        // The three-way split measures where a correction lands relative to the prediction
        // frontier. Full statement at CorrectionLandingProbe in OGSimulation/ResimGateProbe.h.
        //
        // ⛔ THIS PROBE CLASSIFIES A POSITION, NOT A TRIGGER — which is why the switch
        // to an edge-triggered resim gate left it unchanged. What the position
        // IMPLIES does depend on the gate. §12
        // ⛔ SO DO NOT READ A REQUESTED/ATFRONTIER RATIO WITHOUT FIRST READING THE SESSION
        // POLICY — grep [ResimGate] session policy in the log. The compiled default and
        // the shipped OnDisagreement differ, and which SHIPS is stated once, with
        // both halves anchored, at TimeConfig::resimTriggerPolicy — not re-derived here. §12
        //
        // ⛔ READ THROUGH findInputCache, NOT by giving StateCorrectionCache an identity —
        // a placement already ruled against. nullptr on the authority is the right answer:
        // no caches are allocated there and this callback cannot fire there. §12
        //
        // ⚠ READ AFTER THE INSERT — SAFE, AND ±1 RACY. Safe: tryInsertingCorrectState
        // never touches the tick buffer. Racy: pushPredictionTick advances the frontier
        // on the PHYSICS thread, misfiling one sample AtFrontier->Behind. §13
        // ⛔ NOT WORTH SYNCHRONIZING — ±1 on a 120-sample window, non-accumulating. §13
        const auto* landingCache =
            m_reconciliation.template findInputCache<SimulatableT>(id);
        const CorrectionLandingSite landingSite = classifyCorrectionLanding(
            verdict.landed, verdict.tick,
            landingCache != nullptr ? landingCache->getPredictionTick() : 0u);

        CorrectionArrivalDecision decision;
        decision.characterClass       = characterClass;
        decision.landingSite          = landingSite;
        decision.landed               = verdict.landed;
        decision.tick                 = verdict.tick;
        decision.predictionWasCorrect = verdict.predictionWasCorrect;
        return decision;
    }

    // ⛔ THE PROJECTION IS NOT HERE — NetSyncTelemetry::emitCorrectionArrival, also the
    // SOLE caller of emitCorrectionVerdictClassLine and emitCorrectionLandingClassLine. §4

    // ⛔ PROBES 1 + 3's per-window summary is not here either — it is
    // InputResolutionTelemetry::emitRelayReadWindowIfDue, once per prediction tick from
    // SimulationInputResolution::collectInputAll. §4

    // Owner-BINDING maps only — see the aliases' own guard for what left.
    std::tuple<AuthorityWriterMapFor<SimulatableTs>...>  m_authorityWriters;
    std::tuple<LocalInputSenderMapFor<SimulatableTs>...> m_localInputSenders;

    // ⛔ THE GAME-THREAD HALF OF THE PROBE ROSTER — RelayArrivalProbe,
    // CorrectionVerdictProbe, CorrectionLandingProbe and FOUR emit* helpers, all
    // members of NetSyncTelemetry. The physics-thread half, RelayReadProbe plus
    // TWELVE emit* helpers, belongs to InputResolutionTelemetry. §4
    NetSyncTelemetry m_telemetry;

    SimulationObjectStorage<SimulatableTs...>&   m_storage;
    SimulationReconciliation<SimulatableTs...>&  m_reconciliation;

    // ⛔ WHICH METHODS REACH THE PEER THROUGH THIS MEMBER, AND WHICH MUST NOT.
    // Member: sendCorrectionAll, sendLocalInputToAuthorityAll, onRelayedInputReceived,
    // decideCorrectionArrival — the last two because their lambdas capture only
    // (this, id). §13
    // ⛔ EXPLICIT PARAMETER instead: registerPredictionOwner, registerAuthorityOwner,
    // unregisterSimulatable — the facade already holds the same reference. §13
    SimulationInputResolution<SimulatableTs...>& m_inputResolution;

    std::function<void(const char*)>             m_logger;

    // ⛔ PLAIN (non-atomic), AND THAT IS THE RULING — an at-most-one-tick-stale
    // authority tick is fine for a multi-tick rollback window, matching
    // RemoteMoveQueue's single-consumer assumption. setAuthorityGuardContext writes
    // these on the PHYSICS thread; the RPC-arrival lambda reads them on the GAME
    // thread. Priced, not overlooked. §13
    // ⛔ -1 disables the future guard until SimulationManager injects the window. §13
    uint32 m_currentAuthorityTick = 0;
    int32  m_rollbackWindowTicks  = -1;

    // ⛔ THE STATE-ROTATION CURSOR — monotonic, advanced by K once per
    // sendCorrectionAll, wrapped per type map at the point of use. Plain (non-atomic)
    // because sendCorrectionAll is GAME thread only. §13
    //
    // ⛔ NOT WIPED BY wipeAllForResync, INTENTIONALLY — a hard resync jumps the CLOCK,
    // and this is a round-robin position, not a tick-keyed quantity. Re-phasing would
    // skip or double-write a character for one round. §13
    std::size_t m_correctionRotationRound = 0u;
};

// SimulationNetSyncConcept

// ⛔ collectInputAll, collectResimInputAll and wipeAllForResync LEFT THIS CONCEPT
// for SimulationInputResolutionConcept. §13
// ⛔ SimulatableTs STAYS for signature symmetry with the peer concepts, though no
// member below returns a SimulatableTs-typed value. §13
template <typename T, typename... SimulatableTs>
concept SimulationNetSyncConcept = requires(
    T& t, const SimulationTimeStep& step, uint32 tick, int32 rollbackWindow,
    int32 correctionRotationK)
{
    // The rotation width is required, not defaulted — see the note at the definition.
    { t.sendCorrectionAll(step, correctionRotationK) };
    { t.sendLocalInputToAuthorityAll(tick, tick) };
    { t.setAuthorityGuardContext(tick, rollbackWindow) };
};

// Free-function registration facade
//
// Client overload: prediction owner + optional input provider. Server overload:
// prediction owner + authority owner, the authority reading from the inbound
// remote-move queue instead.
//
// ⛔ Owner types resolve through SimulatableOwnerTraits; callers NEVER name the
// owner template parameters directly. §1

// Client overload
template <typename SimulatableT, typename... Ts>
    requires PredictionSyncedBufferOwnerConcept<
        PredictionOwnerFor<SimulatableT>,
        typename SimulatableT::StateType,
        typename SimulatableT::InputType>
void registerSimulatable(
    SimulationObjectStorage<Ts...>&        storage,
    SimulationReconciliation<Ts...>&       reconciliation,
    SimulationInputResolution<Ts...>&      inputResolution,
    SimulationNetSync<Ts...>&              netSync,
    unsigned int                           id,
    SimulatableT&&                         simulatable,
    PredictionOwnerFor<SimulatableT>&      owner,
    std::function<typename SimulatableT::InputType(
        const SimulationTimeStep&,
        const LocalInputCache<typename SimulatableT::InputType>&)> inputProvider = nullptr)
{
    // ⛔ PUBLISH LAST — createCacheFor BEFORE storage.add. A physics tick reaching a
    // storage-visible id with no cache calls getCacheFor(id) and throws, and
    // forEachSimulatable is what makes that window reachable. §14
    // ⛔ INVERTED-ORDER INVARIANT: IF STORAGE HAS ID, CACHE HAS ID.
    // ⛔ STATED AT BOTH OVERLOADS ON PURPOSE, so an edit to either sees the sibling it
    // mirrors — the server overload orders registerAuthorityOwner first instead. §14
    reconciliation.template createCacheFor<SimulatableT>(id);
    storage.template add<SimulatableT>(id, std::forward<SimulatableT>(simulatable));
    // ⛔ The inputResolution peer's containers must exist before registerPredictionOwner
    // binds callbacks against them — same order, same reason, one more call. §14
    netSync.template registerPredictionOwner<SimulatableT>(
        id, owner, std::move(inputProvider), inputResolution);
}

// ⛔ No correction cache is allocated on the authority, and none should be: it does
// not predict, resim or reconcile. §14
template <typename SimulatableT, typename... Ts>
    requires PredictionSyncedBufferOwnerConcept<
                 PredictionOwnerFor<SimulatableT>,
                 typename SimulatableT::StateType,
                 typename SimulatableT::InputType>
          && AuthoritySyncedBufferOwnerConcept<
                 AuthorityOwnerFor<SimulatableT>,
                 typename SimulatableT::StateType,
                 typename SimulatableT::InputType>
void registerSimulatable(
    SimulationObjectStorage<Ts...>&        storage,
    SimulationReconciliation<Ts...>&       /*reconciliation*/,
    SimulationInputResolution<Ts...>&      inputResolution,
    SimulationNetSync<Ts...>&              netSync,
    unsigned int                           id,
    SimulatableT&&                         simulatable,
    PredictionOwnerFor<SimulatableT>&      predictionOwner,
    AuthorityOwnerFor<SimulatableT>&       authorityOwner)
{
    // ⛔ PUBLISH LAST, SERVER SIDE — registerAuthorityOwner before storage.add.
    // ⛔ INVERTED-ORDER INVARIANT: IF STORAGE HAS ID, queueMap HAS ID.
    // ⛔ WHAT THIS ORDERING PROTECTS CHANGED WHEN FRONTIER-SLOT ALLOCATION MOVED OUT,
    // AND IS PRICED, NOT STALE. It no longer prevents a throw; it keeps the
    // queueMap-based branch dispatch in
    // SimulationInputResolution::collectInputForCharacter correct — an id exposed
    // early reading a relay store that will never exist for it. Belt-and-braces. §14
    // ⛔ Neither register call touches storage or the stored simulatable. §14
    netSync.template registerPredictionOwner<SimulatableT>(
        id, predictionOwner, nullptr, inputResolution);
    netSync.template registerAuthorityOwner<SimulatableT>(id, authorityOwner, inputResolution);
    storage.template add<SimulatableT>(id, std::forward<SimulatableT>(simulatable));
}

// Unregister facade — mirrors registration.
//
// ⛔ REGISTRATION PUBLISHES LAST, SO UNREGISTRATION MUST UNPUBLISH FIRST — the id
// must leave storage BEFORE anything erases the queueMap or cache entries a
// concurrent physics sweep gates on. storage.remove, then
// netSync.unregisterSimulatable, then removeCacheFor. §14
// ⛔ THIS WAS PREVIOUSLY REVERSED — a live crash on the authority path on
// player-leave, with the comment claiming the order the code did not do. §14
//
// ⛔ CLIENT-PATH SYMMETRY, CONFIRMED BY READING BOTH CALLEES, NOT ASSUMED — the
// cache is created before storage.add, so removeCacheFor must be LAST, and neither
// call before it touches storage. §14
//
// ⛔ THIS REORDER IS LANDED AND MUST NOT BE SIMPLIFIED BACK — said precisely
// because the frontier-allocation move retired its ORIGINAL motivation. A
// DIFFERENT, real hazard depends on it now: the queueMap-based branch dispatch
// in
// SimulationInputResolution::collectInputForCharacter. §14
//
// ⛔ THE GUARANTEE, RE-CHECKED AFTER THAT MOVE — no fully-executed register/unregister
// sequence, in the stated order, produces a window visible to a concurrent
// physics tick. BOTH sweeps are now nullable- or storage-filtered. §14
template <typename SimulatableT, typename... Ts>
void unregisterSimulatable(
    SimulationObjectStorage<Ts...>&   storage,
    SimulationReconciliation<Ts...>&  reconciliation,
    SimulationInputResolution<Ts...>& inputResolution,
    SimulationNetSync<Ts...>&         netSync,
    unsigned int                      id,
    PredictionOwnerFor<SimulatableT>* predictionOwner,
    AuthorityOwnerFor<SimulatableT>*  authorityOwner = nullptr)
{
    storage.template remove<SimulatableT>(id);
    netSync.template unregisterSimulatable<SimulatableT>(
        id, predictionOwner, inputResolution, authorityOwner);
    reconciliation.template removeCacheFor<SimulatableT>(id);
}

OGSIM_OPTIMIZE_ON
// pragma optimize on.
