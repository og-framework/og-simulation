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
// [og-netcode-v2-input-relay item 42] The game-thread half of the resim-gate
// telemetry — where each landed correction sat relative to the prediction
// frontier. Fed from the same OnRep-bound callback as the verdict probe.
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

// ---------------------------------------------------------------------------
// SimulatableOwnerTraits<SimulatableT>
//
// Primary template — intentionally undefined. Each simulatable type must
// specialize this in the TestYo layer (or wherever UE types are available),
// declaring PredictionOwnerType and AuthorityOwnerType. Undefined primary
// gives a clean compile error if a specialization is missing.
// ---------------------------------------------------------------------------
template <typename SimulatableT>
struct SimulatableOwnerTraits;

template <typename T>
using PredictionOwnerFor = typename SimulatableOwnerTraits<T>::PredictionOwnerType;

template <typename T>
using AuthorityOwnerFor = typename SimulatableOwnerTraits<T>::AuthorityOwnerType;

// ---------------------------------------------------------------------------
// Owner-bound pointer structs — no std::function, no per-id heap allocation.
// Single pointer; all surrounding per-tick state (storage slot, last-used
// input, pending queue) is looked up in the call body by id, not captured here.
// sizeof must equal sizeof(void*) — enforced via static_assert in test files.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// [item 86] The per-type map aliases for the FIVE input container families,
// the provider map, the join-key map and the ladder free functions all moved
// to `SimulationInputResolution.h` (verbatim — design §F). What stays here is
// exactly the two owner-BINDING maps below: `SimulationNetSync` keeps the
// owner concepts, `SimulatableOwnerTraits`, and the registration bindings;
// the container LIFECYCLE these two maps used to be populated alongside now
// lives on the resolution peer (see `registerPredictionOwner` /
// `registerAuthorityOwner` / `unregisterSimulatable` below).
// ---------------------------------------------------------------------------

template <typename T>
using AuthorityWriterMapFor = std::unordered_map<unsigned int, AuthorityWriter<T>>;

template <typename T>
using LocalInputSenderMapFor = std::unordered_map<unsigned int, LocalInputSender<T>>;

// ---------------------------------------------------------------------------
// Concept helpers — declared here so concepts below can reference them.
//
// CompositeSyncedBufferConcept encodes the wire-format contract of every
// tick-stamped replicated buffer in the system: it must support writing a
// composite with a tick, and reading one back, returning the tick. The two
// are the exact mirror pair used by SimulationNetSync::sendCorrectionAll /
// sendLocalInputToAuthorityAll (write side) and
// SimulationReconciliation::injectCorrectionState +
// SimulationNetSync::registerAuthorityOwner RPC callback (read side).
// ([T8] `injectCorrectionInput` used to be named here as a third read-side
// consumer; the correction-input channel is retired.)
// Constraining buffer accessors through this concept is what guarantees
// the two sides stay in lockstep — breaking either the write or the readInto
// signature becomes a compile error at the registerSimulatable call site,
// not a runtime "tick=1056398093" wire-format corruption.
// ---------------------------------------------------------------------------

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

// [og-netcode-v2-input-relay T4] The CORRECTION-STATE buffer carries one field
// the input buffer does not: the per-tick applied-capture-tick reference (the
// join key between the state channel and the relayed-input channel). It is
// therefore a REFINEMENT of the shared composite contract rather than an
// extension of it — requiring the ref on every tick-stamped buffer would put a
// meaningless field on the input buffer, whose payload IS an input and has no
// "which input produced this" question to answer.
//
// Both halves are required in one place for the same reason
// CompositeSyncedBufferConcept exists: the writer (sendCorrectionAll) and the
// reader (injectCorrectionState) must stay in lockstep, so breaking either side
// is a compile error at the registerSimulatable call site rather than a silent
// wire-format divergence.
template <typename BufferT, typename StateT>
concept CorrectionStateSyncedBufferConcept =
    CompositeSyncedBufferConcept<BufferT, StateT> &&
    requires(std::remove_reference_t<BufferT>& b,
             const std::remove_reference_t<BufferT>& cb,
             const StateT& in,
             uint32 tick,
             uint32 appliedCaptureTick)
    {
        // Write the state AND the ref together. One call, so a publish can never
        // pair this tick's state with the previous tick's ref.
        { b.write(in, tick, appliedCaptureTick) };
        { cb.getAppliedCaptureTick() } -> std::same_as<uint32>;
    };

// [T5 / input relay] The prediction owner additionally exposes the RELAY RING.
// It was the THIRD replicated channel when T5 wrote this, alongside the
// correction-state and correction-input buffers; **[T8] it is now the second of
// two** — the correction-input channel is retired and the relay ring is what
// replaced it. Both halves are required here for the same reason the
// buffer concepts are: registerPredictionOwner binds the arrival callback AND
// reads the ring once at bind, so an owner that grew only one of the two would
// fail at the registerSimulatable call site rather than silently leaving a proxy
// with a store nothing ever fills.
//
// The ring type is UE-side (`FRelayedInputRing`) and never named here — it arrives
// as `OwnerT::RelayedInputRingType` and is consumed only through the
// engine-agnostic codec's BUFFER CONCEPT, exactly as the sync buffers are.
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
        // SyncedRemoteInputBufferType survives T8 in its OTHER role: it is the
        // CLIENT->SERVER buffer handed back by getClientToServerInputSyncedBuffer
        // below. What is gone is its server->client role — the correction-input
        // arrival callback pair used to be required right here.
        requires CompositeSyncedBufferConcept<typename OwnerT::SyncedRemoteInputBufferType, InputT>;
        { owner.setOnCorrectionStateReceivedCallback(corrFn) };
        { owner.clearOnCorrectionStateReceivedCallback() };
        { owner.setOnRelayedInputReceivedCallback(relayFn) };
        { owner.clearOnRelayedInputReceivedCallback() };
        { constOwner.getRelayedInputRing() } -> std::same_as<const typename OwnerT::RelayedInputRingType&>;
        { owner.getClientToServerInputSyncedBuffer() } -> std::same_as<typename OwnerT::SyncedRemoteInputBufferType*>;
        // The local-input RPC is the unreliable + redundancy FInputRedundancyBundle
        // path. The owner builds the bundle (a UE wire type opaque to this UE-free
        // layer) from the most-recent `redundancyDepth` ticks still held in
        // `pendingQueue` and fires the unreliable RPC. The bundle type never appears
        // here — only the core PendingInputQueue + scalar params do.
        { owner.sendLocalInputToAuthority(pendingQueue, currentTick, redundancyDepth) };
    };

// [og-netcode-v2-input-relay T8] THE AUTHORITY OWNER NO LONGER OWNS AN OUTBOUND
// INPUT BUFFER. `getSyncedCorrectionInputBuffer` and, with it, the
// `SyncedRemoteInputBufferType` typedef + composite-buffer constraint are gone
// from this concept: sendCorrectionAll's input write was the sole consumer of
// both, and the authority's outbound surface is now exactly the correction STATE
// buffer (carrying T4's applied-capture-tick ref) plus the relay ring the T3 sink
// writes through the component. The constraints were removed rather than left
// dangling because a concept that still demanded a buffer for a role nothing
// serves would make every future authority owner carry a member for nothing.
template <typename OwnerT, typename StateT, typename InputT>
concept AuthoritySyncedBufferOwnerConcept =
    requires(OwnerT& owner,
             std::function<void(uint32, const InputT&)> fn)
    {
        { owner.getSyncedCorrectionStateBuffer() } -> CorrectionStateSyncedBufferConcept<StateT>;
        // The remote-move callback is per-slot. The owner unpacks the inbound
        // FInputRedundancyBundle and invokes this callback once per
        // (capture_tick, input) slot — the bundle type stays UE-side.
        { owner.setOnRemoteMoveReceivedCallback(fn) };
        { owner.clearOnRemoteMoveReceivedCallback() };
    };

// ---------------------------------------------------------------------------
// SimulationNetSync<SimulatableTs...>
//
// Owns per-type owner-binding maps (AuthorityWriter, LocalInputSender) as
// concrete pointer structs (no std::function on the hot path), the owner
// registration bindings and callbacks, transport (correction-state send,
// local-input RPC send, relay-ring arrival, remote-move RPC arrival,
// correction-arrival landing), and the receive-side guard context. Holds
// refs to SimulationObjectStorage, SimulationReconciliation and — as of
// item 87's promotion — SimulationInputResolution, a real
// composition-root-constructed sibling peer (design §A.3) reached through
// the by-id doors its own registration and transport methods call. NetSync
// no longer owns or forwards the resolution peer's surface; callers reach it
// directly.
//
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
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
        // [task 79, split at item 85; item 87] `m_telemetry` is this class's
        // OWN telemetry sibling — the resolution peer's `InputResolutionTelemetry`
        // is seeded by the composition root calling
        // `SimulationInputResolution::setLogger` directly now that the peer is
        // no longer a scaffold sub-object (see that method's own comment).
        // `m_logger` stays on this class too — the SIMLOG call sites that are
        // NOT part of the sixteen `emit*` helpers (e.g. [ReceiveLocalInput],
        // [SendCorrectionStateToClients]) never moved and still use it
        // directly.
        m_telemetry.setLogger(logger);
        m_logger = std::move(logger);
    }

    // Published by SimulationManager each authority tick so the RPC-arrival
    // queueMove path can reject too-far-future capture ticks. currentAuthorityTick
    // is the server's current tick; rollbackWindowTicks comes from TimeConfig::rollbackWindowTicks
    // (no hardcoded literal here). Until this is called, the guard is disabled
    // (m_rollbackWindowTicks defaults to -1), so unconfigured instances (isolated unit
    // tests) keep an accept-all behavior plus the capture-tick dedup.
    void setAuthorityGuardContext(uint32 currentAuthorityTick, int32 rollbackWindowTicks)
    {
        m_currentAuthorityTick = currentAuthorityTick;
        m_rollbackWindowTicks  = rollbackWindowTicks;
    }

    // =======================================================================
    // [og-netcode-v2-input-relay task 59, retargeted task 79] THE THREE PROBE
    // ACCESSORS' DIAGNOSTIC VIEW — RN-9 + amendment, grouped per
    // `docs/DiagnosticsConventions.md` (the classification rule these views
    // follow is centralised there, not re-derived here). A sibling ruling
    // groups `SimulationManager`'s own resim-gate probe accessor the same
    // way, in the same task.
    //
    // ⛔ ONLY THESE THREE READ-ONLY ACCESSORS MOVE. The probe MEMBERS, the
    // `note*` call sites that feed them every frame, and the `emit*` helpers
    // are production instrumentation and are UNTOUCHED by this view — task 79
    // relocated all four onto sibling telemetry objects; item 85 split the PT
    // group onto `InputResolutionTelemetry`; item 87 dropped this class's own
    // two-hop `relayReadProbe()` delegation entirely (design §C.6: "the
    // resolution peer's `getDiagnostics()` exposes `relayReadProbe()`;
    // NetSync's keeps the other three") — callers reach it directly at
    // `inputResolution.getDiagnostics().relayReadProbe()` now that the peer
    // is no longer a scaffold sub-object. Each remaining accessor's NAME,
    // COUNT and const-only shape is unchanged. See `NetSyncTelemetry.h` and
    // `SimulationInputResolution.h` / `InputResolutionTelemetry.h` for the
    // probe declarations and the fence comment on each, and
    // `docs/DiagnosticsConventions.md` §2/§3 for the current roster and
    // count.
    //
    // CONST-ONLY AND DELIBERATELY SO: the only writers are on the telemetry
    // siblings, each reachable from exactly one thread (see each class's own
    // two-thread banner), and a mutable handle handed out here would be an
    // invitation to increment a physics-thread counter from the game thread —
    // the exact hazard the two-object split exists to prevent. There is
    // therefore no `MutableDiagnostics` / `editDiagnostics()` on this class,
    // unlike `StateCorrectionCache`'s pair.
    //
    // Nested class, not a free function: it has the same access to
    // SimulationNetSync's private members as any other member function — no
    // friend declaration needed, and the view holds nothing but a reference.
    // =======================================================================
    class Diagnostics
    {
    public:
        explicit Diagnostics(const SimulationNetSync& netSync) : m_netSync(netSync) {}

        const RelayArrivalProbe& relayArrivalProbe() const { return m_netSync.m_telemetry.relayArrivalProbe(); }

        // [T24] Same contract, same reason. Its consumer is the og-brawler-tests
        // wiring block: the probe's arithmetic is unit-tested in og-simulation-tests,
        // but only a suite with SimulatableOwnerTraits bound to concrete owners can
        // show that the shipped correction callback actually feeds it AND classifies
        // by the same provider-presence test the rest of this class uses.
        const CorrectionVerdictProbe& correctionVerdictProbe() const
        { return m_netSync.m_telemetry.correctionVerdictProbe(); }

        // [og-netcode-v2-input-relay item 42 / I2] Same contract, same consumer, same
        // reason as the verdict probe's accessor directly above: the three-way
        // classification is swept as a unit in og-simulation-tests, but only a suite
        // that can register a real local and a real remote character against the real
        // callback can show that the shipped site feeds it, files each landing under
        // the right class, and puts a DISCARD in the discarded bucket rather than
        // dropping it on the verdict probe's early return.
        const CorrectionLandingProbe& correctionLandingProbe() const
        { return m_netSync.m_telemetry.correctionLandingProbe(); }

    private:
        const SimulationNetSync& m_netSync;
    };

    Diagnostics getDiagnostics() const { return Diagnostics(*this); }

    // -----------------------------------------------------------------------
    // Registration — free functions delegate here; not called directly by users
    // -----------------------------------------------------------------------

    // Client overload registration helpers — called from registerSimulatable free function.
    //
    // [item 86 / design §C.4's closing paragraph] THE CROSS-PEER SET
    // INVARIANT. Before this item, "m_inputProviders / m_pendingInputQueues /
    // m_localInputSenders populated as a set" was an INTRA-class invariant —
    // all three lived here. Post-cut, providers+queues live on
    // `m_inputResolution`, senders stay here: the invariant is now
    // *sender-map keys ⊆ provider-set*, maintained SOLELY by this method (and
    // `registerAuthorityOwner` below) calling the resolution peer's
    // container-lifecycle methods and this class's own map insert in the SAME
    // fixed sequence, every time. `sendLocalInputToAuthorityAll` is the one
    // consumption point, and it enforces the invariant loudly
    // (`findPendingInputQueue` + `OG_CHECK`) rather than assuming it.
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
        // Input provider is present iff this owner drives a locally-controlled
        // simulatable.
        if (inputProvider)
        {
            inputResolution.template registerLocalCharacter<SimulatableT>(id, std::move(inputProvider));

            std::get<LocalInputSenderMapFor<SimulatableT>>(m_localInputSenders)
                .emplace(id, LocalInputSender<SimulatableT>{ &predictionOwner });
        }
        else
        {
            // [T5] REMOTE character (no provider) — the resolution peer's
            // registerRemoteCharacter creates the neutral-seeded relay store;
            // this class binds the two callbacks that feed it, by id only
            // (design §C.3 — no captured container reference).
            inputResolution.template registerRemoteCharacter<SimulatableT>(id);

            // CALL SITE 1 — arrival. Fires on the GAME thread (see the THREADING
            // section in Network/RemoteInputCache.h); the store is read on the
            // physics thread.
            predictionOwner.setOnRelayedInputReceivedCallback(
                [this, id](const typename PredictionOwnerFor<SimulatableT>::RelayedInputRingType& ring) {
                    onRelayedInputReceived<SimulatableT>(id, ring);
                });

            // CALL SITE 2 — POPULATE ONCE AT BIND, through the same by-id door
            // the OnRep callback uses. Closes the hole where the ring
            // replicated (and its OnRep fired into a null callback) before this
            // registration ran, and the sender then went quiet: without this the
            // store would stay empty until the next relay write.
            //
            // DO NOT COPY T10/T11's LATCH-AND-REPLAY PATTERN HERE — this is the
            // likely mistake precisely because the two immediately-preceding tasks
            // established the opposite shape. Tier and floor are change-notification
            // -only SCALARS: a notification that fires before the listener binds is
            // never repeated, so the value has to be latched. The relay ring is a
            // PERSISTENT PROPERTY — re-reading it always yields the full current
            // state, so THE PROPERTY IS ITS OWN LATCH. Latch machinery here would
            // earn nothing and would add a second copy of the ring to keep in sync.
            //
            // On the AUTHORITY this reads the server's own (as-yet-unwritten) ring
            // and no-ops on version 0; the callback it binds can never fire there,
            // because OnRep is a client-only notification.
            //
            // [T19] DELIBERATELY NOT FED TO THE CADENCE PROBE. This is a bind-time
            // catch-up read, not an arrival: no replication event occurred, and on
            // the authority it runs at all — so counting it would put a phantom
            // sample in the histogram on the one role that has no relay traffic.
            // The cost is that the first real OnRep seeds the watermark instead of
            // producing a gap, which is exactly one lost sample per component per
            // session. The report is discarded here, unread — `ingestRelayRing`'s
            // own comment states the same rule from the peer side.
            inputResolution.template ingestRelayRing<SimulatableT>(
                id, predictionOwner.getRelayedInputRing());
        }

        predictionOwner.setOnCorrectionStateReceivedCallback(
            [this, id](const typename PredictionOwnerFor<SimulatableT>::SyncedCorrectionBufferType& buffer) {
                onCorrectionReceived<SimulatableT>(id, buffer);
            });

        // [og-netcode-v2-input-relay T8] The correction-INPUT arrival binding that
        // sat here is gone. A prediction owner now binds exactly two inbound
        // channels: the correction STATE (above, carrying T4's applied-capture-tick
        // ref) and — for remote characters only — the relay ring (in the
        // provider-absent branch above). The input a proxy runs on comes from the
        // ring, resolved by capture tick, not from a per-tick server echo.
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
        // [item 86 / design §C.4's closing paragraph] THE CROSS-PEER SET
        // INVARIANT'S OTHER HALF — see the full statement at
        // registerPredictionOwner above. This call seeds the resolution
        // peer's remote-move queue + join-key entry; this method's own
        // `m_authorityWriters` insert below is this class's matching half of
        // the same fixed sequence.
        inputResolution.template registerAuthorityCharacter<SimulatableT>(id);

        // [T17] THE NEUTRAL-INJECTION ROLE GUARD.
        //
        // Registration is the moment this character's authority path goes live, and
        // the very next authority tick takes collectInputAll's remote branch with an
        // empty queue — i.e. underruns, and substitutes the injected neutral. A
        // composition root that injected on the client role only would therefore
        // integrate `InputType{}` ((0,0,0) forward vectors) for the whole join
        // window, every join, with nothing to say so. This warns at the site where
        // that first becomes reachable. Deliberately not a hard failure: the
        // degraded value is still a legal input, and aborting a running session over
        // it would be worse than the defect.
        //
        // [T8] T17 ALSO SEEDED `m_lastUsedInputs` HERE, and that seed is gone with
        // the correction-input channel it existed to replicate (AM-1's "the seed
        // reaches peers before the first applied input" reasoning was entirely about
        // that channel). Two consequences worth stating rather than leaving a reader
        // to infer:
        //   * The warning STAYS and its site is still right — it now guards the
        //     underrun substitute alone, which is the one authority consumer left.
        //   * The ORDERING ASSUMPTION T17 documented here DISSOLVES. The substitute
        //     re-reads m_neutralInputs on every underrun tick, so a setNeutralInput
        //     arriving after registration now does fix subsequent ticks; nothing is
        //     read once and frozen any more. The warning is consequently a
        //     "not injected yet, and this character is already live" signal rather
        //     than a permanent-damage one. It is kept because that window is still
        //     real and still silent otherwise.
        if (!inputResolution.template hasNeutralInput<SimulatableT>())
        {
            SIMLOG(m_logger,
                "[Warning][NeutralInput] registerAuthorityOwner id=%u ran BEFORE setNeutralInput"
                " - every underrun substitute until injection falls back to a"
                " value-initialised input", id);
        }

        // RPC inbound — [item 86] now routes through the resolution peer's
        // by-id `queueRemoteMove` door instead of a captured `&remoteQueue`
        // reference (design §C.3): a late-firing callback after
        // unregisterCharacter's erase becomes a benign lookup miss instead of
        // a dangling reference. Guard context crosses in BY VALUE from this
        // class's own members — the guard stays NetSync's (design §C.4). The
        // queue dedups by capture_tick (first-writer-wins) and rejects
        // too-far-future capture ticks against the guard context published by
        // SimulationManager via setAuthorityGuardContext (current authority tick +
        // TimeConfig::rollbackWindowTicks). A too-far-future drop is warned here so the
        // queue stays logger-free.
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

    // Centralized unregister — fixed order; ordering is load-bearing.
    // Step 1 MUST precede step 3: the pre-item-86 onRemoteMoveReceived captured
    // &remoteQueue from m_remoteMoveQueues; [item 86] the callbacks now route by
    // id through the resolution peer, so a late fire is a benign lookup miss
    // rather than UB — but clearing callbacks first, before any lifecycle erase,
    // remains the contract every registered owner is bound under, and is kept
    // unchanged rather than loosened now that its failure mode is cheaper.
    template <typename T>
    void unregisterSimulatable(
        unsigned int id,
        PredictionOwnerFor<T>* predictionOwner,
        SimulationInputResolution<SimulatableTs...>& inputResolution,
        AuthorityOwnerFor<T>* authorityOwner = nullptr)
    {
        // Step 1: clear RPC-inbound callbacks on the owner(s) — before any data-map erase.
        if (predictionOwner)
        {
            predictionOwner->clearOnCorrectionStateReceivedCallback();
            // [T8] `clearOnCorrectionInputReceivedCallback` was here; the channel it
            // unbound is retired, so there is nothing to clear.
            predictionOwner->clearOnRelayedInputReceivedCallback();
        }
        if (authorityOwner)
        {
            authorityOwner->clearOnRemoteMoveReceivedCallback();
        }

        // Step 2: erase writer structs.
        std::get<AuthorityWriterMapFor<T>>(m_authorityWriters).erase(id);
        std::get<LocalInputSenderMapFor<T>>(m_localInputSenders).erase(id);

        // Step 3: [item 86] the container-lifecycle erase — five container
        // families' entries plus the join key — now lives on the resolution
        // peer; see SimulationInputResolution::unregisterCharacter.
        inputResolution.template unregisterCharacter<T>(id);

        // [T19, relocated task 79, split at item 85, resolution peer owns the
        // PT sibling at item 86] Step 4: drop BOTH telemetry siblings' per-id
        // state (the two relay probes' id-keyed maps plus the
        // version-mismatch log-once latch) — one call each, same fixed step
        // of the same ordering that lived here before task 79 moved the
        // probes off this class. `inputResolution.forgetOwner` forwards to
        // its own `InputResolutionTelemetry` sibling. See
        // `NetSyncTelemetry::forgetOwner` and
        // `InputResolutionTelemetry::forgetOwner` for the full rationale
        // (including why the two correction probes are deliberately NOT
        // forgotten by either).
        m_telemetry.forgetOwner(id);
        inputResolution.forgetOwner(id);
    }

    // -----------------------------------------------------------------------
    // Outbound — authority STATE replication (game thread)
    // -----------------------------------------------------------------------
    //
    // [og-netcode-v2-input-relay T8] THE CONTRACT HALF OF T3's EXPAND/CONTRACT
    // PAIR. This method used to publish TWO things per character per tick: the
    // corrected state, and the input the authority applied ("here is what I ran
    // you on"). The second write is gone, and with it the whole SERVER->CLIENT
    // correction-INPUT channel:
    //
    //   core   : m_lastUsedInputs + LastUsedInputMapFor          (the staged value)
    //            StateCorrectionCache::insertCorrectionInput      (the sink)
    //            StateCorrectionCache::getLastCorrectionInput     (the reader)
    //            StateCorrectionCache::m_containsCorrectionInput  (the flag)
    //            receiveCorrectionInput                           (the decoder)
    //            SimulationReconciliation::injectCorrectionInput  (the router)
    //            the two owner-concept requirements + the OnRep binding
    //   UE     : USimmableUpdateComponent::m_replicatedInputSyncedBuffer, its
    //            OnRep_CorrectionInput, its DOREPLIFETIME registration, its
    //            callback plumbing and getSyncedCorrectionInputBuffer
    //
    // WHAT REPLACED IT, and why this is a net removal rather than a trade: a
    // remote character's input now reaches peers on the RELAY RING, keyed by
    // CAPTURE tick and stamped with the schedule (T1/T3/T5), and the correction
    // state carries a 4-byte applied-capture-tick REF (T4) naming which capture
    // the authority ran. Identity plus one scalar replaced a whole replicated
    // input composite, so the per-tick wire cost of this send goes DOWN.
    //
    // ---- OWN-CHARACTER DROP (InputRelayDesign.md §7, coverage gap 3) ----------
    // The retired channel also fired on the OWNING client — OnRep_CorrectionInput
    // ran on every peer, including the one whose input it echoed. The relay ring
    // deliberately does not replace that half: relay stores are created only for
    // provider-ABSENT ids (registerPredictionOwner), so a client gets no relay
    // store for its own character and there is now NO server-applied-input truth
    // channel for the local character at all.
    //
    // WHY THAT IS SAFE THIS INCREMENT, stated so a future reader does not have to
    // re-derive it:
    //   * Nothing consumed it. The local resim resolves its own input from
    //     LocalInputCache by the T4 ref (T6), the local viz reads the live
    //     sampler (T13's hasLiveLocalInput branch), and the motion matcher reads
    //     raw captures (T15). The last reader of the echoed value was re-pointed
    //     before this task landed.
    //   * Divergence is masked by the STATE, not by the input. Corrections ship
    //     every frame; if the authority applied a different input than the client
    //     predicted, the resulting STATE difference lands within ~1 RTT and drives
    //     a resim regardless of whether the input value was echoed.
    //   * The T4 ref still carries the DIAGNOSTIC content that mattered: the
    //     client can always ask "which of my captures did the server actually run
    //     at tick T", and answer it against its own delay line.
    // THE ONE THING THAT WOULD RE-OPEN THIS: sparse (non-every-frame) state. The
    // masking argument above is exactly the every-frame-correction argument, and
    // it is the same premise the deferred stale-hold rule rests on (design §8.7).
    // A sparse-state increment must re-examine this drop, not assume it.
    //
    // ---- ⭐ [T39] THAT INCREMENT HAS LANDED. STATE IS NO LONGER EVERY-FRAME. ---
    // The paragraph above was written as a fence; this is the task that crossed it,
    // deliberately and with the trade priced, so the fence is now a RECORD of what
    // was traded rather than a warning about what must not be
    // (design_task38_input_first_replication.md §2.3, §13.1).
    //
    // WHAT CHANGED. `correctionRotationK` characters' states are written per tick,
    // round-robin (correctionRotation::isInRound below), so each character's state
    // replicates at `tickFrequency * K / N` Hz instead of `tickFrequency`. At the
    // shipped K = 2 that is unchanged at two characters and 40/30/20 Hz at 3/4/6.
    //
    // WHY THE OWN-CHARACTER INPUT-ECHO DROP SURVIVES IT — the re-examination this
    // block demanded, performed rather than assumed:
    //   * The masking argument WEAKENS but does not invert. A correction snapshot
    //     is a COMPLETE anchor with no delta chain (see §2.1 / the codec), so a
    //     skipped tick costs REPAIR LATENCY, never repair ability: the next
    //     snapshot still detects a divergence introduced during the gap and still
    //     drives the resim. The rotation wait is bounded — every writer is written
    //     within ceil(N/K) sends by construction — so the added latency is at most
    //     ceil(N/K) - 1 ticks, i.e. ~1-2 ticks at the shipped numbers.
    //   * The thing that made the drop safe in the first place is unchanged: the
    //     T4 applied-capture-tick ref still travels with every snapshot, so the
    //     client can still answer "which of my captures did the server run at tick
    //     T" from its own delay line, at whatever cadence the snapshots arrive.
    //   * The trade is strongly favourable and is the POINT of the change: the
    //     payload that lengthens its repair window is the SELF-HEALING one, and it
    //     lengthens so that the IRREPLACEABLE one (the relay ring, which has no
    //     recovery path anywhere) can no longer be displaced by it under packet
    //     pressure. Before T39 both shared one atomic Iris batch and died together
    //     (finding_task37_depth_regression.md).
    //   * It is MEASURED, not asserted: `[DivergenceProbe.Window]` reports
    //     corrections/s per character per class, so the delivered cadence is a
    //     number a run reads back rather than a claim this comment makes.
    // The one thing that would re-open THIS: driving K low enough that ceil(N/K)
    // approaches the rollback window. Nothing does today (K >= 1 and N <= 6 give
    // at most 6), and the clamp's floor of 1 is what bounds it.
    // -------------------------------------------------------------------------

    // `correctionRotationK` is NOT defaulted, on purpose. The whole deliverable of
    // T39 is that the state cadence is a DECIDED, configured number rather than an
    // emergent consequence of Iris dropping things; a default here would let a
    // future call site acquire a cadence by accident, which is the exact failure
    // mode the task exists to remove. The production caller
    // (SimulationManager::onPostSimulationGameThread) passes
    // TimeConfig::correctionRotationK; tests that are not about cadence pass an
    // explicit every-frame value.
    void sendCorrectionAll(const SimulationTimeStep& step, int32 correctionRotationK)
    {
        // Wire format is fully encapsulated by the buffer's write(composite, tick,
        // appliedCaptureTick). Must stay in lockstep with the client-side readInto()
        // in SimulationReconciliation::injectCorrectionState.
        const uint32 tick = step.getTick();

        // Read ONCE for the whole send so every type map in the tuple is scheduled
        // against the same round, and advance ONCE at the end. Advancing per type
        // would make one type's registration count re-phase the other's schedule.
        const std::size_t roundBase = m_correctionRotationRound;

        forEachTypeMap(m_authorityWriters, [&]<typename T>(auto& perTypeMap) {
            // N for THIS type. The round base is monotonic and unwrapped precisely
            // so it can be wrapped per type here — see CorrectionRotation.h.
            const std::size_t writerCount = perTypeMap.size();
            std::size_t position = 0u;

            for (auto& [id, w] : perTypeMap)
            {
                const std::size_t thisPosition = position++;

                // [T39] THE ROTATION GATE. A character not written is not dirty,
                // and Iris rolls back the batch header of a clean object, so this
                // `continue` costs ZERO wire bytes rather than deferring a write.
                //
                // POSITION, NOT ID. The cursor walks the enumeration order of this
                // map rather than a separately maintained registration-order list.
                // That is a deliberate simplification of the scoped design: the
                // coverage guarantee is a property of POSITIONS (every position is
                // covered within ceil(N/K) rounds regardless of which id occupies
                // it), so a parallel id-order container would buy identity
                // determinism that nothing reads, at the cost of a second structure
                // to keep in sync with registration/unregistration. A registration
                // change re-phases who is in a given frame's window and nothing
                // more; coverage is unaffected and self-heals within one round.
                //
                // NO PER-SKIP LOG. At 6 characters and K=2 that would be 240
                // suppressed-but-formatted lines per second for information the
                // `[DivergenceProbe.Window]` corrections/s figure already reports
                // as a delivered rate.
                if (!correctionRotation::isInRound(
                        thisPosition, roundBase, writerCount, correctionRotationK))
                {
                    continue;
                }

                auto& stored = m_storage.template get<T>(id);
                // [T4] The join key travels WITH the state it belongs to: the
                // capture tick behind the input this character's authority applied
                // (T2's tracked value), or kNoInputCaptureTick when the authority
                // substituted one. Read through the accessor rather than .at() so
                // an id present in m_authorityWriters but not yet in the track
                // answers the sentinel instead of throwing on the send path.
                const uint32 appliedCaptureTick =
                    m_inputResolution.template getLastUsedCaptureTick<T>(id);
                SIMLOG(m_logger, "[SendCorrectionStateToClients] id=%u tick=%u appliedCaptureTick=%u",
                    id, tick, appliedCaptureTick);
                w.owner->getSyncedCorrectionStateBuffer().write(
                    stored.getAllState().getState(), tick, appliedCaptureTick);
                // [T8] The second write — the input buffer, plus its
                // `[SendRemoteInputToClients]` trace — used to sit here. See the
                // retirement block above.
            }
        });

        m_correctionRotationRound =
            correctionRotation::advanceRound(m_correctionRotationRound, correctionRotationK);
    }

    // -----------------------------------------------------------------------
    // Outbound — local input RPC to authority (game thread)
    // -----------------------------------------------------------------------

    void sendLocalInputToAuthorityAll(uint32 currentTick, uint32 redundancyDepth)
    {
        // Unreliable + redundancy local-input RPC. Instead of draining the pending
        // queue one entry per reliable RPC, the owner builds a single
        // FInputRedundancyBundle out of the most-recent `redundancyDepth` ticks
        // still retained in the pending queue and fires ONE unreliable RPC. A dropped
        // datagram self-heals on the next frame's overlapping bundle. The bundle wire
        // type stays UE-side (opaque to this layer); we only hand the owner the queue
        // + scalar params. After the send we retain only the redundancy window for the
        // next frame's overlap and release older entries so the queue stays bounded.
        //
        // [item 86 / design §C.4] `findPendingInputQueue` + `OG_CHECK` replaces
        // the pre-cut `.at(id)` into `m_pendingInputQueues` (now owned by the
        // resolution peer): the cross-peer set invariant (sender-map keys ⊆
        // provider-set, see registerPredictionOwner's comment) means a queue
        // MUST exist for every id enumerated here, so the check is loud
        // rather than silent — an id present in m_localInputSenders without a
        // matching pending queue is exactly the invariant violation the two
        // registration sites exist to prevent.
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
    // Variadic helper: expands over each per-type tuple slot using index_sequence.
    // Calls fn<SimulatableT>(perTypeMap) for each SimulatableT in the pack.
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

    // -----------------------------------------------------------------------
    // [task 60 / RN-10] registerPredictionOwner's two named callbacks.
    //
    // RN-10 measured registerPredictionOwner at 358 lines, 59% comments, with
    // two ~140-line lambdas burying a ~30-line registration skeleton. Part A
    // lifted each lambda into a named member below, unchanged in every
    // probe-call semantic. Part B (arrival, immediately below) then collapsed
    // the tail probing behind ONE `emit*` call, safe because nothing in that
    // tail alters control flow. Part C (correction, further below) could NOT
    // take the same lift-and-fold: its probing has an early `return`
    // interleaved between two probe calls, and folding "the probing" into one
    // helper would move what that `return` returns from — see
    // `decideCorrectionArrival` / `CorrectionArrivalDecision` below, which
    // apply the RN-8/task-58 decide-then-project shape instead.
    // -----------------------------------------------------------------------

    // [RN-10 part A+B; re-bound at item 86] CALL SITE 1's callback
    // (registerPredictionOwner), its probing tail folded behind ONE `emit*`
    // call. [item 86] No longer takes a `store` reference — the ingest itself
    // now routes through the resolution peer's by-id `ingestRelayRing` door
    // (design §C.3); this method's own job is unchanged: project the
    // returned report through `emitRelayArrival`.
    //
    // [item 89 / design §C.3, corrected] Cost of the by-id lookup this callback
    // makes on every arrival: worst case ~180 lookups/s per character
    // (redundancy depth 3 × the 60 Hz relay rate) — off the hot per-tick path,
    // which stays at 60 Hz regardless of how many redundant entries one
    // replicated bundle carries.
    template <typename SimulatableT>
    void onRelayedInputReceived(
        unsigned int id,
        const typename PredictionOwnerFor<SimulatableT>::RelayedInputRingType& ring)
    {
        const RelayedInputIngestReport report =
            m_inputResolution.template ingestRelayRing<SimulatableT>(id, ring);
        m_telemetry.emitRelayArrival(id, report);
    }

    // [RN-10 part B, relocated task 79] CALL SITE 1's probing + logging now
    // lives on the telemetry sibling — `NetSyncTelemetry::emitRelayArrival` —
    // reusing the existing `emit*` verb rather than coining a new one. See
    // `docs/DiagnosticsConventions.md` §3 for the current `emit*` roster.
    // No `store` argument crosses this call: the helper's only use of it was
    // the one-shot version-mismatch latch, which moved INTO the telemetry
    // object, id-keyed, rather than being handed a reference to this class's
    // own `RemoteInputCache` map — see `NetSyncTelemetry.h`'s file banner and
    // `emitRelayArrival`'s own comment for the full ruling.

    // [RN-10 part A] CALL SITE 2's callback (registerPredictionOwner), lifted
    // out verbatim — a pure lift; the production call and the two helpers it
    // now delegates to below are unchanged in every semantic, only their
    // location moved.
    template <typename SimulatableT>
    void onCorrectionReceived(
        unsigned int id,
        const typename PredictionOwnerFor<SimulatableT>::SyncedCorrectionBufferType& buffer)
    {
        // [T24] THE DIVERGENCE VERDICT, SURFACED AND ATTRIBUTED.
        //
        // The comparison itself is unchanged and happens where it always
        // has — StateCorrectionCache::tryInsertingCorrectState's
        // `isSimilarTo`, on every correction, for every character. All that
        // is new is that the verdict now comes back out, and that THIS site
        // adds the two facts the cache could never supply: the character
        // `id`, and its CLASS.
        CorrectionInsertVerdict verdict;
        m_reconciliation.template injectCorrectionState<SimulatableT>(id, buffer, &verdict);

        const CorrectionArrivalDecision decision =
            decideCorrectionArrival<SimulatableT>(id, verdict);
        m_telemetry.emitCorrectionArrival(id, decision);
    }

    // [task 60 / RN-10 part C] THE CORRECTION-ARRIVAL DECISION.
    //
    // Task 58/RN-8's shape, reused rather than reinvented: this callback's
    // probing has an early `return` interleaved between two probe calls
    // (`noteLanding` always runs; `noteCorrection` must NOT run on a
    // discard), so folding "the probing" into one helper the way
    // `emitRelayArrival` does would move what that `return` returns from —
    // the verdict probe would then run on discarded corrections and silently
    // corrupt item 41's `aboveNewest` population (the discard bucket
    // `CorrectionLandingProbe` counts). `landed` is therefore a FIELD of the
    // decision, not a mid-block `return`, exactly as
    // `ScheduledRelayedReadDecision::isUnderflowMiss` is a field rather than
    // a re-derivable fact. `characterClass` / `landingSite` are meaningful
    // even when `!landed` — the landing probe needs them unconditionally,
    // see the "hoisted above the gate" note below.
    //
    // [relocated task 79] `CorrectionArrivalDecision` itself now lives in
    // `NetSyncTelemetry.h` as a free struct, not nested here — it needs a
    // name both this peer (which constructs it, below) and the telemetry
    // sibling (whose `emitCorrectionArrival` is its one consumer) can see
    // without one class owning the other. See that struct's own comment in
    // `NetSyncTelemetry.h` for the full rationale.

    // [RN-10 part C] THE PURE HALF — computes the decision, touches no probe
    // and calls no `note*`/`emit*`/`log*`. `characterClass` and `landingSite`
    // are computed UNCONDITIONALLY, before `landed` is even consulted,
    // because the landing probe needs them on every correction including
    // discards (item 42's "hoisted above the landed gate" — the class/site
    // facts must exist on the path the verdict probe returns early from).
    template <typename SimulatableT>
    CorrectionArrivalDecision decideCorrectionArrival(
        unsigned int id, const CorrectionInsertVerdict& verdict)
    {
        // THE CLASS TEST IS PROVIDER-PRESENCE — [item 86] routed through the
        // resolution peer's `isLocallyControlled` query, THE identity test
        // (design §C.1/§A.2) — the SAME lookup registerPredictionOwner forks
        // on above and collectInputAll forks on every tick, not a second
        // notion of "remote". Read live rather than captured at bind time so
        // it cannot drift from the map that actually decides behaviour.
        const bool hasProvider =
            m_inputResolution.template isLocallyControlled<SimulatableT>(id);
        const PredictedCharacterClass characterClass = hasProvider
            ? PredictedCharacterClass::LocallyPredicted
            : PredictedCharacterClass::RemoteProxy;

        // ---------------------------------------------------------------
        // [og-netcode-v2-input-relay item 42 / I2] WHERE DID IT LAND?
        //
        // THE FIRST-GATE DISCRIMINATOR. Item 31 established that resim
        // triggers were gated by the prediction-frontier slot's INHERITED
        // `m_isResimulated` bit: `getLastResimulationTick` scanned from the
        // frontier at offset 0, so that bit shadowed every older corrected
        // slot, and the ONLY event that re-opened the gate in play was a
        // correction landing EXACTLY ON the frontier. A correction landing
        // behind it set its own slot's flag, was never seen by the scan, was
        // never replayed through (resims restore at the NEWEST corrected
        // slot) and therefore never touched live state at all.
        //
        // So this three-way split is the measurement the whole item turns
        // on: `landedBehind` large while triggers track only
        // `landedAtFrontier` IS the demonstrated under-resimulation
        // statement. Full statement at CorrectionLandingProbe in
        // OGSimulation/ResimGateProbe.h.
        //
        // ⚠ [item 45] THAT MECHANISM IS GONE — the gate is now edge-triggered
        // on an explicit pending anchor, and `m_isResimulated`, the scan and
        // the inheritance are retired. THIS PROBE IS UNCHANGED AND STILL
        // CORRECT, for a reason worth stating rather than re-deriving: it
        // classifies a landing's POSITION relative to the frontier, which is
        // a fact about the correction stream, not about the gate. What
        // changed is only what that position IMPLIES: on a default build the
        // `FrontierExact` policy makes `AtFrontier` exactly the anchor-set
        // condition (same predicate, same value), so `requested` still tracks
        // `atFrontier`; after item 46's flip to `OnDisagreement` the trigger
        // population becomes the DISAGREEING landings — mostly this
        // `Behind` bucket — and the tracking claim above inverts by design.
        // Read `[ResimGate] session policy` in the log before reading a
        // ratio.
        //
        // ⚠ THE FRONTIER IS READ THROUGH THE EXISTING RECONCILIATION
        // ACCESSOR, not by giving the cache an identity. `findInputCache`
        // is the nullable route every other accessor on that class already
        // takes, and it answers nullptr on the authority (no caches are
        // allocated there), which is exactly the right answer: this
        // callback is OnRep-dispatched and can never fire on an authority
        // world anyway. Pushing id-awareness down into StateCorrectionCache
        // to carry the frontier out with the verdict is the placement T24
        // ruled against, and this needs no such thing.
        //
        // ⚠ READ AFTER THE INSERT, WHICH IS SAFE AND ±1 RACY, and both
        // halves of that matter. Safe: `tryInsertingCorrectState` never
        // touches `m_tickBuffer`, so the frontier it saw and the frontier
        // read here are the same value on this thread. Racy: the frontier
        // is ADVANCED by `pushPredictionTick` on the PHYSICS thread, and
        // the whole cache is already a formally unsynchronized GT/PT
        // structure (finding §1). A physics frame landing between the
        // insert and this read misfiles one sample from AtFrontier to
        // Behind. That is a per-sample ±1 on a 120-sample window, it does
        // not accumulate, and it is a property of the mechanism being
        // measured rather than of this instrument: whether a correction
        // hits the frontier slot at all is decided by that same
        // interleaving.
        const auto* landingCache =
            m_reconciliation.template findInputCache<SimulatableT>(id);
        const CorrectionLandingSite landingSite = classifyCorrectionLanding(
            verdict.landed, verdict.tick,
            landingCache != nullptr ? landingCache->getPredictionTick() : 0u);
        // ---------------------------------------------------------------

        CorrectionArrivalDecision decision;
        decision.characterClass       = characterClass;
        decision.landingSite          = landingSite;
        decision.landed               = verdict.landed;
        decision.tick                 = verdict.tick;
        decision.predictionWasCorrect = verdict.predictionWasCorrect;
        return decision;
    }

    // [RN-10 part C, relocated task 79] THE PROJECTION now lives on the
    // telemetry sibling — `NetSyncTelemetry::emitCorrectionArrival` — which
    // is also the sole caller of `emitCorrectionVerdictClassLine` and
    // `emitCorrectionLandingClassLine` (both relocated with it; neither had
    // any other caller). See `NetSyncTelemetry.h` for the full body and its
    // comments, unchanged from the pre-task-79 shape.

    // [T19, relocated task 79, split at item 85, resolution peer owns the PT
    // sibling at item 86] PROBES 1 + 3 — the per-window summary now lives on
    // `SimulationInputResolution`'s `InputResolutionTelemetry` sibling as
    // `emitRelayReadWindowIfDue`, called once per prediction tick from
    // `SimulationInputResolution::collectInputAll`. See
    // `InputResolutionTelemetry.h` for the full bodies and comments,
    // unchanged otherwise from the pre-task-79 shape.

    // [item 86] Owner-BINDING maps only — see the type aliases' own comment
    // above for why the container-lifecycle maps moved off this class.
    std::tuple<AuthorityWriterMapFor<SimulatableTs>...>  m_authorityWriters;
    std::tuple<LocalInputSenderMapFor<SimulatableTs>...> m_localInputSenders;

    // [T19/T24/item 42, relocated task 79, SPLIT AT ITEM 85] THE GAME-THREAD
    // telemetry sibling — `RelayArrivalProbe`, `CorrectionVerdictProbe`,
    // `CorrectionLandingProbe` and their six `emit*` helpers. The
    // PHYSICS-THREAD sibling (`RelayReadProbe` + ten `emit*` helpers) is
    // owned by the resolution peer (`m_inputResolution` below) — see
    // `NetSyncTelemetry.h` and `InputResolutionTelemetry.h` for the probe
    // declarations, the fence on each, and each class's own two-thread rule.
    // [item 87] Diagnostics access shrinks with the split: `Diagnostics`
    // above now exposes exactly the three GAME-THREAD probes this sibling
    // owns; `relayReadProbe()` is reached directly at
    // `inputResolution.getDiagnostics().relayReadProbe()` (design §C.6).
    NetSyncTelemetry m_telemetry;

    SimulationObjectStorage<SimulatableTs...>&   m_storage;
    SimulationReconciliation<SimulatableTs...>&  m_reconciliation;

    // [item 86 / step 2, PROMOTED item 87 / step 3 of the input-resolution
    // migration, design §A.3] A real reference to the composition-root-
    // constructed resolution peer — no longer an owned scaffold sub-object.
    // NetSync knows both this peer and Reconciliation (design §A.3's
    // dependency spine); this peer and Reconciliation do not know NetSync
    // exists. Used by the per-tick/per-event methods whose signatures are
    // fixed by the manager-facing concepts (`sendCorrectionAll`,
    // `sendLocalInputToAuthorityAll`) and by the two async-callback helpers
    // (`onRelayedInputReceived`, `decideCorrectionArrival`) whose bound
    // lambdas capture only `(this, id)` and so must reach this peer through
    // a member rather than a call-time parameter. The three
    // registration/unregistration methods (`registerPredictionOwner`,
    // `registerAuthorityOwner`, `unregisterSimulatable`) instead take the
    // resolution peer as an EXPLICIT parameter (design §C.5's facade
    // signature change) — they run synchronously from the registration
    // facade, which already has the same reference in hand.
    SimulationInputResolution<SimulatableTs...>& m_inputResolution;

    std::function<void(const char*)>             m_logger;

    // Receive-side dedup guard context, pushed by SimulationManager
    // (setAuthorityGuardContext) every authority tick. Plain (non-atomic) members match
    // RemoteMoveQueue's existing single-consumer threading assumption — the authority tick
    // is refreshed once per tick and read at RPC arrival, where an at-most-one-tick-stale
    // value is fine for a multi-tick rollback window. m_rollbackWindowTicks = -1 disables
    // the future guard until SimulationManager injects TimeConfig::rollbackWindowTicks.
    uint32 m_currentAuthorityTick = 0;
    int32  m_rollbackWindowTicks  = -1;

    // [T39] THE STATE-ROTATION CURSOR. Monotonic, advanced by K once per
    // sendCorrectionAll, wrapped per type map at the point of use — see the
    // per-type wrapping note in Network/CorrectionRotation.h. Plain (non-atomic):
    // sendCorrectionAll runs on the GAME thread only, from
    // SimulationManager::onPostSimulationGameThread.
    //
    // NOT WIPED BY wipeAllForResync, and that is intentional: a hard resync jumps
    // the CLOCK, and this is a position in a round-robin over characters, not a
    // tick-keyed quantity. Re-phasing it would skip or double-write a character
    // for one round and buy nothing.
    std::size_t m_correctionRotationRound = 0u;
};

// ---------------------------------------------------------------------------
// SimulationNetSyncConcept
// ---------------------------------------------------------------------------

// [item 87 / design §C.7] `collectInputAll` / `collectResimInputAll` /
// `wipeAllForResync` LEFT this concept for `SimulationInputResolutionConcept`
// (SimulationInputResolution.h, item 83's recorded placement preference) —
// the three methods moved off this class onto the resolution peer at the
// same item. What remains is exactly NetSync's own surface: the two publish
// methods (state send, local-input RPC send) and the receive-side guard.
// `SimulatableTs` stays a template parameter for signature symmetry with the
// peer concepts even though no member below returns a SimulatableTs-typed
// value any more.
template <typename T, typename... SimulatableTs>
concept SimulationNetSyncConcept = requires(
    T& t, const SimulationTimeStep& step, uint32 tick, int32 rollbackWindow,
    int32 correctionRotationK)
{
    // [T39] sendCorrectionAll gained the state-rotation width. It is a required
    // argument rather than a defaulted one — see the note at the definition.
    { t.sendCorrectionAll(step, correctionRotationK) };
    { t.sendLocalInputToAuthorityAll(tick, tick) };
    { t.setAuthorityGuardContext(tick, rollbackWindow) };
};

// ---------------------------------------------------------------------------
// Free-function registration facade
//
// Client overload: prediction owner + optional input provider.
// Server overload: prediction owner + authority owner (no input provider needed
//   — authority reads from the inbound remote-move queue).
//
// Owner types are resolved via SimulatableOwnerTraits<SimulatableT>;
// callers never name the owner template parameters directly.
// ---------------------------------------------------------------------------

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
    // Order matters: cache must exist before storage, because the physics thread
    // iterates m_storage.forEachSimulatable and looks up each id in the cache map
    // (postPredictionAll etc.). If storage gets the id first, a concurrent physics
    // tick sees storage-has-id and calls getCacheFor(id), which throws.
    // Inverted-order invariant: if storage has id, cache has id.
    // [item 92] THE SAME SHAPE OF INVARIANT IS ENFORCED BELOW, IN THE SERVER
    // OVERLOAD — that overload has no cache to create, so it orders
    // registerAuthorityOwner (the call that lets sweep 2 skip an authority id)
    // before storage.add instead. State both so a future edit to either overload
    // cannot break its ordering without seeing the sibling invariant it mirrors.
    // [item 93] THE SAME INVARIANT, READ BACKWARDS, GOVERNS unregisterSimulatable
    // below (publish-last on the way in ⇒ unpublish-first on the way out).
    reconciliation.template createCacheFor<SimulatableT>(id);
    storage.template add<SimulatableT>(id, std::forward<SimulatableT>(simulatable));
    // [item 87 / design §C.5] `inputResolution` gained by this facade's own
    // signature — the resolution peer's containers must exist before
    // `registerPredictionOwner` can bind callbacks against them, same order,
    // same reason as the cache-before-storage invariant above, now spanning
    // this extra call.
    netSync.template registerPredictionOwner<SimulatableT>(
        id, owner, std::move(inputProvider), inputResolution);
}

// Server overload — no correction cache is allocated: the authority does not
// predict, resim, or reconcile, so it has no need for per-simulatable state
// history. NetSync alone handles the outbound correction send and inbound
// remote-move queue.
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
    // [item 92, RE-CHECKED AT ITEM 94] Order matters, mirroring the client
    // overload's cache-before-storage invariant above: this overload has no
    // cache to create, but registerAuthorityOwner still SHOULD run before
    // storage.add. [item 94] ⚠ WHAT THIS ORDERING NOW PROTECTS HAS CHANGED —
    // priced explicitly rather than left stale. `queueMap` is no longer read
    // by the frontier-allocation sweep at all: `SimulationReconciliation::
    // allocateFrontierSlotsAll` filters on ITS OWN cache population
    // (`findInputCache != nullptr`), not on this class's `queueMap`, so a
    // storage-exposed id with no cache is now SILENTLY SKIPPED there rather
    // than falling through to a throwing `.at(id)` — item 92's loud
    // `OG_CHECK` guard was deleted along with the resolution-side sweep that
    // carried it (`allocateFrontierSlotForCharacter`, item 94). What THIS
    // ordering still protects is sweep 1's `queueMap`-based branch dispatch
    // in `SimulationInputResolution::collectInputForCharacter`: an id exposed
    // to storage before `queueMap` is populated would misclassify as a
    // simulated-proxy id instead of an authority id for the width of the
    // window, reading a relay store that will never exist for it rather than
    // its remote-move queue — a real, lower-severity defect than the retired
    // crash, and the reason this reorder is BELT-AND-BRACES now, not
    // unnecessary. Neither registerPredictionOwner (provider is null here, so
    // it takes the provider-absent/remote branch) nor registerAuthorityOwner
    // touches `storage` or the stored `simulatable`, so both are safe to run
    // first regardless.
    // Inverted-order invariant: if storage has id, queueMap has id.
    // [item 93] THE SAME INVARIANT, READ BACKWARDS, GOVERNS unregisterSimulatable
    // below (publish-last on the way in ⇒ unpublish-first on the way out).
    netSync.template registerPredictionOwner<SimulatableT>(
        id, predictionOwner, nullptr, inputResolution);
    netSync.template registerAuthorityOwner<SimulatableT>(id, authorityOwner, inputResolution);
    storage.template add<SimulatableT>(id, std::forward<SimulatableT>(simulatable));
}

// Unregister facade — mirrors registration.
//
// [item 93] REGISTRATION PUBLISHES LAST, SO UNREGISTRATION MUST UNPUBLISH
// FIRST — the two register overloads above establish "if storage has id,
// queueMap/cache has id" by making storage.add the LAST call (queueMap/cache
// is already populated the instant the id becomes visible to
// storage.forEachSimulatable). Unregistration is the same invariant read
// backwards: the id must stop being visible to forEachSimulatable — i.e.
// leave storage — BEFORE anything erases the queueMap/cache entries that
// gate what a concurrent physics-thread sweep does with it. So
// storage.remove<SimulatableT> runs FIRST here, ahead of
// netSync.unregisterSimulatable (whose step 3 erases queueMap via
// SimulationInputResolution::unregisterCharacter) and reconciliation's
// removeCacheFor. This was previously reversed — storage.remove ran AFTER
// netSync.unregisterSimulatable — which left the id visible in storage
// with an already-erased queueMap entry for the width of that call: exactly
// the item-92 crash shape (sweep 2's queueMap guard passes, then
// allocateFrontierSlotForCharacter's OG_CHECK aborts / getCacheFor's
// bare .at(id) throws), reachable on the authority path on player-leave.
// ⚠ The comment previously on this facade already CLAIMED "remove from
// storage before cache" — but the code below it did the opposite (netSync's
// queueMap-erasing call ran first). The claim was aspirational, never
// implemented; corrected here rather than just centered on the code.
//
// [item 93] CLIENT-PATH SYMMETRY, CONFIRMED NOT ASSUMED: the client register
// overload creates the correction cache BEFORE storage.add, so by the same
// rule the cache must be removed AFTER storage.remove on the way out.
// Placing reconciliation.removeCacheFor LAST (after both storage.remove and
// netSync.unregisterSimulatable) satisfies that — read
// SimulationReconciliation::removeCacheFor and NetSync::unregisterSimulatable's
// own body: neither touches `storage`, so nothing here depends on the cache
// or the queueMap/telemetry maps outliving storage.remove. This ordering was
// already correct before this fix (removeCacheFor was already the last of
// the three calls) and remains correct now; only the storage/netSync
// relative order changes.
//
// [item 94, Part F] ⚠ THIS REORDER IS LANDED AND MUST NOT BE "SIMPLIFIED"
// BACK — stated explicitly because task 94 removes the reorder's ORIGINAL
// motivation. Frontier allocation is now storage-driven and filtered on
// reconciliation's own cache population (`SimulationReconciliation::
// allocateFrontierSlotsAll`), so it no longer falls through to the item-92
// `OG_CHECK` / bare `.at(id)` throw this reorder was built to avoid — that
// specific crash shape is retired by task 94's own existence, independent of
// this ordering. That makes this reorder BELT-AND-BRACES, not unnecessary:
// it still keeps sweep 1's `queueMap`-based branch dispatch in
// `SimulationInputResolution::collectInputForCharacter` correct across the
// teardown window (see the server `registerSimulatable` overload's own
// item-94 paragraph, above, for the identical argument on the registration
// side). A future author must not remove this reorder on the grounds that
// "the crash it was fixing can't happen any more" — a DIFFERENT, real hazard
// depends on it now.
//
// [item 94, Part F] THE MIRRORED INVARIANT, RE-CHECKED POST-94. "If storage
// has id, queueMap/cache has id" (registration) / the reverse (teardown)
// STILL HOLDS, and the per-tick TOLERANCE for a momentary violation is
// STRICTLY WIDER than it was pre-94: previously only sweep 1 (queueMap-keyed
// branch dispatch) tolerated the window silently; sweep 2 (frontier
// allocation) did not, and that intolerance is exactly what item 92 patched
// with a loud guard. Post-94, BOTH sweeps are nullable/storage-filtered — the
// allocation sweep now degrades the same way sweep 1 always has (a quiet
// skip or a plausible-but-wrong branch choice, never a crash). The guarantee
// this ordering states is therefore: *no path from a fully-executed
// register/unregister sequence, in the stated order, ever produces a window
// visible to a concurrent physics tick* — unchanged in its literal text, but
// now backed by two tolerant sweeps instead of one tolerant and one
// abort-on-violation.
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
