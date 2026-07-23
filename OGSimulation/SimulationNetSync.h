#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include <atomic>
#include <concepts>
#include <functional>
#include <tuple>
#include <unordered_map>

#include "OGSimulation/Network/ClientInputDelayLine.h"
#include "OGSimulation/SimulationLog.h"
#include "OGSimulation/SimulationObjectStorage.h"
#include "OGSimulation/SimulationQueues.h"
#include "OGSimulation/SimulationReconciliation.h"
#include "OGSimulation/SimulationTimeContext.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
#pragma optimize("", off)

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
// Per-type map aliases for SimulationNetSync members
// ---------------------------------------------------------------------------

template <typename T>
using InputProviderMapFor = std::unordered_map<
    unsigned int,
    std::function<typename T::InputType(const SimulationTimeStep&)>>;

template <typename T>
using RemoteMoveQueueMapFor = std::unordered_map<
    unsigned int,
    RemoteMoveQueue<typename T::InputType>>;

template <typename T>
using PendingInputQueueMapFor = std::unordered_map<
    unsigned int,
    PendingInputQueue<typename T::InputType>>;

template <typename T>
using LastUsedInputMapFor = std::unordered_map<unsigned int, typename T::InputType>;

// [T9 parts 3+4] Per-locally-controlled-simulatable ring of the client's own raw
// input captures. Populated ONLY for ids that have an input provider, i.e. the
// exact set for which m_pendingInputQueues is populated. See
// Network/ClientInputDelayLine.h for why this is a separate structure from the
// correction cache — the short version is that the cache's slot T already means
// "input APPLIED at tick T" and resim reads it with no offset.
template <typename T>
using ClientInputDelayLineMapFor = std::unordered_map<
    unsigned int,
    ClientInputDelayLine<typename T::InputType>>;

// Per-SIMULATABLE-TYPE (not per-id) neutral input. Wrapped in a struct keyed on
// the simulatable rather than stored as a bare `InputType` so that a pack whose
// members happen to share one InputType still gets distinct tuple slots —
// `std::get<InputType>` would be ill-formed there, and silently so at the
// template level until such a pack first appeared.
template <typename T>
struct NeutralInputFor
{
    typename T::InputType value{};
};

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
// SimulationReconciliation::injectCorrectionState / injectCorrectionInput +
// SimulationNetSync::registerAuthorityOwner RPC callback (read side).
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

template <typename OwnerT, typename StateT, typename InputT>
concept PredictionSyncedBufferOwnerConcept =
    requires(OwnerT& owner,
             std::function<void(const typename OwnerT::SyncedCorrectionBufferType&)> corrFn,
             std::function<void(const typename OwnerT::SyncedRemoteInputBufferType&)> inputFn,
             const PendingInputQueue<InputT>& pendingQueue,
             uint32 currentTick,
             uint32 redundancyDepth)
    {
        typename OwnerT::SyncedCorrectionBufferType;
        typename OwnerT::SyncedRemoteInputBufferType;
        requires CompositeSyncedBufferConcept<typename OwnerT::SyncedCorrectionBufferType, StateT>;
        requires CompositeSyncedBufferConcept<typename OwnerT::SyncedRemoteInputBufferType, InputT>;
        { owner.setOnCorrectionStateReceivedCallback(corrFn) };
        { owner.setOnCorrectionInputReceivedCallback(inputFn) };
        { owner.clearOnCorrectionStateReceivedCallback() };
        { owner.clearOnCorrectionInputReceivedCallback() };
        { owner.getClientToServerInputSyncedBuffer() } -> std::same_as<typename OwnerT::SyncedRemoteInputBufferType*>;
        // The local-input RPC is the unreliable + redundancy FInputRedundancyBundle
        // path. The owner builds the bundle (a UE wire type opaque to this UE-free
        // layer) from the most-recent `redundancyDepth` ticks still held in
        // `pendingQueue` and fires the unreliable RPC. The bundle type never appears
        // here — only the core PendingInputQueue + scalar params do.
        { owner.sendLocalInputToAuthority(pendingQueue, currentTick, redundancyDepth) };
    };

template <typename OwnerT, typename StateT, typename InputT>
concept AuthoritySyncedBufferOwnerConcept =
    requires(OwnerT& owner,
             std::function<void(uint32, const InputT&)> fn)
    {
        typename OwnerT::SyncedRemoteInputBufferType;
        requires CompositeSyncedBufferConcept<typename OwnerT::SyncedRemoteInputBufferType, InputT>;
        { owner.getSyncedCorrectionStateBuffer() } -> CompositeSyncedBufferConcept<StateT>;
        { owner.getSyncedCorrectionInputBuffer() } -> CompositeSyncedBufferConcept<InputT>;
        // The remote-move callback is per-slot. The owner unpacks the inbound
        // FInputRedundancyBundle and invokes this callback once per
        // (capture_tick, input) slot — the bundle type stays UE-side.
        { owner.setOnRemoteMoveReceivedCallback(fn) };
        { owner.clearOnRemoteMoveReceivedCallback() };
    };

// ---------------------------------------------------------------------------
// SimulationNetSync<SimulatableTs...>
//
// Owns all per-type transport state: input providers, remote-move queues,
// pending-input queues, last-used inputs. Owns per-type owner-binding maps
// (AuthorityWriter, LocalInputSender) as concrete pointer structs (no std::function
// on the hot path). Holds refs to SimulationObjectStorage and SimulationReconciliation.
//
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
// ---------------------------------------------------------------------------

template <typename... SimulatableTs>
class SimulationNetSync
{
public:
    SimulationNetSync(
        SimulationObjectStorage<SimulatableTs...>& storage,
        SimulationReconciliation<SimulatableTs...>& reconciliation)
        : m_storage(storage)
        , m_reconciliation(reconciliation)
    {}

    void setLogger(std::function<void(const char*)> logger)
    {
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

    // -----------------------------------------------------------------------
    // [T9 part 3] Client-side Layer-1 input delay
    // -----------------------------------------------------------------------
    //
    // The number of ticks the LOCAL client's own captured input is held before
    // the integrator sees it. One scalar, not a per-id map, because a delay is a
    // property of the local CONNECTION and a client has exactly one — the same
    // reason the server keys its queue by Address rather than by simulatable.
    //
    // THE GAME->PHYSICS THREAD CROSSING, and why an atomic is sufficient here.
    // The writer is USimmableUpdateComponent::OnRep_ConnectionTier on the GAME
    // thread; the reader is collectInputAll on the PHYSICS thread, which loads it
    // ONCE per tick and uses that one value for the whole tick. This is safe
    // precisely because it is a lone independent scalar with no internal
    // structure: the worst a relaxed, one-tick-stale read can do is apply a tier
    // change one tick later than it landed. It is emphatically NOT the same
    // situation as T10's ServerInputDelayQueue, where the shared thing was an
    // unordered_map whose concurrent rehash is undefined behaviour rather than a
    // stale value — which is why that one needed the R2 restructuring and this
    // one does not.
    //
    // Relaxed ordering is deliberate: there is no other datum whose visibility
    // this value has to order, so acquire/release would buy nothing.
    //
    // Defaults to 0 = "no delay", i.e. exact pre-T9 behaviour, so an instance
    // nobody configures (isolated unit tests, the authority-role manager) is
    // unaffected.
    void setClientEffectiveInputDelayTicks(int32 delayTicks)
    {
        m_clientEffectiveInputDelayTicks.store(delayTicks < 0 ? 0 : delayTicks,
                                               std::memory_order_relaxed);
    }

    int32 getClientEffectiveInputDelayTicks() const
    {
        return m_clientEffectiveInputDelayTicks.load(std::memory_order_relaxed);
    }

    // [T9 part 4] Inject the game's zero input, used to fill the
    // [0, effectiveDelay) window at session start and after a resync wipe.
    //
    // MUST be called by the composition root: the default `InputType{}` is NOT
    // the brawler's zero input (`getZeroPlayerInput` builds (0,0,1) forward
    // vectors, a value-initialised one would carry (0,0,0) into normalisation).
    // Applies to delay lines created later AND to any already created, so it is
    // order-independent with respect to registration.
    template <typename SimulatableT>
    void setNeutralInput(const typename SimulatableT::InputType& neutralInput)
    {
        std::get<NeutralInputFor<SimulatableT>>(m_clientNeutralInputs).value = neutralInput;
        for (auto& [id, line] :
             std::get<ClientInputDelayLineMapFor<SimulatableT>>(m_clientInputDelayLines))
        {
            line.setNeutralInput(neutralInput);
        }
    }

    // -----------------------------------------------------------------------
    // Registration — free functions delegate here; not called directly by users
    // -----------------------------------------------------------------------

    // Client overload registration helpers — called from registerSimulatable free function.
    template <typename SimulatableT>
        requires PredictionSyncedBufferOwnerConcept<
            PredictionOwnerFor<SimulatableT>,
            typename SimulatableT::StateType,
            typename SimulatableT::InputType>
    void registerPredictionOwner(
        unsigned int id,
        PredictionOwnerFor<SimulatableT>& predictionOwner,
        std::function<typename SimulatableT::InputType(const SimulationTimeStep&)> inputProvider)
    {
        // Input provider is present iff this owner drives a locally-controlled
        // simulatable. Keep m_inputProviders / m_pendingInputQueues / m_localInputSenders
        // populated as a set: sendLocalInputToAuthorityAll iterates m_localInputSenders
        // and looks up m_pendingInputQueues by id, so the three maps must agree.
        if (inputProvider)
        {
            std::get<InputProviderMapFor<SimulatableT>>(m_inputProviders)
                .emplace(id, std::move(inputProvider));

            // try_emplace default-constructs in place — PendingInputQueue
            // holds std::atomic members and is neither copyable nor movable.
            std::get<PendingInputQueueMapFor<SimulatableT>>(m_pendingInputQueues)
                .try_emplace(id);

            // [T9 part 3] The delay line is populated for exactly the ids that
            // have a provider — the locally-controlled ones. A remote proxy has
            // no capture of its own to delay, so it must not get a line, and
            // collectInputAll's proxy branch is left untouched.
            std::get<ClientInputDelayLineMapFor<SimulatableT>>(m_clientInputDelayLines)
                .try_emplace(id,
                    std::get<NeutralInputFor<SimulatableT>>(m_clientNeutralInputs).value);

            std::get<LocalInputSenderMapFor<SimulatableT>>(m_localInputSenders)
                .emplace(id, LocalInputSender<SimulatableT>{ &predictionOwner });
        }

        predictionOwner.setOnCorrectionStateReceivedCallback(
            [this, id](const typename PredictionOwnerFor<SimulatableT>::SyncedCorrectionBufferType& buffer) {
                m_reconciliation.template injectCorrectionState<SimulatableT>(id, buffer);
            });

        predictionOwner.setOnCorrectionInputReceivedCallback(
            [this, id](const typename PredictionOwnerFor<SimulatableT>::SyncedRemoteInputBufferType& buffer) {
                m_reconciliation.template injectCorrectionInput<SimulatableT>(id, buffer);
            });
    }

    // Server overload registration helper.
    template <typename SimulatableT>
        requires AuthoritySyncedBufferOwnerConcept<
            AuthorityOwnerFor<SimulatableT>,
            typename SimulatableT::StateType,
            typename SimulatableT::InputType>
    void registerAuthorityOwner(unsigned int id, AuthorityOwnerFor<SimulatableT>& authorityOwner)
    {
        std::get<RemoteMoveQueueMapFor<SimulatableT>>(m_remoteMoveQueues)
            .emplace(id, RemoteMoveQueue<typename SimulatableT::InputType>{});

        std::get<LastUsedInputMapFor<SimulatableT>>(m_lastUsedInputs)
            .emplace(id, typename SimulatableT::InputType{});

        // RPC inbound — lambda captures ref into m_remoteMoveQueues.
        // Cleared in unregisterSimulatable before data-map erasure (see ordering comment there).
        auto& remoteQueue = std::get<RemoteMoveQueueMapFor<SimulatableT>>(m_remoteMoveQueues).at(id);
        // Per-slot inbound callback: the owner walks the inbound
        // FInputRedundancyBundle and invokes this once per (capture_tick, input).
        // The queue dedups by capture_tick (first-writer-wins)
        // and rejects too-far-future capture ticks against the guard context published by
        // SimulationManager via setAuthorityGuardContext (current authority tick +
        // TimeConfig::rollbackWindowTicks). A too-far-future drop is warned here so the
        // queue stays logger-free.
        authorityOwner.setOnRemoteMoveReceivedCallback(
            [this, id, &remoteQueue](uint32 tick, const typename SimulatableT::InputType& input) {
                SIMLOG(m_logger, "[ReceiveLocalInput] id=%u tick=%u", id, tick);
                typename SimulatableT::InputType copy = input;
                const QueueMoveResult result = remoteQueue.queueMove(
                    std::move(copy), tick, m_currentAuthorityTick, m_rollbackWindowTicks);
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
    // Step 1 MUST precede step 3: onRemoteMoveReceived captures &remoteQueue from
    // m_remoteMoveQueues. Clear RPC-inbound callbacks before erasing data maps,
    // or the cleared lambda may fire against a dangling queue reference.
    template <typename T>
    void unregisterSimulatable(
        unsigned int id,
        PredictionOwnerFor<T>* predictionOwner,
        AuthorityOwnerFor<T>* authorityOwner = nullptr)
    {
        // Step 1: clear RPC-inbound callbacks on the owner(s) — before any data-map erase.
        if (predictionOwner)
        {
            predictionOwner->clearOnCorrectionStateReceivedCallback();
            predictionOwner->clearOnCorrectionInputReceivedCallback();
        }
        if (authorityOwner)
        {
            authorityOwner->clearOnRemoteMoveReceivedCallback();
        }

        // Step 2: erase writer structs.
        std::get<AuthorityWriterMapFor<T>>(m_authorityWriters).erase(id);
        std::get<LocalInputSenderMapFor<T>>(m_localInputSenders).erase(id);

        // Step 3: erase data maps.
        std::get<InputProviderMapFor<T>>(m_inputProviders).erase(id);
        std::get<RemoteMoveQueueMapFor<T>>(m_remoteMoveQueues).erase(id);
        std::get<PendingInputQueueMapFor<T>>(m_pendingInputQueues).erase(id);
        std::get<ClientInputDelayLineMapFor<T>>(m_clientInputDelayLines).erase(id);
        std::get<LastUsedInputMapFor<T>>(m_lastUsedInputs).erase(id);
    }

    // -----------------------------------------------------------------------
    // Per-tick input resolution (physics thread)
    // -----------------------------------------------------------------------

    ResolvedInputs<SimulatableTs...> collectInputAll(const SimulationTimeStep& step)
    {
        ResolvedInputs<SimulatableTs...> inputs;

        // [T9 part 3] ONE load per tick, used for every locally-controlled
        // simulatable below. Reading it once (rather than per id) is what makes
        // a concurrent OnRep_ConnectionTier write unable to split a single tick
        // across two different delays.
        const int32 effectiveDelay = getClientEffectiveInputDelayTicks();

        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            auto& providerMap   = std::get<InputProviderMapFor<T>>(m_inputProviders);
            auto& queueMap      = std::get<RemoteMoveQueueMapFor<T>>(m_remoteMoveQueues);
            auto& map           = std::get<std::unordered_map<unsigned int, typename T::InputType>>(inputs);

            if (auto it = providerMap.find(id); it != providerMap.end())
            {
                // The RAW capture, as the local player produced it THIS tick.
                const auto capture = it->second(step);

                // [T9 parts 3+4] Layer-1 client input delay.
                //
                // Two different values leave this branch and the distinction is
                // the whole point of the task:
                //
                //   `capture` — the ORIGINAL, undelayed input, stamped at the
                //     CURRENT tick. This is what goes on the wire. The server
                //     parks it in its own ServerInputDelayQueue and applies the
                //     same delay itself; sending an already-delayed input would
                //     have the server delay it a SECOND time and the two ends
                //     would diverge by exactly `effectiveDelay` ticks.
                //
                //   `applied`  — the capture from `tick - effectiveDelay`. This
                //     is what the integrator predicts with and what goes into
                //     the correction cache, because the cache's slot T means
                //     "the input APPLIED at tick T" — the same meaning the
                //     server's replicated correction writes into that slot, and
                //     the meaning collectResimInputAll relies on when it reads
                //     those slots with no offset.
                //
                // With `effectiveDelay == 0` this is exactly the pre-T9 path:
                // resolveDelayedInput returns the live capture untouched.
                auto& delayLine =
                    std::get<ClientInputDelayLineMapFor<T>>(m_clientInputDelayLines).at(id);

                if (step.getStepKind() != StepKind::Stall)
                {
                    delayLine.push(static_cast<int32>(step.getTick()), capture);
                }

                typename T::InputType applied = resolveDelayedInput(
                    delayLine, static_cast<int32>(step.getTick()), effectiveDelay, capture);

                SIMLOG(m_logger,
                    "[CollectInput] id=%u tick=%u source=Provider kind=%s delay=%d",
                    id, step.getTick(), stepKindName(step.getStepKind()), effectiveDelay);

                if (step.getStepKind() == StepKind::Skip)
                    m_reconciliation.template backfillSkippedTick<T>(
                        id, step.getTick() - 1, simulatable.getAllState().getState());

                if (step.getStepKind() != StepKind::Stall)
                {
                    m_reconciliation.template pushPredictionTick<T>(id, step.getTick());
                    m_reconciliation.template pushPredictionInput<T>(id, applied);
                    // ORIGINAL capture, current tick — see above. Do not pass
                    // `applied` here.
                    std::get<PendingInputQueueMapFor<T>>(m_pendingInputQueues)
                        .at(id).enqueue(step.getTick(), capture);
                }

                map.emplace(id, std::move(applied));
            }
            else if (auto qit = queueMap.find(id); qit != queueMap.end())
            {
                auto move = qit->second.dequeueMove();
                SIMLOG(m_logger, "[CollectInput] id=%u tick=%u source=RemoteQueue queuedTick=%u",
                    id, step.getTick(), move.tick);
                std::get<LastUsedInputMapFor<T>>(m_lastUsedInputs).at(id) = move.input;
                map.emplace(id, std::move(move.input));
            }
            else
            {
                // Simulated-proxy branch (client predicting a remote character).
                // Resolve input from the cache's last server-reported input, then
                // advance the cache's prediction tick in lockstep with the provider
                // branch — otherwise postPredictionAll keeps overwriting a stale
                // tick slot, every correction lands outside the cache window, and
                // getLastCorrectionInput never refreshes.
                auto cached = m_reconciliation.template getLastCorrectionInput<T>(id);
                typename T::InputType input = cached.value_or(typename T::InputType{});

                SIMLOG(m_logger, "[CollectInput] id=%u tick=%u source=Cache found=%d",
                    id, step.getTick(), cached.has_value() ? 1 : 0);

                if (step.getStepKind() == StepKind::Skip)
                    m_reconciliation.template backfillSkippedTick<T>(
                        id, step.getTick() - 1, simulatable.getAllState().getState());

                if (step.getStepKind() != StepKind::Stall)
                {
                    m_reconciliation.template pushPredictionTick<T>(id, step.getTick());
                    m_reconciliation.template pushPredictionInput<T>(id, input);
                }

                map.emplace(id, std::move(input));
            }
        });
        return inputs;
    }

    // -----------------------------------------------------------------------
    // Outbound — authority state/input replication (game thread)
    // -----------------------------------------------------------------------

    void sendCorrectionAll(const SimulationTimeStep& step)
    {
        // Wire format is fully encapsulated by the buffer's write(composite, tick).
        // Must stay in lockstep with the client-side readInto() in
        // SimulationReconciliation::injectCorrectionState / injectCorrectionInput.
        const uint32 tick = step.getTick();
        forEachTypeMap(m_authorityWriters, [&]<typename T>(auto& perTypeMap) {
            for (auto& [id, w] : perTypeMap)
            {
                auto& stored    = m_storage.template get<T>(id);
                auto& lastInput = std::get<LastUsedInputMapFor<T>>(m_lastUsedInputs).at(id);
                SIMLOG(m_logger, "[SendCorrectionStateToClients] id=%u tick=%u", id, tick);
                w.owner->getSyncedCorrectionStateBuffer().write(stored.getAllState().getState(), tick);
                SIMLOG(m_logger, "[SendRemoteInputToClients] id=%u tick=%u", id, tick);
                w.owner->getSyncedCorrectionInputBuffer().write(lastInput, tick);
            }
        });
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
        forEachTypeMap(m_localInputSenders, [&]<typename T>(auto& perTypeMap) {
            auto& pendingQueueMap = std::get<PendingInputQueueMapFor<T>>(m_pendingInputQueues);
            for (auto& [id, s] : perTypeMap)
            {
                auto& pendingQueue = pendingQueueMap.at(id);
                SIMLOG(m_logger, "[SendLocalInputToServer] id=%u tick=%u depth=%u",
                    id, currentTick, redundancyDepth);
                s.owner->sendLocalInputToAuthority(pendingQueue, currentTick, redundancyDepth);
                pendingQueue.releaseAllButRecent(static_cast<size_t>(redundancyDepth));
            }
        });
    }

    // Drops any queued local inputs that were produced against the pre-resync
    // prediction clock. Invoked from the ClientPredictionClock resync callback
    // alongside the reconciliation cache wipe.
    void wipeAllForResync(uint32 newPredictionTick)
    {
        forEachTypeMap(m_pendingInputQueues, [&]<typename T>(auto& perTypeMap) {
            for (auto& [id, queue] : perTypeMap)
            {
                SIMLOG(m_logger,
                    "[TimeResync.WipeInputQueue] id=%u newPredictionTick=%u",
                    id, newPredictionTick);
                queue.clear();
            }
        });

        // [T9 part 3] The delay line is keyed by CAPTURE TICK against the
        // pre-resync prediction clock. After a hard resync that clock has jumped,
        // so those keys describe ticks that no longer mean what they meant, and a
        // surviving capture would be read at the wrong tick for `effectiveDelay`
        // ticks. Dropping them re-enters the neutral-filled window — the same
        // well-defined state as session start (part 4), which is exactly why part
        // 4 is not special-cased to tick 0.
        forEachTypeMap(m_clientInputDelayLines, [&]<typename T>(auto& perTypeMap) {
            for (auto& [id, line] : perTypeMap)
            {
                SIMLOG(m_logger,
                    "[TimeResync.WipeInputDelayLine] id=%u newPredictionTick=%u",
                    id, newPredictionTick);
                line.clear();
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

    // m_inputProviders uses std::function intentionally — converting to a pointer struct
    // would require extending PredictionSyncedBufferOwnerConcept with a typed
    // getLocalInputFor<T>(step) method; tracked as a post-cutover follow-up.
    std::tuple<InputProviderMapFor<SimulatableTs>...>    m_inputProviders;
    std::tuple<RemoteMoveQueueMapFor<SimulatableTs>...>  m_remoteMoveQueues;
    std::tuple<PendingInputQueueMapFor<SimulatableTs>...> m_pendingInputQueues;
    std::tuple<LastUsedInputMapFor<SimulatableTs>...>    m_lastUsedInputs;
    std::tuple<AuthorityWriterMapFor<SimulatableTs>...>  m_authorityWriters;
    std::tuple<LocalInputSenderMapFor<SimulatableTs>...> m_localInputSenders;

    // [T9 parts 3+4] Client Layer-1 input delay. Populated for provider-owning
    // ids only; touched exclusively from collectInputAll (physics thread) and
    // wipeAllForResync.
    std::tuple<ClientInputDelayLineMapFor<SimulatableTs>...> m_clientInputDelayLines;
    std::tuple<NeutralInputFor<SimulatableTs>...>            m_clientNeutralInputs;

    SimulationObjectStorage<SimulatableTs...>&   m_storage;
    SimulationReconciliation<SimulatableTs...>&  m_reconciliation;
    std::function<void(const char*)>             m_logger;

    // Receive-side dedup guard context, pushed by SimulationManager
    // (setAuthorityGuardContext) every authority tick. Plain (non-atomic) members match
    // RemoteMoveQueue's existing single-consumer threading assumption — the authority tick
    // is refreshed once per tick and read at RPC arrival, where an at-most-one-tick-stale
    // value is fine for a multi-tick rollback window. m_rollbackWindowTicks = -1 disables
    // the future guard until SimulationManager injects TimeConfig::rollbackWindowTicks.
    uint32 m_currentAuthorityTick = 0;
    int32  m_rollbackWindowTicks  = -1;

    // [T9 part 3] Written on the GAME thread (OnRep_ConnectionTier), read once
    // per tick on the PHYSICS thread. Atomic — and ONLY atomic — for the reasons
    // spelled out on setClientEffectiveInputDelayTicks. 0 = pre-T9 behaviour.
    std::atomic<int32> m_clientEffectiveInputDelayTicks{ 0 };
};

// ---------------------------------------------------------------------------
// SimulationNetSyncConcept
// ---------------------------------------------------------------------------

template <typename T, typename... SimulatableTs>
concept SimulationNetSyncConcept = requires(
    T& t, const SimulationTimeStep& step, uint32 tick, int32 rollbackWindow)
{
    { t.sendCorrectionAll(step) };
    { t.sendLocalInputToAuthorityAll(tick, tick) };
    { t.collectInputAll(step) } -> std::convertible_to<ResolvedInputs<SimulatableTs...>>;
    { t.wipeAllForResync(tick) };
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
    SimulationNetSync<Ts...>&              netSync,
    unsigned int                           id,
    SimulatableT&&                         simulatable,
    PredictionOwnerFor<SimulatableT>&      owner,
    std::function<typename SimulatableT::InputType(const SimulationTimeStep&)> inputProvider = nullptr)
{
    // Order matters: cache must exist before storage, because the physics thread
    // iterates m_storage.forEachSimulatable and looks up each id in the cache map
    // (postPredictionAll etc.). If storage gets the id first, a concurrent physics
    // tick sees storage-has-id and calls getCacheFor(id), which throws.
    // Inverted-order invariant: if storage has id, cache has id.
    reconciliation.template createCacheFor<SimulatableT>(id);
    storage.template add<SimulatableT>(id, std::forward<SimulatableT>(simulatable));
    netSync.template registerPredictionOwner<SimulatableT>(id, owner, std::move(inputProvider));
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
    SimulationNetSync<Ts...>&              netSync,
    unsigned int                           id,
    SimulatableT&&                         simulatable,
    PredictionOwnerFor<SimulatableT>&      predictionOwner,
    AuthorityOwnerFor<SimulatableT>&       authorityOwner)
{
    storage.template add<SimulatableT>(id, std::forward<SimulatableT>(simulatable));
    netSync.template registerPredictionOwner<SimulatableT>(id, predictionOwner, nullptr);
    netSync.template registerAuthorityOwner<SimulatableT>(id, authorityOwner);
}

// Unregister facade — mirrors registration; clears callbacks before data-map erasure.
template <typename SimulatableT, typename... Ts>
void unregisterSimulatable(
    SimulationObjectStorage<Ts...>&   storage,
    SimulationReconciliation<Ts...>&  reconciliation,
    SimulationNetSync<Ts...>&         netSync,
    unsigned int                      id,
    PredictionOwnerFor<SimulatableT>* predictionOwner,
    AuthorityOwnerFor<SimulatableT>*  authorityOwner = nullptr)
{
    // Symmetric to register ordering: remove from storage before cache, so a
    // physics tick racing this teardown never sees storage-has-id without a
    // corresponding cache entry. Preserves the invariant "if storage has id,
    // cache has id" across both lifecycle directions.
    netSync.template unregisterSimulatable<SimulatableT>(id, predictionOwner, authorityOwner);
    storage.template remove<SimulatableT>(id);
    reconciliation.template removeCacheFor<SimulatableT>(id);
}

#pragma optimize("", on)
// pragma optimize on.
