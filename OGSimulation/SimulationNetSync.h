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
#include "OGSimulation/Network/ClientInputDelayLine.h"
#include "OGSimulation/Network/CorrectionRotation.h"
#include "OGSimulation/Network/CorrectionVerdictProbe.h"
// [og-netcode-v2-input-relay item 42] The game-thread half of the resim-gate
// telemetry — where each landed correction sat relative to the prediction
// frontier. Fed from the same OnRep-bound callback as the verdict probe.
#include "OGSimulation/ResimGateProbe.h"
#include "OGSimulation/Network/RelayReadProbe.h"
#include "OGSimulation/Network/RelayedInputStore.h"
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

// [T15 / input relay] THE PROVIDER TAKES THE DELAY LINE AS A PARAMETER.
//
// The second argument is the character's own ClientInputDelayLine — its raw
// capture history, keyed by capture tick. It is here because the motion-sequence
// matcher (the game's Hadouken detector, which runs INSIDE the provider) needs
// contiguous raw captures up to `tick - 1`, and passing them in is what makes
// the read ordering statable in one place rather than as a cross-file contract.
//
// THE ORDERING CONTRACT, AND WHY IT IS NOW VISIBLE. collectInputAll looks the
// line up and calls the provider BEFORE it pushes this tick's capture. So the
// line a provider receives holds ticks <= `step.getTick() - 1`, never the
// current tick, and the current tick's sample is the value the provider is in
// the middle of producing. Both halves of that statement are now in ONE
// function; before, the provider reached back for the history itself and nothing
// at either site said it must not observe the current tick — it held only
// because one line happened to precede another.
//
// The reference is const: a provider reads history, it never writes it.
template <typename T>
using InputProviderMapFor = std::unordered_map<
    unsigned int,
    std::function<typename T::InputType(const SimulationTimeStep&,
                                        const ClientInputDelayLine<typename T::InputType>&)>>;

template <typename T>
using RemoteMoveQueueMapFor = std::unordered_map<
    unsigned int,
    RemoteMoveQueue<typename T::InputType>>;

template <typename T>
using PendingInputQueueMapFor = std::unordered_map<
    unsigned int,
    PendingInputQueue<typename T::InputType>>;

// [og-netcode-v2-input-relay T8] `LastUsedInputMapFor` / `m_lastUsedInputs` — the
// authority's per-id record of "the input I applied on my most recent tick" — ARE
// GONE. That record existed for exactly one purpose: sendCorrectionAll replicated
// it on the correction-INPUT channel. With that write removed it had zero readers
// and was pure write-only bookkeeping on the server's hot path (a hash lookup and
// a composite copy per character per authority tick, feeding nothing).
//
// DO NOT CONFUSE IT WITH `LastUsedCaptureTickMapFor` / `m_lastUsedCaptureTicks`
// BELOW, WHICH STAYS. That one shares the retired map's key set and its
// population/erase sites, but it serves T2's applied-capture-tick REF — the join
// key the relay is built on, published in the correction STATE payload and
// consumed by T6's resim resolution. It is load-bearing.

// ---------------------------------------------------------------------------
// [T2 / input relay] kNoInputCaptureTick — the "no real input was applied"
// sentinel for the applied-capture-tick track below — now lives in
// OGSimulation/CorrectionStateBufferCodec.h (included above), because T4 made it
// a WIRE value shared by three layers that must not depend on this header: the
// correction-state payload, the correction cache's per-slot ref, and the UE sync
// buffer. Same name, same value, same meaning; the rationale moved with it.
// ---------------------------------------------------------------------------

// [T2 / input relay] Per-authority-id record of the capture tick behind the
// input the authority APPLIED this tick — the join key the relay is built on.
// It used to mirror LastUsedInputMapFor (same key set, same population/erase
// sites, same threading); **[T8] that twin is retired and this is now the only
// per-id authority track**, populated at registerAuthorityOwner and written in
// collectInputAll's remote branch. See the member declaration for threading.
//
// WHY A STRUCT AND NOT A BARE ALIAS. The mapped type (uint32) does not depend on
// T, so a plain `using ... = std::unordered_map<unsigned int, uint32>` would give
// EVERY member of the pack the identical tuple slot type and make the
// `std::get<LastUsedCaptureTickMapFor<T>>` lookups used below ill-formed the
// moment a second simulatable type is registered. Wrapping keyed on T gives
// distinct slots — the same reason NeutralInputFor is a struct, except that here
// the collision is guaranteed rather than merely possible.
template <typename T>
struct LastUsedCaptureTickMapFor
{
    std::unordered_map<unsigned int, uint32> value;
};

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

// [T5 / input relay] Per-REMOTE-simulatable store of the inputs the server
// relayed for that character, keyed by the SENDER's capture tick.
//
// THE EXACT COMPLEMENT of ClientInputDelayLineMapFor above: that map is populated
// for the ids that HAVE an input provider (locally controlled), this one for the
// ids that do NOT (remote proxies). Provider-presence is the local-vs-remote test
// throughout this file, and it is the one that stays correct under COUCH CO-OP,
// where a single client legitimately owns several locally-controlled characters —
// any test based on "the client's character" would pick exactly one of them and
// hand the others a relay store they must not have.
//
// NOT wiped by wipeAllForResync — see the deliberate non-wipe note there, and the
// naming ruling in Network/RelayedInputStore.h for why this is its own type rather
// than a second ClientInputDelayLine.
template <typename T>
using RelayedInputStoreMapFor = std::unordered_map<
    unsigned int,
    RelayedInputStore<typename T::InputType>>;

// ---------------------------------------------------------------------------
// [T6] resolveScheduledRelayedInput — THE UNIFIED SCHEDULED READ, in one place.
//
// The remote-proxy answer to "which relayed input does tick N run on, when no
// authoritative ref is available for N". Spelled out as a three-step ladder in
// Backlog T7 and RelayDelaySpectrumDesign.md §4/§5.2:
//
//   0. nothing has ever arrived (`!findLatest().valid`) -> TERMINAL FALLBACK, and
//      specifically NO PROBE AT ALL. `find(N)` can genuinely hit for a LAN peer,
//      so probing with a default dA of 0 would be accidentally-safe-because-a-
//      later-check-catches-it rather than correct (RelayedInputStore.h's
//      initial-state contract states and requires this skip).
//   1. probe `find(N - dLatest)`;
//   2. VERIFY the candidate's own stamp equals `dLatest`. This means "the delay
//      REGIME has not shifted under me", NOT "this entry is individually
//      scheduled at N": the authority applies ONE current delay to every parked
//      entry (its drain reads `effectiveDelay` at drain time), so the freshest
//      stamp is the best estimate of that delay, and a per-entry-stamp scan would
//      faithfully reproduce a schedule the server does not use. On mismatch the
//      schedule is in transition — fall back, never guess.
//   3. hit -> the candidate's input; miss or verify-fail -> `fallback()`, the
//      newest arrived input or the INJECTED game zero if nothing ever arrived.
//
// EMERGENT REGIME, no flag: at floor 0 the probe nearly always misses and this
// degenerates to last-known (the pre-relay behaviour); at a high floor it nearly
// always hits and the proxy consumes the server's actual schedule. Same code.
//
// WHY IT LIVES HERE, as a free function, rather than inline in its callers: T6's
// resim frontier row and T7's prediction proxy branch must run the IDENTICAL
// ladder — a resim that resolved a frontier tick differently from the prediction
// that produced it would manufacture divergence out of nothing. T7 calls this
// from collectInputAll's proxy branch; it is deliberately not a private member so
// the ladder can also be unit-tested on its own.
// ---------------------------------------------------------------------------
// Returns BY VALUE: `find` copies into an out-param, so the hit arm has a local
// to hand back and the two arms must agree on a return type.
//
// ---------------------------------------------------------------------------
// [T19] `outReport` — THE OUTCOME, REPORTED TO THE CALLER RATHER THAN COUNTED HERE.
//
// The relay hit-rate probe needs to know WHICH rung answered. The counters for it
// deliberately do NOT live in this function, and that is a design constraint rather
// than a style choice:
//
//   * this is a free function over a CONST store, deliberately side-effect-free, so
//     that it can be reasoned about (and unit-tested) as a pure classification;
//   * it is SHARED by T6's resim frontier row and T7's prediction branch precisely
//     so the two can never disagree — a resim that resolved a frontier tick
//     differently from the prediction that produced it would manufacture divergence
//     out of nothing. State living here would be state shared across two threads'
//     worth of call sites through a function whose whole value is that it has none.
//
// Reporting to the caller also means THE TWO CALL SITES ARE COUNTED SEPARATELY,
// which is worth having on its own: prediction and resim hitting at different rates
// is a real signal about the frontier, and summing them would erase it.
//
// The parameter is a defaulted OUT-POINTER rather than a widened return type so
// that every existing call site — and every existing test — compiles and behaves
// unchanged. Writing through a pointer the CALLER owns is not a side effect on the
// store, and mirrors `RelayedInputStore::find`, which already answers through
// out-params for the same reason.
//
// CLASSIFICATION NOTE — the `tick < dA` guard reports Miss, not NoProbe. Rung 0 is
// specifically `!findLatest().valid`, "nothing has EVER arrived": the join window.
// The underflow guard is a different situation — data HAS arrived, we simply cannot
// form a probe tick for a session younger than the delay. Since the D4 stale-run
// rule (review F5) is stated exactly as "count fallback serves where
// `findLatest().valid` is true", this case belongs in the run and NoProbe does not.
//
// ---------------------------------------------------------------------------
// [T20] STILL SIDE-EFFECT-FREE, STILL `const`, AND THAT IS THE CONSTRAINT THIS
// TASK WAS GIVEN EXPLICITLY. T20 adds a WHY to the miss — the store's resident span
// classifies it as an in-span coverage hole, an ask above the newest arrival, or an
// ask below the oldest — plus the signed distance from the probe to the newest
// resident. Every one of those is written through the SAME caller-owned out-pointer
// and derived from `const` reads of the store. Nothing was added to the ladder's
// control flow: the arms, their conditions, their order and their returned values
// are byte-for-byte what T6/T7 shipped and T19 left alone.
// ---------------------------------------------------------------------------
template <typename InputT>
InputT resolveScheduledRelayedInput(const RelayedInputStore<InputT>& store, uint32 tick,
                                    ScheduledRelayedReadReport* outReport = nullptr)
{
    const auto report = [outReport](ScheduledRelayedReadOutcome outcome,
                                    uint32 probeTick, std::uint8_t dLatest,
                                    std::uint8_t candidateDA) {
        if (outReport != nullptr)
        {
            outReport->outcome     = outcome;
            outReport->probeTick   = probeTick;
            outReport->dLatest     = dLatest;
            outReport->candidateDA = candidateDA;
        }
    };

    // [T20] PROBE B, half one — the SIGNED DISTANCE from the probe tick to the
    // newest thing the store holds. Attached on every rung that formed a probe tick,
    // Hit included: the hit deltas are the calibration the miss deltas are read
    // against. FREE — `newestResident` here is `findLatest().captureTick`, which the
    // ladder computed on its first line, so this adds an integer subtraction and no
    // scan. Written through the caller's pointer only; nothing here touches `store`.
    const auto reportDelta = [outReport](uint32 probeTick, uint32 newestResident) {
        if (outReport != nullptr)
        {
            outReport->newestResident     = newestResident;
            outReport->deltaToNewestValid = true;
            outReport->deltaToNewest      = static_cast<std::int32_t>(
                static_cast<std::int64_t>(probeTick) - static_cast<std::int64_t>(newestResident));
        }
    };

    // [T20] PROBE B, half two — WHY the miss happened, from the store's RESIDENT
    // SPAN. This is the one addition that costs anything: a second scan of the
    // store's slots. It is paid ONLY on a miss and ONLY when a report was asked for,
    // so a caller that passes no report (the pure-classification unit tests, and any
    // future non-probing consumer) is byte-for-byte as cheap as before T20.
    //
    // THE SPAN IS THE STORE'S, NOT THE RING'S. The ring carries `depth` entries per
    // replication; the store accumulates up to 64 arrivals. That is exactly why an
    // in-span hole is meaningful at depth 1 — it IS the hole the replace-latest ring
    // punched between two replications. Reading the ring's span here instead would
    // make every miss trivially "out of span" and the classification worthless.
    const auto reportMissClass = [outReport, &store](uint32 probeTick) {
        if (outReport == nullptr)
        {
            return;
        }

        const auto span = store.residentSpan();
        outReport->spanValid      = span.valid;
        outReport->oldestResident = span.oldest;
        outReport->newestResident = span.newest;
        outReport->residentCount  = static_cast<std::uint32_t>(span.count);

        if (!span.valid)
        {
            // Unreachable: this lambda only runs below the rung-0 gate, so at least
            // one slot is occupied. Classified rather than asserted because a probe
            // must never be the thing that brings a session down.
            outReport->missClass = ScheduledRelayedReadMissClass::NoProbeTick;
            return;
        }

        outReport->missClass =
              (probeTick > span.newest) ? ScheduledRelayedReadMissClass::AboveNewest
            : (probeTick < span.oldest) ? ScheduledRelayedReadMissClass::BelowOldest
                                        : ScheduledRelayedReadMissClass::InSpan;
    };

    const auto latest = store.findLatest();
    if (!latest.valid)
    {
        report(ScheduledRelayedReadOutcome::NoProbe, 0u, 0u, 0u);
        return store.fallback();                    // rung 0 — no probe at all
    }

    // Guard the subtraction rather than wrapping into a ~4-billion capture tick:
    // a session fewer than dA ticks old has no scheduled entry yet, which is the
    // same "nothing to read" situation as rung 0.
    if (tick < static_cast<uint32>(latest.dA))
    {
        report(ScheduledRelayedReadOutcome::Miss, 0u, latest.dA, 0u);
        // [T20] NO PROBE TICK EXISTS, so there is no delta and no span comparison to
        // make. Kept as its own miss class rather than folded into BelowOldest,
        // which it superficially resembles: this is an early-session artefact and
        // that one is a clock/capacity fault, and the whole point of the split is to
        // tell causes apart.
        if (outReport != nullptr)
        {
            outReport->missClass = ScheduledRelayedReadMissClass::NoProbeTick;
        }
        return store.fallback();
    }

    const uint32 probeTick = tick - static_cast<uint32>(latest.dA);

    std::uint8_t candidateDA = 0u;
    InputT       candidate{};
    if (store.find(probeTick, candidateDA, candidate))
    {
        if (candidateDA == latest.dA)
        {
            report(ScheduledRelayedReadOutcome::Hit, probeTick, latest.dA, candidateDA);
            reportDelta(probeTick, latest.captureTick);
            return candidate;
        }

        // A candidate WAS resident, but stamped against a delay that is no longer
        // the current one. The delay REGIME shifted under us — a transition, not
        // starvation. Same fallback, completely different diagnosis.
        report(ScheduledRelayedReadOutcome::VerifyFail, probeTick, latest.dA, candidateDA);
        reportDelta(probeTick, latest.captureTick);
        return store.fallback();
    }

    report(ScheduledRelayedReadOutcome::Miss, probeTick, latest.dA, 0u);
    reportDelta(probeTick, latest.captureTick);
    reportMissClass(probeTick);
    return store.fallback();
}

// Per-SIMULATABLE-TYPE (not per-id) neutral input. Wrapped in a struct keyed on
// the simulatable rather than stored as a bare `InputType` so that a pack whose
// members happen to share one InputType still gets distinct tuple slots —
// `std::get<InputType>` would be ill-formed there, and silently so at the
// template level until such a pack first appeared.
//
// [og-netcode-v2-input-relay T17] `injected` records whether the composition root
// ever called setNeutralInput for this type. It exists because the AUTHORITY is
// now a first-class consumer of this value — collectInputAll's remote branch
// substitutes it on a queue underrun, i.e. on every tick of every join window.
// ([T8] T17 also seeded `m_lastUsedInputs` with it at registerAuthorityOwner;
// that map is retired, so the underrun substitute is the surviving authority
// consumer. The warning at the registration site is unchanged and is still the
// right place for it — registration is where an un-injected neutral first
// becomes reachable by the branch below.) A composition root that injects on the
// client role only
// would silently reintroduce the value-initialised `InputType{}` — whose (0,0,0)
// forward vectors are exactly the value ClientInputDelayLine.h documents as
// normalisation-breaking. Before T17 the miss was invisible; the flag turns it
// into a warning at the registration site where it bites.
template <typename T>
struct NeutralInputFor
{
    typename T::InputType value{};
    bool                  injected{ false };
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
    // MUST be called by the composition root ON BOTH ROLES: the default
    // `InputType{}` is NOT the brawler's zero input (`getZeroPlayerInput` builds
    // (0,0,1) forward vectors, a value-initialised one would carry (0,0,0) into
    // normalisation). Applies to delay lines created later AND to any already
    // created, so it is order-independent with respect to registration.
    //
    // [T5] The SAME injected zero also seeds every relay store — it is that
    // store's `fallback()` value before anything has ever arrived for a remote
    // character (T7's rung 0, the pre-registration / idle window). One injection
    // point, two consumers, so the two can never disagree about what "zero" is.
    //
    // [T17] ...and a THIRD consumer, on the AUTHORITY: the substitute integrated
    // on a remote-move queue underrun. "Client" is no longer part of this value's
    // name for that reason. ([T8] T17 named a second authority consumer here, the
    // `m_lastUsedInputs` seed at registerAuthorityOwner; that map is retired with
    // the correction-input channel, so the underrun substitute is the whole of
    // the authority's use.) The authority path is still the one that is NOT
    // order-independent (see registerAuthorityOwner), which is why the injection
    // being missed there is warned about rather than merely commented.
    template <typename SimulatableT>
    void setNeutralInput(const typename SimulatableT::InputType& neutralInput)
    {
        auto& neutral = std::get<NeutralInputFor<SimulatableT>>(m_neutralInputs);
        neutral.value    = neutralInput;
        neutral.injected = true;
        for (auto& [id, line] :
             std::get<ClientInputDelayLineMapFor<SimulatableT>>(m_clientInputDelayLines))
        {
            line.setNeutralInput(neutralInput);
        }
        for (auto& [id, store] :
             std::get<RelayedInputStoreMapFor<SimulatableT>>(m_relayedInputStores))
        {
            store.setNeutralInput(neutralInput);
        }
    }

    // [T17] Has the composition root injected the game's zero input for this
    // simulatable type? Public so the ROLE that consumes it can be pinned by a
    // test rather than by a comment: the authority's seed and its underrun
    // substitute are both silently degraded — never wrong-looking — when the
    // injection is missed, which is exactly the class of regression a comment
    // does not catch. Also the read side of registerAuthorityOwner's warning.
    template <typename SimulatableT>
    bool hasNeutralInput() const
    {
        return std::get<NeutralInputFor<SimulatableT>>(m_neutralInputs).injected;
    }

    // [T17] The injected neutral itself, for the same reason: a test that asserts
    // "the authority applied the GAME's zero" must be able to name the value the
    // composition root injected without re-deriving it.
    template <typename SimulatableT>
    const typename SimulatableT::InputType& getNeutralInput() const
    {
        return std::get<NeutralInputFor<SimulatableT>>(m_neutralInputs).value;
    }

    // -----------------------------------------------------------------------
    // [T5 / input relay] Relay-store access — NULLABLE BY DESIGN
    // -----------------------------------------------------------------------
    //
    // Returns nullptr for an id that has no store, which is every LOCAL character
    // (provider-present) and every id the authority owns. Nullable rather than
    // throwing because both eventual callers are on paths that legitimately see
    // both classes of id: T7's proxy branch iterates all simulatables, and the
    // game-thread visualization readers run on the listen-server host too, where
    // no store exists at all. A throwing accessor on those paths is a known trap
    // in this codebase — the retired `getLastCorrectionInput` routed through the
    // THROWING `getCacheFor`, so calling it on the authority (which keeps no
    // caches) threw rather than answering "nothing here".
    template <typename SimulatableT>
    RelayedInputStore<typename SimulatableT::InputType>* findRelayedInputStore(unsigned int id)
    {
        auto& map = std::get<RelayedInputStoreMapFor<SimulatableT>>(m_relayedInputStores);
        const auto it = map.find(id);
        return it == map.end() ? nullptr : &it->second;
    }

    template <typename SimulatableT>
    const RelayedInputStore<typename SimulatableT::InputType>* findRelayedInputStore(unsigned int id) const
    {
        const auto& map = std::get<RelayedInputStoreMapFor<SimulatableT>>(m_relayedInputStores);
        const auto it = map.find(id);
        return it == map.end() ? nullptr : &it->second;
    }

    // -----------------------------------------------------------------------
    // [T7] getLastRelayedInput — LAST-KNOWN, for the GAME-THREAD viz readers
    // -----------------------------------------------------------------------
    //
    // `lastKnown` as the design doc's §4 vocabulary defines it: the newest relayed
    // input for a REMOTE character, or nothing if none has ever arrived. It is the
    // replacement source for the two remote-proxy visualization consumers that
    // used to read the correction cache's input column
    // (SimulationReconciliation::getLatestInput -> the SimmableUpdateComponent viz
    // site, and through it BrawlerVisualizationInputSource's remote branch).
    // [T8] That re-point is now IRREVERSIBLE, not merely preferred: with the
    // correction-input channel retired, a remote character's cache input column
    // held only this client's own prediction. [T16] The column does not exist at
    // all any more. This accessor is THE source of a remote character's actual
    // input on the client — there is no second one to fall back to.
    //
    // WHY `findLatest().input` RATHER THAN `fallback()`, which is what the
    // per-tick ladder terminates on. The viz caller must be able to tell "nothing
    // has ever arrived" from "an input arrived and it happened to be neutral":
    // the pre-existing behaviour is that a remote proxy with no data yet has its
    // input-carrying viz SKIPPED for that frame, and `fallback()` — which invents
    // the injected game zero on an empty store — would silently start drawing an
    // aim indicator at the neutral pose instead. std::optional keeps the two
    // distinguishable, exactly as the (since-retired, T16) getLatestInput it
    // replaced did with its own nullopt. (T5's notes name both spellings and this
    // condition for choosing.)
    //
    // NULLABLE FOR TWO SEPARATE REASONS, both of which the callers really see:
    // no store exists for a LOCAL character or on the AUTHORITY at all (a listen
    // server's own pawn), and a store that exists may still be cold.
    //
    // THREADING. This is the ONE reader of the store on the GAME thread, which is
    // the store's WRITER thread (OnRep_RelayedInputRing -> populate). It therefore
    // does not participate in the accepted GT-write/PT-read tear documented in
    // RelayedInputStore.h — it is same-thread with the writer, and strictly safer
    // than the physics-thread readers. It returns a COPY, so no reference into the
    // slots outlives the call.
    template <typename SimulatableT>
    std::optional<typename SimulatableT::InputType> getLastRelayedInput(unsigned int id) const
    {
        const auto* store = this->template findRelayedInputStore<SimulatableT>(id);
        if (store == nullptr)
            return std::nullopt;

        const auto latest = store->findLatest();
        if (!latest.valid)
            return std::nullopt;

        return std::optional<typename SimulatableT::InputType>(latest.input);
    }

    // [T19] Read-only access to the two relay probes.
    //
    // CONST-ONLY AND DELIBERATELY SO: the only writers are the three call sites in
    // this class, each on its own thread, and a mutable handle handed out here would
    // be an invitation to increment a physics-thread counter from the game thread —
    // the exact hazard the two-object split exists to prevent.
    //
    // Its consumer is the og-brawler-tests wiring block, which drives the REAL
    // collectInputAll and asserts the counters the log lines are built from. That
    // proof cannot be written against the probe types alone: they are unit-tested in
    // og-simulation-tests, but only a suite with SimulatableOwnerTraits bound to
    // concrete owners can show that the shipped branch actually feeds them.
    const RelayReadProbe&    getRelayReadProbe()    const { return m_relayReadProbe; }
    const RelayArrivalProbe& getRelayArrivalProbe() const { return m_relayArrivalProbe; }

    // [T24] Same contract, same reason. Its consumer is the og-brawler-tests
    // wiring block: the probe's arithmetic is unit-tested in og-simulation-tests,
    // but only a suite with SimulatableOwnerTraits bound to concrete owners can
    // show that the shipped correction callback actually feeds it AND classifies
    // by the same provider-presence test the rest of this class uses.
    const CorrectionVerdictProbe& getCorrectionVerdictProbe() const
    { return m_correctionVerdictProbe; }

    // [og-netcode-v2-input-relay item 42 / I2] Same contract, same consumer, same
    // reason as the verdict probe's accessor directly above: the three-way
    // classification is swept as a unit in og-simulation-tests, but only a suite
    // that can register a real local and a real remote character against the real
    // callback can show that the shipped site feeds it, files each landing under
    // the right class, and puts a DISCARD in the discarded bucket rather than
    // dropping it on the verdict probe's early return.
    const CorrectionLandingProbe& getCorrectionLandingProbe() const
    { return m_correctionLandingProbe; }

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
        std::function<typename SimulatableT::InputType(
            const SimulationTimeStep&,
            const ClientInputDelayLine<typename SimulatableT::InputType>&)> inputProvider)
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
            // no capture of its own to delay, so it must not get a line —
            // collectInputAll's proxy branch reads no delay line at all (post-T7
            // it reads that character's relay store instead).
            std::get<ClientInputDelayLineMapFor<SimulatableT>>(m_clientInputDelayLines)
                .try_emplace(id,
                    std::get<NeutralInputFor<SimulatableT>>(m_neutralInputs).value);

            std::get<LocalInputSenderMapFor<SimulatableT>>(m_localInputSenders)
                .emplace(id, LocalInputSender<SimulatableT>{ &predictionOwner });
        }
        else
        {
            // [T5] REMOTE character (no provider) — the exact complement of the
            // delay line above. A locally-controlled character must NOT get a relay
            // store: its inputs come from its own provider, and the server does not
            // relay a character's input back to the client that produced it.
            auto& store =
                std::get<RelayedInputStoreMapFor<SimulatableT>>(m_relayedInputStores)
                    .try_emplace(id,
                        std::get<NeutralInputFor<SimulatableT>>(m_neutralInputs).value)
                    .first->second;

            // CALL SITE 1 — arrival. Fires on the GAME thread (see the THREADING
            // section in Network/RelayedInputStore.h); the store is read on the
            // physics thread.
            predictionOwner.setOnRelayedInputReceivedCallback(
                [this, id, &store](const typename PredictionOwnerFor<SimulatableT>::RelayedInputRingType& ring) {
                    const RelayedInputIngestReport report =
                        populateRelayedInputStore<typename SimulatableT::InputType>(store, ring);

                    // [T19] PROBE 2 — replication cadence, measured in CAPTURE
                    // TICKS. GAME-THREAD window, a different object from the
                    // physics-side read probe; see the two-thread statement at the
                    // top of Network/RelayReadProbe.h.
                    //
                    // `report.newestCaptureTick` is the newest tick THIS RING
                    // carried, not the newest the store holds — the distinction is
                    // load-bearing and the report field's own comment explains it.
                    if (report.newestCaptureTickValid)
                    {
                        RelayArrivalWindowSummary arrival;
                        std::uint32_t gapCaptureTicks = 0u;
                        // ⛔ [T34 loss-counter fix] `newCaptureTicksIngested`, NOT
                        // `entriesIngested` and NOT a hard-coded 1. It is the count
                        // of capture ticks this arrival made newly resident, and it
                        // is what turns `lostCaptureTicksX1000` from a measure of
                        // the BURST RATE into a measure of loss. `entriesIngested`
                        // would count re-delivered ticks as new coverage and hide
                        // real loss; a hard-coded 1 is the retired replace-latest
                        // premise and is exactly what reported ~120 per mille on a
                        // working flush.
                        const bool windowClosed = m_relayArrivalProbe.noteArrival(
                            id,
                            report.newestCaptureTick,
                            static_cast<std::uint32_t>(report.newCaptureTicksIngested),
                            arrival,
                            &gapCaptureTicks);

                        // Per-event Verbose, and only when the cadence actually
                        // hiccuped. A gap of exactly 1 is the healthy depth-1
                        // steady state and would be a per-tick line; the interesting
                        // events are the stalls, which are what set the depth rule.
                        if (gapCaptureTicks > 1u)
                        {
                            SIMLOG(m_logger,
                                "[Verbose][RelayProbe.Arrival] id=%u newestCapture=%u "
                                "gapCaptureTicks=%u",
                                id, report.newestCaptureTick, gapCaptureTicks);
                        }

                        if (windowClosed)
                        {
                            // PER-WINDOW SUMMARY AT WARNING — the cadence
                            // [InputStats] already uses. p99 is what the
                            // `depth >= gap_p99 + margin` rule reads; the mean is
                            // deliberately absent because it hides the tail that
                            // sets depth.
                            //
                            // ⭐ [T34] `lostCaptureTicksX1000` IS THE R = 0 LOSS
                            // INSTRUMENT, and it is on this line rather than its
                            // own because it is derived from these same samples.
                            // WARNING, not Log, is load-bearing:
                            // `Config/DefaultEngine.ini` sets `LogOGNet=Warning`, so
                            // a Log line does not exist on a dedicated server —
                            // items 35 and 36 each cost this initiative a proof line
                            // for exactly that. Steady-state expectation ~ 11 per
                            // mille (the measured 1.122 % wire loss); the raw
                            // numerator and denominator ride along so a window can
                            // be re-derived rather than trusted.
                            //
                            // ⭐ [T34 rework] `discont=` IS PART OF THE GATE, not
                            // garnish. A window reporting `discont=` > 0 was
                            // interrupted (see kRelayArrivalDiscontinuityTicks) and
                            // must be DISCARDED rather than averaged in — the same
                            // rule `[RelayProbe.Write] discont=` already carries.
                            // `discontMax=` is the largest excluded gap, exact, so
                            // discarding a window never hides how bad it was.
                            //
                            // ⭐ [T34 loss-counter fix] `delivered=` IS THE FIELD
                            // THAT MAKES THIS LINE SELF-CHECKING. `lost + delivered
                            // == expected` must hold on every window; a reader who
                            // sees it fail knows the delivered count is not being
                            // plumbed and that `lostCaptureTicksX1000` is measuring
                            // the burst rate again. Before this field existed, that
                            // failure mode was indistinguishable from a lossy wire.
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
                        && store.shouldLogVersionMismatchOnce())
                    {
                        // ONCE per component per session — an incompatible peer
                        // re-replicates its ring forever.
                        SIMLOG(m_logger,
                            "[Warning][RelayedInput] DROP wire-version mismatch id=%u onWire=%u expected=%u",
                            id,
                            static_cast<unsigned int>(report.versionOnWire),
                            static_cast<unsigned int>(relayedInputRing::kWireFormatVersion));
                    }
                });

            // CALL SITE 2 — POPULATE ONCE AT BIND. Closes the hole where the ring
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
            // session.
            populateRelayedInputStore<typename SimulatableT::InputType>(
                store, predictionOwner.getRelayedInputRing());
        }

        predictionOwner.setOnCorrectionStateReceivedCallback(
            [this, id](const typename PredictionOwnerFor<SimulatableT>::SyncedCorrectionBufferType& buffer) {
                // [T24] THE DIVERGENCE VERDICT, SURFACED AND ATTRIBUTED.
                //
                // The comparison itself is unchanged and happens where it always
                // has — StateCorrectionCache::tryInsertingCorrectState's
                // `isSimilarTo`, on every correction, for every character. All that
                // is new is that the verdict now comes back out, and that THIS site
                // adds the two facts the cache could never supply: the character
                // `id`, and its CLASS.
                CorrectionInsertVerdict verdict;
                m_reconciliation.template injectCorrectionState<SimulatableT>(
                    id, buffer, &verdict);

                // THE CLASS TEST IS PROVIDER-PRESENCE — the SAME lookup
                // registerPredictionOwner forked on above and collectInputAll forks
                // on every tick, not a second notion of "remote". Read live rather
                // than captured at bind time so it cannot drift from the map that
                // actually decides behaviour.
                //
                // [item 42] HOISTED ABOVE THE LANDED GATE, unchanged in every other
                // respect. The landing-site probe below counts DISCARDS as
                // first-class samples (they are item 41's `aboveNewest` population),
                // so it needs the class on a path the verdict probe returns early
                // from.
                const bool hasProvider =
                    std::get<InputProviderMapFor<SimulatableT>>(m_inputProviders).count(id) != 0u;
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

                SIMLOG(m_logger,
                    "[Verbose][ResimProbe.Landing] id=%u tick=%u class=%s site=%s",
                    id, verdict.tick,
                    predictedCharacterClassName(characterClass),
                    correctionLandingSiteName(landingSite));

                CorrectionLandingWindowSummary landingWindow;
                if (m_correctionLandingProbe.noteLanding(
                        characterClass, landingSite, landingWindow))
                {
                    emitCorrectionLandingClassLine(
                        PredictedCharacterClass::LocallyPredicted,
                        landingWindow.local, landingWindow.samples);
                    emitCorrectionLandingClassLine(
                        PredictedCharacterClass::RemoteProxy,
                        landingWindow.remote, landingWindow.samples);
                }
                // ---------------------------------------------------------------

                // A correction whose tick had no slot was DISCARDED — no comparison
                // happened. Counting it would put a denominator under a verdict that
                // was never reached, and the discard path already logs itself
                // (isAnomalousMiss-gated, in the cache).
                //
                // [item 42] THE LANDING PROBE ABOVE DELIBERATELY SITS ON THE OTHER
                // SIDE OF THIS RETURN. Its `discarded` bucket is the one place the
                // two probes' sample sets are required to differ, and moving this
                // gate up would silently empty it.
                if (!verdict.landed)
                    return;

                // PER-EVENT DETAIL AT VERBOSE — off under the shipped
                // LogOGDivergenceProbe=Warning. Emitted on EVERY landed correction
                // rather than on disagreements only, because "per-correction verdict
                // observable with id and class" is the acceptance criterion and a
                // disagreement-only line cannot distinguish "predicted correctly"
                // from "no correction arrived". The cost is one snprintf at a site
                // that already performs two per correction ([InjectCorrectionState]
                // and the cache's own line), so this adds no new volume CLASS — the
                // thing T19 was filed to stop.
                SIMLOG(m_logger,
                    "[Verbose][DivergenceProbe.Correction] id=%u tick=%u class=%s correct=%u",
                    id, verdict.tick,
                    predictedCharacterClassName(characterClass),
                    verdict.predictionWasCorrect ? 1u : 0u);

                CorrectionVerdictWindowSummary window;
                if (!m_correctionVerdictProbe.noteCorrection(
                        characterClass, verdict.predictionWasCorrect, window))
                    return;

                // PER-WINDOW SUMMARY AT WARNING, ONE LINE PER CLASS. Never one
                // pooled line: only the remote half can move with the relay delay
                // floor, and summing it with a locally-predicted population that
                // cannot move would dilute exactly the signal T23 scenario 4 reads.
                //
                // A class with no corrections in the window is SKIPPED rather than
                // printed as `rate=0`, which would read as a perfect record instead
                // of as no observation. That is also the steady state on a client
                // with no remote proxies.
                emitCorrectionVerdictClassLine(
                    PredictedCharacterClass::LocallyPredicted, window.local, window.samples);
                emitCorrectionVerdictClassLine(
                    PredictedCharacterClass::RemoteProxy, window.remote, window.samples);
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
    void registerAuthorityOwner(unsigned int id, AuthorityOwnerFor<SimulatableT>& authorityOwner)
    {
        std::get<RemoteMoveQueueMapFor<SimulatableT>>(m_remoteMoveQueues)
            .emplace(id, RemoteMoveQueue<typename SimulatableT::InputType>{});

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
        const auto& neutral = std::get<NeutralInputFor<SimulatableT>>(m_neutralInputs);
        if (!neutral.injected)
        {
            SIMLOG(m_logger,
                "[Warning][NeutralInput] registerAuthorityOwner id=%u ran BEFORE setNeutralInput"
                " - every underrun substitute until injection falls back to a"
                " value-initialised input", id);
        }

        // [T2] Populated here and erased in unregisterSimulatable —
        // collectInputAll's remote branch writes it with .at(id), so the key must
        // exist for the whole lifetime of the authority registration.
        // The initial value is the SENTINEL, not 0: before the first authority tick
        // this id has applied no input at all, which is exactly what the sentinel
        // means. Seeding 0 would claim capture tick 0 was applied.
        std::get<LastUsedCaptureTickMapFor<SimulatableT>>(m_lastUsedCaptureTicks)
            .value.emplace(id, kNoInputCaptureTick);

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
            // [T8] `clearOnCorrectionInputReceivedCallback` was here; the channel it
            // unbound is retired, so there is nothing to clear.
            // [T5] Same ordering requirement as the authority's remote-move
            // callback below: the relay lambda captures `&store` into
            // m_relayedInputStores, so it must be cleared before step 3 erases it.
            predictionOwner->clearOnRelayedInputReceivedCallback();
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
        // [T5] Erased on UNREGISTRATION only — never on resync (see wipeAllForResync).
        std::get<RelayedInputStoreMapFor<T>>(m_relayedInputStores).erase(id);
        // [T2] Erased here, populated in registerAuthorityOwner. ([T8] its twin
        // m_lastUsedInputs used to be erased on the line above; retired.)
        std::get<LastUsedCaptureTickMapFor<T>>(m_lastUsedCaptureTicks).value.erase(id);

        // [T19] Step 4: drop the probes' per-id telemetry state. Neither probe holds
        // anything the simulation reads, but both keep an id-keyed map (the stale
        // run and the capture-tick watermark), and without this they would grow with
        // every character that has ever existed in the session — the unbounded-memo
        // shape this codebase has already had to fix once in the log throttles.
        m_relayReadProbe.forgetOwner(id);
        m_relayArrivalProbe.forgetOwner(id);
        // [T24] m_correctionVerdictProbe is deliberately NOT forgotten here, and the
        // asymmetry is by design rather than an omission: it keeps no id-keyed map
        // at all. Its counters are per-CLASS aggregates across characters, so an
        // unregistered character leaves nothing to erase and the class totals for
        // the window it was part of stay correct. Adding a forgetOwner here would
        // need per-id state to exist first, which would be a different metric.
    }

    // -----------------------------------------------------------------------
    // [T2 / input relay] Applied-capture-tick accessor
    // -----------------------------------------------------------------------
    //
    // The capture tick behind the input the authority applied for `id` on its most
    // recent tick, or kNoInputCaptureTick if that input was a substitute (queue
    // underrun) or the authority has not ticked this id yet. T4 attaches this to
    // the correction state; the tests observe it here.
    //
    // Returns the sentinel rather than throwing for an unknown id: a caller asking
    // about an id the authority does not own is in exactly the "no real input
    // applied" situation the sentinel describes.
    template <typename SimulatableT>
    uint32 getLastUsedCaptureTick(unsigned int id) const
    {
        const auto& map =
            std::get<LastUsedCaptureTickMapFor<SimulatableT>>(m_lastUsedCaptureTicks).value;
        const auto it = map.find(id);
        return it == map.end() ? kNoInputCaptureTick : it->second;
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
                // [T15] HOISTED ABOVE THE PROVIDER CALL — this used to be looked
                // up further down, right before the delayed read.
                //
                // The line is passed INTO the provider because the provider runs
                // the game's motion-sequence matcher, which needs raw capture
                // history. Reading it here and handing it over puts two facts
                // side by side that used to live in different files:
                //
                //   * the provider sees history up to `tick - 1` ONLY — the push
                //     below is the next statement it does not get to observe;
                //   * the current tick's sample is the provider's own return
                //     value, so it never needs to look for it.
                //
                // `.at(id)` rather than a nullable lookup on purpose: the line is
                // created iff a provider is registered (see
                // registerPredictionOwner), so provider-present and line-present
                // are the same condition by construction. A throw here would mean
                // that invariant had been broken, which is exactly when a silent
                // fallback would be wrong.
                auto& delayLine =
                    std::get<ClientInputDelayLineMapFor<T>>(m_clientInputDelayLines).at(id);

                // The RAW capture, as the local player produced it THIS tick.
                const auto capture = it->second(step, delayLine);

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
                //     is what the integrator predicts with.
                //
                //     [T16] It used to ALSO be pushed into the correction cache's
                //     input column, because that column's slot T meant "the input
                //     APPLIED at tick T" — matching both the server's replicated
                //     correction-input write and the no-offset read the pre-T6
                //     collectResimInputAll did. All three of those are retired:
                //     the channel at T8, the resim read at T6 (it resolves by the
                //     T4 capture-tick REF now), and the column itself at T16. The
                //     `applied` value is consumed by the integrator and by nothing
                //     else on this line; the RAW capture is what persists, in the
                //     delay line, which is where the resim and the motion matcher
                //     both read it from.
                //
                // With `effectiveDelay == 0` this is exactly the pre-T9 path:
                // resolveDelayedInput returns the live capture untouched.
                //
                // [T15] `delayLine` is bound ABOVE, before the provider call.
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
                    // [T16] The `pushPredictionInput<T>(id, applied)` that stood
                    // here is gone with the cache's input column. The tick push
                    // above is what allocates the slot; only the input write went.
                    // ORIGINAL capture, current tick — see above. Do not pass
                    // `applied` here.
                    std::get<PendingInputQueueMapFor<T>>(m_pendingInputQueues)
                        .at(id).enqueue(step.getTick(), capture);
                }

                map.emplace(id, std::move(applied));
            }
            else if (auto qit = queueMap.find(id); qit != queueMap.end())
            {
                // [T2] UNDERRUN MUST BE DETECTED HERE, BEFORE THE DEQUEUE.
                //
                // dequeueMove() on an empty queue returns a value-initialised
                // Move{} whose `tick` is 0 (SimulationQueues.h) — and 0 is a
                // perfectly ordinary real capture tick at session start. So the
                // RETURNED tick cannot distinguish "the authority substituted an
                // input" from "the authority applied the client's tick-0 capture".
                // Only the pre-dequeue empty() gate can, which is why the flag is
                // taken first and the returned tick is never used to infer it.
                const bool underrun = qit->second.empty();
                auto move = qit->second.dequeueMove();
                SIMLOG(m_logger, "[CollectInput] id=%u tick=%u source=RemoteQueue queuedTick=%u",
                    id, step.getTick(), move.tick);
                // [T2] The relay's join key: the ORIGINAL capture tick of the input
                // just applied. move.tick is that original capture tick: the drain
                // (ServerReceptionCoordinator::releaseDelayedInputs) delivers the
                // entry's STORED captureTick rather than a reconstructed
                // `simTick - delay`, so it survives an overdue release intact.
                // The sentinel replaces it when the input was a substitute and no
                // client capture stands behind it.
                //
                // [T17] UNCHANGED by the substitution below — the ref stays the
                // sentinel on an underrun, which remains exactly true: no client
                // capture stands behind the game's zero input either. T17 fixes
                // WHICH input is substituted, not how it is classified.
                std::get<LastUsedCaptureTickMapFor<T>>(m_lastUsedCaptureTicks).value.at(id) =
                    underrun ? kNoInputCaptureTick : move.tick;

                if (underrun)
                {
                    // [T17] THE SUBSTITUTE IS THE GAME'S ZERO INPUT.
                    //
                    // dequeueMove() on an empty queue hands back a value-initialised
                    // Move (SimulationQueues.h), so `move.input` here is an
                    // `InputType{}` — for the brawler that is (0,0,0) forward
                    // vectors, the value ClientInputDelayLine.h names as the one
                    // that "would be carried into normalisation and break". The
                    // injected neutral is the game's real zero ((0,0,1) forwards),
                    // it is already present on both roles, and it was simply unused
                    // on this path.
                    //
                    // This is NOT a loss-only path: the remote branch runs from
                    // registerAuthorityOwner onward, so every tick between
                    // registration and the client's first input arriving underruns
                    // — i.e. every join window, in ordinary play.
                    //
                    // [T8] T17 also wrote this value into `m_lastUsedInputs` here,
                    // because that map was replicated as "the input applied" on the
                    // correction-input channel. Both the map and the channel are
                    // retired; what the substitution feeds now is the integration
                    // (and, through it, the correction STATE peers actually consume).
                    // The T17 fix is undiminished — this is still the only value the
                    // authority simulates on an underrun tick — it simply has one
                    // consumer instead of two.
                    const auto& neutral = std::get<NeutralInputFor<T>>(m_neutralInputs).value;
                    map.emplace(id, neutral);
                }
                else
                {
                    // The dequeued input passes through UNTOUCHED — this arm is
                    // byte-for-byte the pre-T17 behaviour.
                    map.emplace(id, std::move(move.input));
                }
            }
            else
            {
                // -----------------------------------------------------------
                // [T7] SIMULATED-PROXY BRANCH — THE UNIFIED SCHEDULED READ.
                // -----------------------------------------------------------
                //
                // The client is predicting a character it does not control. Until
                // T7 this read the CORRECTION CACHE's last server-reported input
                // — "hold whatever the server last told us this player did",
                // about one RTT stale and with no notion of WHEN the authority
                // would apply it. It now asks the relay store the same question
                // the resim asks: which relayed input does tick N run on.
                //
                // ONE CODE PATH, NO REGIME FLAG — the emergent-regime property
                // (RelayDelaySpectrumDesign.md §4). resolveScheduledRelayedInput
                // probes `N - dLatest` and verifies the stamp:
                //
                //   * at relay delay floor 0 the probe nearly always MISSES (the
                //     sender's capture for tick N has not reached us yet, because
                //     nothing was scheduled to make it) and the ladder degenerates
                //     to last-known — byte-for-byte the pre-relay behaviour;
                //   * at a high floor it nearly always HITS and the proxy consumes
                //     the server's actual schedule, tick for tick.
                //
                // Nothing here selects between those. The regime is decided per
                // input, per receiver, by whether the data is there — which is why
                // raising the floor needs no second implementation and no switch.
                //
                // And "nearly always" is deliberate on BOTH ends: a hit at floor 0
                // is legitimate, not a bug. A mixed pair — a WAN sender stamped
                // dA=4 against a LAN receiver whose lead wobbles down inside the
                // dead band — can satisfy the schedule, and the answer is then the
                // server's REAL scheduled input rather than a stale hold. That is
                // a non-regression, and it is why the degenerate-equivalence test
                // pins byte-equivalence to empty/behind-store conditions only
                // rather than asserting it absolutely at floor 0 (review A4).
                //
                // THE SAME FUNCTION T6'S RESIM CALLS, on purpose: a resim that
                // resolved a frontier tick differently from the prediction that
                // produced it would manufacture divergence out of nothing. Do not
                // inline a second copy of the ladder here.
                //
                // THE TERMINAL VALUE IS THE INJECTED GAME ZERO. This branch used
                // to read `cached.value_or(typename T::InputType{})` — the same
                // (0,0,0)-forward poison T17 removed from the authority path, and
                // reached on EVERY tick of the D4 pre-registration / idle window
                // rather than only on loss. Rung 0 of the ladder answers
                // `store.fallback()`, which is the injected neutral until anything
                // arrives; a missing store answers the same neutral directly.
                // Sentinel-aware for free: `kNoInputCaptureTick` is never a store
                // key (push rejects it), so a sentinel can only ever resolve
                // through the same neutral, never through an InputType{}.
                //
                // Nullable store lookup for the reason T5 documents on the
                // accessor: this branch iterates every simulatable, and a
                // character can be seen here before its registration completes.
                //
                // The prediction tick is still advanced in lockstep with the
                // provider branch below — otherwise postPredictionAll keeps
                // overwriting a stale tick slot and every correction lands outside
                // the cache window. That part is unchanged by T7, and by T16:
                // the tick push is the slot allocation and it stays; only the
                // paired input write went with the column.
                //
                // [T19] The read is CLASSIFIED here (probe 1 + probe 3). The ladder
                // itself stays stateless — it reports which rung answered through
                // `readReport` and this call site does the counting, which is also
                // what keeps prediction and resim counted separately. A missing
                // store is not a read at all and is deliberately not counted: there
                // was no probe to hit or miss, and folding it in would make a
                // not-yet-registered proxy look like starvation.
                const auto* store = this->template findRelayedInputStore<T>(id);
                ScheduledRelayedReadReport readReport;
                typename T::InputType input =
                    store != nullptr
                        ? resolveScheduledRelayedInput(*store, step.getTick(), &readReport)
                        : std::get<NeutralInputFor<T>>(m_neutralInputs).value;

                if (store != nullptr)
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
                            id, step.getTick(), readReport.probeTick,
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
                            id, step.getTick(), readReport.probeTick,
                            readReport.oldestResident, readReport.newestResident,
                            readReport.residentCount);
                    }
                }

                SIMLOG(m_logger, "[CollectInput] id=%u tick=%u source=RelayStore hasStore=%d",
                    id, step.getTick(), store != nullptr ? 1 : 0);

                if (step.getStepKind() == StepKind::Skip)
                    m_reconciliation.template backfillSkippedTick<T>(
                        id, step.getTick() - 1, simulatable.getAllState().getState());

                if (step.getStepKind() != StepKind::Stall)
                {
                    m_reconciliation.template pushPredictionTick<T>(id, step.getTick());
                    // [T16] The remote `pushPredictionInput<T>(id, input)` that
                    // stood here is gone. It was the write that made a REMOTE
                    // column look alive while holding only this client's guess —
                    // spectrum doc §14.2 amendment 2. Nothing stores that guess
                    // now; the relay store holds what the proxy actually sent.
                }

                map.emplace(id, std::move(input));
            }
        });

        // [T19] PROBES 1 + 3, per-window summary. Driven from here — once per
        // prediction tick, after every character has been resolved — for the same
        // reason ServerReceptionCoordinator drives [InputStats] from its
        // once-per-tick reapConnections hook: it is the only place with a
        // monotonic tick AND a guarantee of running exactly once per tick.
        //
        // PHYSICS-THREAD WINDOW. The game-thread arrival probe has its own, separate
        // window object; see the two-thread statement at the top of
        // Network/RelayReadProbe.h for why they must not be merged.
        emitRelayReadWindowIfDue(step.getTick());

        return inputs;
    }

    // -----------------------------------------------------------------------
    // [T6] Resim replay input (physics thread) — THE RESOLUTION TABLE
    // -----------------------------------------------------------------------
    //
    // RELOCATED HERE from SimulationReconciliation (T6 placement ruling, Option
    // C). It used to read one thing — the correction cache's input column — and
    // its old home was justified on exactly that. It now reads the client's own
    // delay lines, the relayed-input stores and the injected neutrals, all owned
    // by this class; only the JOIN KEY still comes from reconciliation, through
    // the single narrow getAppliedCaptureTickRef query. Nothing was added to
    // either class's dependencies: this class already held m_reconciliation, and
    // reconciliation still knows nothing about netsync.
    //
    // TWO-LEVEL DISPATCH: TICK CLASS x CHARACTER CLASS.
    //
    //                    | LOCAL (provider present)   | REMOTE (proxy)
    //   -----------------+----------------------------+---------------------------
    //   Ref (corrected)  | delayLine.at(ref)          | store.find(ref) -> input
    //   Sentinel         | injected game zero         | injected game zero
    //   ...store miss    | line miss -> its neutral   | store.fallback() (SELF-HEAL)
    //   NoRef (frontier) | delayLine.at(t - d)        | the scheduled read
    //   NoSlot           | no entry at all — the character is not in this resim
    //
    // CHARACTER CLASS IS PROVIDER PRESENCE, NEVER A ROLE CHECK (T5's mechanism).
    // It is the only test that stays correct under COUCH CO-OP, where one client
    // legitimately owns several locally-controlled characters and any
    // "the client's character" test would pick exactly one of them.
    //
    // ---------------------------------------------------------------------
    // THE PRECEDENCE RULE (RelayDelaySpectrumDesign.md §5.3): WHEREVER A REF
    // EXISTS, THE REF WINS — and the relay entry's `dA` stamp is IGNORED on a
    // corrected tick. The stamp is the INTENDED schedule; the ref is what the
    // authority ACTUALLY did, and resim exists to reproduce the authority. The
    // case that makes this concrete: T26 can release a capture LATE (up to
    // rollbackWindowHardCap), so capture 6 stamped for tick 7 may be applied at
    // tick 7+d — the ref on 7+d then says 6, and resim must replay 6 there.
    // An implementation that consulted the stamp would replay whatever capture
    // the schedule pointed at instead, and be confidently wrong.
    //
    // ---------------------------------------------------------------------
    // NoSlot EMITS NOTHING, AND THAT IS LOAD-BEARING. The pre-T6 body only
    // emplaced on a cache hit, and SimulationIntegrationExecutor::integrateAll
    // skips any id absent from the map. prepareResimAll likewise only restores
    // state for a character whose slot exists — so emitting an input for a
    // slotless character would integrate it from an UN-RESTORED state. Freshly
    // registered proxies sit in exactly that window for a few ticks.
    //
    // ---------------------------------------------------------------------
    // THE D2 FRONTIER EDGE — DOCUMENTED, DELIBERATELY NOT SOLVED (Backlog T6).
    // The NoRef/local row re-derives with the CURRENT effective delay. If the
    // delay changed between the original prediction of tick `t` and this resim,
    // `t - currentEffectiveDelay` can name a DIFFERENT capture than the one
    // originally integrated there, so a resim that crosses a delay change replays
    // a mixture of offsets. That is the accepted offset-mixture edge: exposure is
    // about one tick at every-frame correction cadence, the every-frame state
    // anchor supersedes it immediately, and T14's stall paydown narrows the window
    // further. Do not "fix" this by stashing the per-tick delay without re-opening
    // the design decision — the cache slot would have to carry a third column.
    //
    // ---------------------------------------------------------------------
    // RESYNC INTERPLAY, stated so it is not rediscovered as a regression. A hard
    // resync wipes the correction cache AND the local delay lines, but NOT the
    // relay stores (T5's ruling — a relayed entry is keyed by the SENDER's capture
    // tick, which our clock jumping does not invalidate). So through the wipe
    // window local ticks resolve to the neutral, exactly as they did pre-T6, while
    // remote proxies keep resolving from surviving entries. Strictly better than
    // the pre-T6 behaviour, in which the wiped cache blinded both.
    ResolvedInputs<SimulatableTs...> collectResimInputAll(uint32 simTick)
    {
        ResolvedInputs<SimulatableTs...> inputs;

        // ONE load per resim tick, mirroring collectInputAll's one-load-per-tick
        // rule so a concurrent OnRep cannot split a single tick across two
        // different delays. See the D2 note above for what it does NOT promise.
        const int32 effectiveDelay = getClientEffectiveInputDelayTicks();

        // LOG VOLUME, deliberately bounded. The branches below are mutually
        // exclusive, so this emits EXACTLY ONE line per character per resim tick
        // — and it is [Verbose]-prefixed while its `[Resim.*]` neighbours are not,
        // so an operator can silence this table trace alone (LogOGSim=Log) and
        // keep [Resim.Pre]/[Resim.Post]. The pre-T6 body logged nothing at all;
        // a resolution table nobody can trace in PIE is not worth that saving.

        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            auto& map = std::get<std::unordered_map<unsigned int, typename T::InputType>>(inputs);

            const AppliedCaptureRef ref =
                m_reconciliation.template getAppliedCaptureTickRef<T>(id, simTick);

            if (ref.kind == AppliedCaptureRefKind::NoSlot)
            {
                SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=NoSlot", id, simTick);
                return;
            }

            const auto& neutral = std::get<NeutralInputFor<T>>(m_neutralInputs).value;

            if (ref.kind == AppliedCaptureRefKind::Sentinel)
            {
                // BOTH character classes, one answer: the authority told us it
                // applied no client capture at this tick, and T17 made the value
                // it substituted the INJECTED GAME ZERO. Reproducing an authority
                // decision means applying what the authority applied — never a
                // value-initialised InputType{}, which is the exact poison T17
                // removed from that path.
                SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=Sentinel", id, simTick);
                map.emplace(id, neutral);
                return;
            }

            auto& providerMap  = std::get<InputProviderMapFor<T>>(m_inputProviders);
            const bool isLocal = providerMap.find(id) != providerMap.end();

            if (isLocal)
            {
                // `.at(id)` for the same reason collectInputAll uses it: the line
                // is created iff a provider is registered, so provider-present and
                // line-present are the same condition by construction.
                const auto& delayLine =
                    std::get<ClientInputDelayLineMapFor<T>>(m_clientInputDelayLines).at(id);

                const int32 captureTick = (ref.kind == AppliedCaptureRefKind::Ref)
                    ? static_cast<int32>(ref.captureTick)
                    : static_cast<int32>(simTick) - effectiveDelay;

                // A line miss answers with the line's own INJECTED neutral (never
                // InputT{}) — the store-miss/local cell. Post-resync that is the
                // only reachable way to get here, and it is today's behaviour.
                //
                // NOT resolveDelayedInput: that helper exists to protect the LIVE
                // capture at delay 0, and a resim has no live capture. Reading
                // at(simTick) at delay 0 is right precisely because the original
                // prediction pass pushed that tick's capture into the line.
                SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=%s src=DelayLine capture=%d",
                    id, simTick, ref.kind == AppliedCaptureRefKind::Ref ? "Ref" : "NoRef", captureTick);
                map.emplace(id, delayLine.at(captureTick));
                return;
            }

            // REMOTE. Nullable by design (T5): the authority allocates no stores
            // for ids it owns, and a character can be iterated before its
            // registration completes. Neutral rather than a throw, matching every
            // other reader of this accessor.
            const auto* store = findRelayedInputStore<T>(id);
            if (store == nullptr)
            {
                SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=Remote src=NoStore", id, simTick);
                map.emplace(id, neutral);
                return;
            }

            if (ref.kind == AppliedCaptureRefKind::Ref)
            {
                // THE REF WINS — see the precedence block above. `outDA` is bound
                // only because `find` requires an out-param; its value is
                // deliberately never read. The stamp is the intended schedule and
                // has no authority over a tick the server has already ruled on.
                std::uint8_t   ignoredScheduleStamp = 0u;
                typename T::InputType relayed{};
                const bool hit = store->find(ref.captureTick, ignoredScheduleStamp, relayed);
                SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=Ref src=RelayStore ref=%u hit=%d",
                    id, simTick, ref.captureTick, hit ? 1 : 0);
                // THE SELF-HEAL: a miss degrades THIS TICK's replay input to
                // last-known, never the injected state. The state is a complete
                // anchor, so the next every-frame correction supersedes the error.
                map.emplace(id, hit ? relayed : store->fallback());
                return;
            }

            // NoRef / REMOTE — no authoritative answer for this tick, so run the
            // same scheduled read the prediction pass runs (T7). Sharing the ladder
            // is what stops a resim from disagreeing with the prediction it replays.
            //
            // [T19] SECOND CALL SITE OF PROBE 1 — counted into its OWN counter block,
            // never summed with the prediction one. Resim resolves ticks the
            // prediction already ran, and entries missing then may have landed since,
            // so a resim hit rate materially above the prediction's is real
            // information about the frontier rather than noise.
            //
            // It does NOT feed the stale run (probe 3): a run is "consecutive ticks
            // this character was served a fallback", and resim revisits ticks out of
            // order and repeatedly. It also does not advance the window — the window
            // is keyed to the monotonic prediction tick, and a resim's `simTick`
            // walks backwards.
            ScheduledRelayedReadReport readReport;
            typename T::InputType scheduled =
                resolveScheduledRelayedInput(*store, simTick, &readReport);
            // [T20] The whole report — same reason as the prediction site.
            m_relayReadProbe.noteResimRead(readReport);

            if (readReport.outcome == ScheduledRelayedReadOutcome::VerifyFail)
            {
                SIMLOG(m_logger,
                    "[Verbose][RelayProbe.Read] id=%u tick=%u VERIFY-FAIL probeTick=%u "
                    "candidateDA=%u dLatest=%u src=Resim",
                    id, simTick, readReport.probeTick,
                    static_cast<unsigned int>(readReport.candidateDA),
                    static_cast<unsigned int>(readReport.dLatest));
            }

            SIMLOG(m_logger, "[Verbose][Resim.Input] id=%u tick=%u class=NoRef src=ScheduledRead", id, simTick);
            map.emplace(id, std::move(scheduled));
        });

        return inputs;
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
    //     ClientInputDelayLine by the T4 ref (T6), the local viz reads the live
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
                    this->template getLastUsedCaptureTick<T>(id);
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
        //
        // [T15] SECOND CONSUMER, SECOND CONSEQUENCE — stated so it is not
        // rediscovered as a bug. The line is now also the motion matcher's
        // history source, so this wipe blanks the matcher for up to
        // `inputSequence::kHistoryWindowFrames` (30) ticks after a hard resync,
        // where the correction cache it used to read would still have served
        // data. That is CORRECT, not a regression: the cache's data was keyed to
        // the pre-resync clock, so a motion "matched" out of it would have been
        // assembled from ticks that no longer mean what they meant. Going cold is
        // the honest answer, and it costs at most half a second of matcher
        // availability on an event that already visibly jumps the simulation.
        forEachTypeMap(m_clientInputDelayLines, [&]<typename T>(auto& perTypeMap) {
            for (auto& [id, line] : perTypeMap)
            {
                SIMLOG(m_logger,
                    "[TimeResync.WipeInputDelayLine] id=%u newPredictionTick=%u",
                    id, newPredictionTick);
                line.clear();
            }
        });

        // [T5 / input relay] m_relayedInputStores is DELIBERATELY NOT WIPED HERE,
        // and this comment exists because the loop above is the obvious thing to
        // mirror.
        //
        // The delay line's keys are the LOCAL prediction clock's tick numbers, and
        // a hard resync jumps that clock — so those keys stop describing the ticks
        // they were written for, and keeping them would read a capture at the wrong
        // tick for `effectiveDelay` ticks. A relay entry's key is the SENDER's
        // capture tick: a server-domain identity produced by another machine's
        // clock, which a resync of OUR clock does not touch. Wiping it would blind
        // every remote proxy for a window after every resync, for no reason.
        //
        // (This wipe divergence is also the decisive reason the store is its own
        // type rather than a reused ClientInputDelayLine — see the naming ruling in
        // Network/RelayedInputStore.h. A reused type would have been swept by
        // whoever next mirrored these per-id map loops.)
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

    // [T24] ONE per-window class block of the correction-verdict summary. Called
    // twice — once per class — from the correction callback bound in
    // registerPredictionOwner, and ONLY when a window closed.
    //
    // SILENT ON AN EMPTY CLASS. `corrections == 0` means this window observed
    // nothing about that class; printing `disagreed=0 ratePerMille=0` would assert
    // a perfect record where there is no record at all, and that misreading is
    // precisely the one a benefit claim would be built on. It is also the steady
    // state for the remote block on a client that has no proxies and for the local
    // block on a spectator, so gating it keeps those sessions quiet as well.
    //
    // ONE LINE PER CLASS, NEVER ONE POOLED LINE — the reason is at the top of
    // Network/CorrectionVerdictProbe.h and is the whole point of the split.
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

    // [og-netcode-v2-input-relay item 42 / I2] ONE per-window class block of the
    // frontier-landing split. Called twice — once per class — from the correction
    // callback, and ONLY when a window closed.
    //
    // SILENT ON AN EMPTY CLASS, same rule and same reason as the verdict line
    // above: printing `behind=0 atFrontier=0 discarded=0` would assert a perfect
    // record where there is no record at all, and that is the misreading this whole
    // instrument exists to prevent. It is also the steady state for the remote
    // block on a client with no proxies.
    //
    // ⭐ HOW TO READ THE PAIR THIS LINE FORMS WITH `[ResimProbe.Gate]`. Under the
    // mechanism, resim triggers track `atFrontier` and are blind to `behind`. So:
    //   * `atFrontierPerMille` here ~= `requestedPerMille` on the Gate line  ⇒ the
    //     finding's central claim reproducing live;
    //   * `behind` large with the Gate line's `requested` small  ⇒ the suppressed-
    //     correction population, i.e. the under-resimulation statement itself;
    //   * `discarded` large  ⇒ item 41's `aboveNewest` anomaly, whose fix will MOVE
    //     this mix (it is referenced here, not solved here).
    // The two lines cannot be merged into one: they are fed by different threads
    // and item 42 requires one probe per thread with no sharing. See the cost note
    // at the top of ResimGateProbe.h.
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

    // [T19] PROBES 1 + 3 — the per-window summary. Called once per prediction tick
    // from collectInputAll; silent unless a window both CLOSED and carried at least
    // one scheduled read, so the authority (no relay stores, so neither call site is
    // ever reached) and an idle client never heartbeat a Warning line.
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

    // [T20] PROBE B — the miss partition for ONE call site. Silent when that call
    // site missed nothing, so a healthy window costs no line.
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

    // [T20] PROBE B — the signed `probeTick - newestResident` distribution for ONE
    // call site. At depth 1 this is the richer signal: it says WHERE the receiver is
    // asking relative to what it holds, continuously, rather than in three buckets.
    // A window whose p50 sits above 0 is a receiver reading ahead of its data (no
    // depth helps); one whose p50 sits below 0 while missing is reading inside a
    // span full of holes (depth does).
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

    // m_inputProviders uses std::function intentionally — converting to a pointer struct
    // would require extending PredictionSyncedBufferOwnerConcept with a typed
    // getLocalInputFor<T>(step) method; tracked as a post-cutover follow-up.
    std::tuple<InputProviderMapFor<SimulatableTs>...>    m_inputProviders;
    std::tuple<RemoteMoveQueueMapFor<SimulatableTs>...>  m_remoteMoveQueues;
    std::tuple<PendingInputQueueMapFor<SimulatableTs>...> m_pendingInputQueues;

    // [T2 / input relay] The capture tick behind each applied remote input, or
    // kNoInputCaptureTick on an underrun substitution. Written in collectInputAll
    // (PHYSICS thread), read in sendCorrectionAll (GAME thread) where T4 attaches
    // it to the correction state.
    //
    // [T8] It used to be the strict parallel of `m_lastUsedInputs` and INHERITED
    // that member's pre-existing physics-write / game-read pattern rather than
    // introducing one. `m_lastUsedInputs` is now retired, so this member carries
    // that pattern alone — it is the same pattern, not a new one, and the argument
    // for why it is tolerable is unchanged: it is a plain scalar per id, so the
    // worst a torn schedule can do is carry the previous tick's value for one
    // tick, which the every-frame correction that transports it then supersedes.
    std::tuple<LastUsedCaptureTickMapFor<SimulatableTs>...> m_lastUsedCaptureTicks;

    std::tuple<AuthorityWriterMapFor<SimulatableTs>...>  m_authorityWriters;
    std::tuple<LocalInputSenderMapFor<SimulatableTs>...> m_localInputSenders;

    // [T9 parts 3+4] Client Layer-1 input delay. Populated for provider-owning
    // ids only; touched exclusively from collectInputAll (physics thread) and
    // wipeAllForResync.
    std::tuple<ClientInputDelayLineMapFor<SimulatableTs>...> m_clientInputDelayLines;

    // [T9 part 4, RENAMED by T17] The game's zero input, per simulatable type.
    // NOT "client" neutral inputs any more: the AUTHORITY reads this too (the
    // underrun substitute in collectInputAll's remote branch; [T8] T17 also named
    // the `m_lastUsedInputs` seed in registerAuthorityOwner, since retired), so the
    // old name described one of several consumers and invited exactly the reasoning
    // that left the authority path integrating `InputType{}`. Written once by the
    // composition root
    // (setNeutralInput, game thread, before/around registration), read on the
    // physics thread thereafter.
    std::tuple<NeutralInputFor<SimulatableTs>...>            m_neutralInputs;

    // [T5 / input relay] The client's relayed-input stores, one per REMOTE
    // character (provider-ABSENT ids — the complement of m_clientInputDelayLines).
    //
    // WRITTEN ON THE GAME THREAD (the OnRep_RelayedInputRing callback bound in
    // registerPredictionOwner), READ ON THE PHYSICS THREAD (T7's proxy branch of
    // collectInputAll, T6's collectResimInputAll). That crossing is INHERITED from
    // the correction path, which already does exactly this, and its full rationale
    // — the torn-slot-not-container-UB argument, the correction heal, and the
    // named deferred SPSC-seam cleanup — is in the THREADING section of
    // Network/RelayedInputStore.h. Do not restate it here; do not weaken it there.
    std::tuple<RelayedInputStoreMapFor<SimulatableTs>...> m_relayedInputStores;

    // [T19] THE TWO CLIENT-SIDE RELAY PROBES. Pure telemetry: nothing in the
    // resolution path reads them, and every consumer is a SIMLOG.
    //
    // TWO OBJECTS BECAUSE THERE ARE TWO THREADS, and they must never be merged into
    // one window — the full statement is at the top of Network/RelayReadProbe.h.
    //
    //   m_relayReadProbe     PHYSICS thread only: collectInputAll's proxy branch
    //                        and collectResimInputAll's NoRef/remote row.
    //   m_relayArrivalProbe  GAME thread only: the relay-ring arrival callback bound
    //                        in registerPredictionOwner.
    //
    // NOT ATOMIC, AND CORRECTLY SO — unlike m_clientEffectiveInputDelayTicks below,
    // neither object is ever touched from the other's thread, so there is no
    // crossing to guard. This is the ONE piece of relay state in this class that
    // does NOT inherit the game-write/physics-read pattern, precisely because it was
    // split to avoid it.
    //
    // THE SERVER'S maybeEmitInputStats WAS THE OBVIOUS TEMPLATE AND DOES NOT FIT:
    // ServerReceptionCoordinator is server-side and both of these probes are
    // client-side, so they need their own window rather than a counter bolted onto
    // the server's.
    RelayReadProbe    m_relayReadProbe;
    RelayArrivalProbe m_relayArrivalProbe;

    // [T24] THE CORRECTION-VERDICT PROBE. Also pure telemetry, also client-side,
    // and — unlike the pair above — a SINGLE object, because it has a single
    // feeder: the OnRep-dispatched correction-state callback bound in
    // registerPredictionOwner, on the GAME thread. There is no physics-thread
    // correction arrival, so there is no second window to keep apart. It also
    // carries no per-id state, which is why unregisterSimulatable forgets the two
    // relay probes and not this one. Full statement at the top of
    // Network/CorrectionVerdictProbe.h.
    CorrectionVerdictProbe m_correctionVerdictProbe;

    // [og-netcode-v2-input-relay item 42 / I2] THE FRONTIER-LANDING SPLIT. Same
    // feeder, same thread and same no-per-id-state rule as the verdict probe above
    // — it is deliberately a second object on the same site rather than three more
    // fields on the first, because the two answer different questions on different
    // denominators (a DISCARDED correction is a non-event for the verdict and a
    // first-class observation for the landing site) and merging them would have
    // forced one of the two to adopt the other's sample set.
    //
    // ITS PHYSICS-THREAD SIBLING IS ON SimulationManager (ResimGateProbe). The two
    // are never shared and never atomic — see the two-object rule at the top of
    // OGSimulation/ResimGateProbe.h.
    CorrectionLandingProbe m_correctionLandingProbe;

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

template <typename T, typename... SimulatableTs>
concept SimulationNetSyncConcept = requires(
    T& t, const SimulationTimeStep& step, uint32 tick, int32 rollbackWindow,
    int32 correctionRotationK)
{
    // [T39] sendCorrectionAll gained the state-rotation width. It is a required
    // argument rather than a defaulted one — see the note at the definition.
    { t.sendCorrectionAll(step, correctionRotationK) };
    { t.sendLocalInputToAuthorityAll(tick, tick) };
    { t.collectInputAll(step) } -> std::convertible_to<ResolvedInputs<SimulatableTs...>>;
    // [T6] MOVED here from SimulationReconciliationConcept along with the method:
    // resim input resolution reads this class's delay lines, relay stores and
    // neutrals, and only borrows the join key from reconciliation.
    { t.collectResimInputAll(tick) } -> std::convertible_to<ResolvedInputs<SimulatableTs...>>;
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
    std::function<typename SimulatableT::InputType(
        const SimulationTimeStep&,
        const ClientInputDelayLine<typename SimulatableT::InputType>&)> inputProvider = nullptr)
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
