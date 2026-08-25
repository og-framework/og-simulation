#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include <atomic>
#include <concepts>
#include <functional>
#include <optional>
#include <tuple>
#include <unordered_map>

#include "OGSimulation/CorrectionStateBufferCodec.h"
#include "OGSimulation/InputResolutionTelemetry.h"
#include "OGSimulation/Network/LocalInputCache.h"
#include "OGSimulation/Network/RemoteInputCache.h"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/SimulationLog.h"
#include "OGSimulation/SimulationObjectStorage.h"
#include "OGSimulation/SimulationQueues.h"
#include "OGSimulation/SimulationReconciliation.h"
#include "OGSimulation/SimulationTimeContext.h"

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// ---------------------------------------------------------------------------
// SimulationInputResolution -- THE RESOLUTION PEER (DesignInputResolutionPeer.md).
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
//
// ⛔ DEPENDENCY SPINE: knows SimulationObjectStorage and SimulationReconciliation
//   ONLY -- never SimulationNetSync, which knows it. §1
// ⛔ Every member arrived here VERBATIM in one relocation, comments included (two
//   named seam exceptions). A comment still saying "SimulationNetSync" is
//   UN-REWORDED MIGRATION PROSE, not an ownership claim; the rewording is still
//   owed. The two that were WRONG rather than merely unreworded are fixed. §1, §15
// ⛔ Every NetSync forwarder for this class's surface is DELETED. §1
// ⛔ The reconciliation edge is ONE READ (getAppliedCaptureTickRef);
//   zero pushPredictionTick / backfillSkippedTick call sites remain here. §8
//
// Relocation history, retired rationale and archived measurement records:
// `docs/SimulationInputResolution-rationale.md`.
//
// Where this peer's two resolution paths sit in the whole path a captured
// input travels, end to end: `docs/Perspective-RemoteInputFlow.md` -- §6
// ("B's half -- the scheduled read, and what happens when nothing arrived")
// is this class's own ladder, §9 ("The resim mirror -- why the same ladder
// is called twice") its resim twin.
// ---------------------------------------------------------------------------

// ===========================================================================
// ORIENTATION -- what this class owns, who touches it, from where. Every fence it
// summarises is still stated at its own site below.
// ===========================================================================
// THE SEVEN CONTAINERS. Provider-presence is the local/remote test, always.
//
//   m_inputProviders        LOCAL only      presence IS the identity test
//   m_localInputCaches      LOCAL only      raw captures, by capture tick
//   m_pendingInputQueues    LOCAL only      outbound send queue
//   m_remoteInputCaches     REMOTE only     relayed inputs, by SENDER's tick
//   m_remoteMoveQueues      AUTHORITY only  inbound client moves
//   m_lastUsedCaptureTicks  AUTHORITY only  the relay's join key
//   m_neutralInputs         per TYPE        the game's zero input
//
// ---------------------------------------------------------------------------
// THE TWO-THREAD ROSTER. Assembled view; each crossing is fenced at its owner.
//
//   what                              GAME thread         PHYSICS thread
//   --------------------------------  ------------------  ------------------
//   m_clientEffectiveInputDelayTicks  write OnRep_Conn-   read once per tick
//     (atomic, relaxed)               ectionTier          (both collects)
//   m_remoteInputCaches               write OnRep_Rel-    read (both collects)
//                                     ayedInputRing
//   m_remoteInputCaches               read getLastRelay-  --
//                                     edInput -- THE ONE
//   m_lastUsedCaptureTicks            read sendCorrec-    write collectInputAll
//                                     tionAll (NetSync)
//   m_localInputCaches                --                  write+read; also
//                                                         wipeAllForResync
//   m_neutralInputs                   write ONCE set-     read
//                                     NeutralInput
//   m_inputResolutionTelemetry        --                  write (every emit*)
//   registration / unregistration     all of it           --
//
// ⛔ getLastRelayedInput is THE ONE game-thread reader of a store the physics
//   thread also reads -- and is same-thread with that store's WRITER, which is
//   why it alone does not participate in the accepted tear. §14
// ⛔ TWO crossings are ACCEPTED TEARS argued elsewhere, NOT re-derived here: the
//   relay stores (Network/RemoteInputCache.h) and the join key (member below).
//
// ---------------------------------------------------------------------------
// WHERE THE TWO COLLECT PATHS ARE CALLED FROM. All three: physics thread.
//
//   prediction  SimulationManager::onGameSimulationPrediction
//                 -> preparePredictionSimulationStep (SimulationStepSequencing.h)
//                 -> collectInputAll THEN reconciliation.allocateFrontierSlotsAll
//   authority   SimulationManager::onGameSimulationAuthority
//                 -> collectInputAll ALONE, deliberately -- no frontier pair
//   resim       SimulationManager::onGameSimulationResimulation
//                 -> collectResimInputAll -- allocates nothing, ever
//
// ⛔ The prediction ordering is CALLER DISCIPLINE, not compiler-enforced. The
//   contract is stated in full at collectInputAll. §8
// ===========================================================================

// ---------------------------------------------------------------------------
// Per-type map aliases for THIS CLASS's own members below -- an earlier label said
// "SimulationNetSync members"; they have been this class's since the relocation. §15
// ---------------------------------------------------------------------------

// The provider takes the delay line as a parameter: the motion-sequence
// matcher runs INSIDE the provider and needs raw capture history. §2
// ⛔ ORDERING CONTRACT: collectInputAll binds the line and calls the provider BEFORE
//   pushing this tick's capture, so a provider sees ticks <= tick-1 and MUST NOT
//   observe the current tick -- which is its own return value. §2
// ⛔ const: a provider reads history, it never writes it.
template <typename T>
using InputProviderMapFor = std::unordered_map<
    unsigned int,
    std::function<typename T::InputType(const SimulationTimeStep&,
                                        const LocalInputCache<typename T::InputType>&)>>;

template <typename T>
using RemoteMoveQueueMapFor = std::unordered_map<
    unsigned int,
    RemoteMoveQueue<typename T::InputType>>;

template <typename T>
using PendingInputQueueMapFor = std::unordered_map<
    unsigned int,
    PendingInputQueue<typename T::InputType>>;

// ⛔ `LastUsedInputMapFor` / `m_lastUsedInputs` ARE GONE with the
//   correction-INPUT channel that was their only reader. §3
// ⛔ DO NOT CONFUSE THEM WITH `LastUsedCaptureTickMapFor` /
//   `m_lastUsedCaptureTicks`, WHICH STAYS: same key set and sites, but it carries
//   the applied-capture-tick REF -- the relay's join key. Load-bearing. §3

// ⛔ `kNoInputCaptureTick` lives in CorrectionStateBufferCodec.h, not here: it is a
//   WIRE value shared by three layers that must not depend on this header. §3

// Per-authority-id capture tick behind the input the authority APPLIED --
// the relay's join key. Populated at registerAuthorityCharacter, written in
// collectInputAll's remote branch; threading at the member.
// ⛔ A STRUCT, NOT A BARE ALIAS: the mapped type does not depend on T, so an
//   alias would give every pack member the identical tuple slot and make the
//   `std::get<LastUsedCaptureTickMapFor<T>>` lookups ill-formed the moment a
//   second simulatable type is registered. §3
template <typename T>
struct LastUsedCaptureTickMapFor
{
    std::unordered_map<unsigned int, uint32> value;
};

// Per-locally-controlled ring of the client's own raw captures. Separate
// from the correction cache -- Network/LocalInputCache.h.
// ⛔ Populated ONLY for provider-owning ids -- exactly m_pendingInputQueues'
//   set. §2
template <typename T>
using LocalInputCacheMapFor = std::unordered_map<
    unsigned int,
    LocalInputCache<typename T::InputType>>;

// Per-REMOTE store of the inputs the server relayed, keyed by the SENDER's
// capture tick.
// ⛔ THE EXACT COMPLEMENT of LocalInputCacheMapFor. PROVIDER PRESENCE IS THE TEST,
//   NEVER A ROLE CHECK -- only it stays correct under COUCH CO-OP, where "the
//   client's character" picks one and hands the rest a store they must not have. §2
// ⛔ NOT wiped by wipeAllForResync -- see the deliberate non-wipe note there, and
//   RemoteInputCache.h for why this is its own type. §10
template <typename T>
using RemoteInputCacheMapFor = std::unordered_map<
    unsigned int,
    RemoteInputCache<typename T::InputType>>;

// ---------------------------------------------------------------------------
// resolveScheduledRelayedInput -- THE UNIFIED SCHEDULED READ. Which relayed
// input does tick N run on, when no authoritative ref exists for N?
// Three-step ladder (RelayDelaySpectrumDesign.md §4/§5.2):
//   0. `!findLatest().valid` -> TERMINAL FALLBACK, and NO PROBE AT ALL;
//   1. probe `find(N - dLatest)`;
//   2. VERIFY the candidate's stamp equals dLatest;
//   3. hit -> candidate; miss or verify-fail -> `fallback()`.
// ---------------------------------------------------------------------------
// ⛔ RUNG 0 SKIPS THE PROBE DELIBERATELY -- `find(N)` can hit for a LAN peer, so a
//   default dA of 0 would be accidentally-safe, not correct. §4
// ⛔ STEP 2 MEANS "the REGIME has not shifted", NOT "this entry is scheduled at
//   N". On mismatch fall back -- never guess. §4
// ⛔ EMERGENT REGIME, NO FLAG -- raising the floor needs no second implementation.
// ⛔ RESIM AND PREDICTION MUST RUN THE IDENTICAL LADDER; a free function, not a
//   private member, so it is unit-testable alone. §4
// ⛔ THE `tick < dA` GUARD REPORTS Miss, NOT NoProbe -- rung 0 is "nothing EVER
//   arrived". §5
// ⛔ DECIDE, THEN PROJECT. Three properties the split must preserve: (1) the
//   no-report path stays as cheap, no new scan or allocation; (2) `residentSpan()`
//   is paid ONLY on a miss and ONLY when a report was asked for, and `decide` NEVER
//   calls it; (3) `NoProbeTick` STAYS DISTINCT FROM `BelowOldest` -- pinned by
//   `RelayMissClass: the tick < dA underflow guard is its OWN class, not
//   belowOldest` (RelayReadProbeTest.cpp). §5
// ---------------------------------------------------------------------------

// What the ladder decided, before any of it becomes a report. Returned BY VALUE:
// `find` copies into an out-param, so the Hit arm has a local to hand back.
// ⛔ EVERY FIELD IS MEANINGLESS ON THE OUTCOMES ITS OWN COMMENT EXCLUDES -- the
//   convention `ScheduledRelayedReadReport` (RelayReadProbe.h) already uses. §5
template <typename InputT>
struct ScheduledRelayedReadDecision
{
    ScheduledRelayedReadOutcome outcome = ScheduledRelayedReadOutcome::NoProbe;

    // ⛔ 0u and MEANINGLESS on NoProbe and on the tick<dA guard -- neither formed a
    //   real probe tick.
    uint32       probeTick   = 0u;
    std::uint8_t dLatest     = 0u;   // meaningless on NoProbe
    std::uint8_t candidateDA = 0u;   // meaningful on Hit / VerifyFail only

    // ⛔ Served when `useCandidate` (Hit only); every other outcome projects to
    //   `store.fallback()`. Filled unconditionally by `find` -- no extra cost.
    InputT candidateInput{};
    bool   useCandidate = false;

    // ⛔ Whether a probe tick was FORMED -- false on rung 0 and on the underflow
    //   guard. Gates the delta projection and the isUnderflowMiss path.
    bool probeTickFormed = false;

    // ⛔ FREE when set -- `decide` already has it from its first line. Meaningless
    //   unless `probeTickFormed`.
    uint32 newestResident = 0u;

    // ⛔ Set ONLY on the tick<dA guard. PROJECT must not re-derive it and must not
    //   pay `residentSpan()` for this rung -- property 3. §5
    bool isUnderflowMiss = false;
};

// ⛔ THE PURE LADDER -- property 1's reason for existing. Side-effect-free, `const`
//   over the store, no out-param to take. Byte-for-byte the arms, conditions,
//   order and values the pre-split ladder shipped. §5
template <typename InputT>
ScheduledRelayedReadDecision<InputT> decideScheduledRelayedRead(
    const RemoteInputCache<InputT>& store, uint32 tick)
{
    ScheduledRelayedReadDecision<InputT> decision;

    const auto latest = store.findLatest();
    if (!latest.valid)
    {
        return decision;                             // rung 0 — no probe at all
    }

    // ⛔ Guard the subtraction rather than wrapping into a ~4-billion capture tick.
    //   Reported Miss, not NoProbe -- classification fence on this function's banner.
    if (tick < static_cast<uint32>(latest.dA))
    {
        decision.outcome         = ScheduledRelayedReadOutcome::Miss;
        decision.dLatest         = latest.dA;
        decision.isUnderflowMiss = true;
        return decision;
    }

    const uint32 probeTick   = tick - static_cast<uint32>(latest.dA);
    decision.probeTick       = probeTick;
    decision.dLatest         = latest.dA;
    decision.probeTickFormed = true;
    decision.newestResident  = latest.captureTick;

    std::uint8_t candidateDA = 0u;
    InputT       candidate{};
    if (store.find(probeTick, candidateDA, candidate))
    {
        decision.candidateDA = candidateDA;
        if (candidateDA == latest.dA)
        {
            decision.outcome        = ScheduledRelayedReadOutcome::Hit;
            decision.candidateInput = candidate;
            decision.useCandidate   = true;
            return decision;
        }

        // Resident, but stamped against a delay that is no longer current: the regime
        // shifted -- a transition, not starvation. Same fallback, different diagnosis.
        decision.outcome = ScheduledRelayedReadOutcome::VerifyFail;
        return decision;
    }

    decision.outcome = ScheduledRelayedReadOutcome::Miss;
    return decision;
}

// ---------------------------------------------------------------------------
// `outDiagnosticReport` -- THE OUTCOME, REPORTED TO THE CALLER RATHER
// THAN COUNTED HERE. A PROJECTION: call `decide` once, turn it into
// candidate-or-fallback, and only behind a non-null pointer into the report.
// ⛔ THE COUNTERS DELIBERATELY DO NOT LIVE HERE -- a design constraint, not style.
//   `decide` must stay pure, and it is SHARED across two threads' call sites. §5
// ⛔ THE TWO CALL SITES ARE COUNTED SEPARATELY -- summing erases real information
//   about the frontier. §5
// ⛔ A DEFAULTED OUT-POINTER, not a widened return type, so every existing call
//   site and test compiles unchanged. §5
// ---------------------------------------------------------------------------
template <typename InputT>
InputT resolveScheduledRelayedInput(const RemoteInputCache<InputT>& store, uint32 tick,
                                    ScheduledRelayedReadReport* outDiagnosticReport = nullptr)
{
    const auto decision = decideScheduledRelayedRead(store, tick);

    if (outDiagnosticReport != nullptr)
    {
        outDiagnosticReport->outcome     = decision.outcome;
        outDiagnosticReport->probeTick   = decision.probeTick;
        outDiagnosticReport->dLatest     = decision.dLatest;
        outDiagnosticReport->candidateDA = decision.candidateDA;

        // ⛔ PROBE B half one -- signed distance from the probe tick to
        //   `newestResident`. Set on EVERY rung that formed a probe tick, Hit INCLUDED:
        //   the hit deltas are the calibration the miss deltas are read against. FREE --
        //   `decide` already has `latest.captureTick` from its first line. §5
        if (decision.probeTickFormed)
        {
            outDiagnosticReport->newestResident     = decision.newestResident;
            outDiagnosticReport->deltaToNewestValid = true;
            outDiagnosticReport->deltaToNewest      = static_cast<std::int32_t>(
                static_cast<std::int64_t>(decision.probeTick)
                - static_cast<std::int64_t>(decision.newestResident));
        }

        // ⛔ PROBE B half two -- WHY the miss happened. The one addition that costs
        //   anything (a second scan), paid ONLY on a miss and ONLY behind the report
        //   guard, so a caller passing no report is as cheap as before the probe. §5
        // ⛔ THE SPAN IS THE STORE'S, NOT THE RING'S: the ring carries `depth` entries per
        //   replication, the store up to 64 arrivals -- which is why an in-span hole is
        //   meaningful at depth 1. The ring's span would make every miss trivially
        //   out-of-span and the classification worthless. §5
        if (decision.outcome == ScheduledRelayedReadOutcome::Miss)
        {
            if (decision.isUnderflowMiss)
            {
                // ⛔ NO PROBE TICK EXISTS -- no delta, no span comparison. Its own miss class
                //   rather than folded into BelowOldest, which it superficially resembles.
                outDiagnosticReport->missClass = ScheduledRelayedReadMissClass::NoProbeTick;
            }
            else
            {
                const auto span = store.residentSpan();
                outDiagnosticReport->spanValid      = span.valid;
                outDiagnosticReport->oldestResident = span.oldest;
                outDiagnosticReport->newestResident = span.newest;
                outDiagnosticReport->residentCount  = static_cast<std::uint32_t>(span.count);

                if (!span.valid)
                {
                    // ⛔ UNREACHABLE BY DESIGN: this arm runs only on a Miss below the rung-0 gate, so
                    //   a slot is occupied. CLASSIFIED RATHER THAN ASSERTED -- a probe must never be
                    //   the thing that brings a session down. §5
                    outDiagnosticReport->missClass = ScheduledRelayedReadMissClass::NoProbeTick;
                }
                else
                {
                    outDiagnosticReport->missClass =
                          (decision.probeTick > span.newest) ? ScheduledRelayedReadMissClass::AboveNewest
                        : (decision.probeTick < span.oldest) ? ScheduledRelayedReadMissClass::BelowOldest
                                                              : ScheduledRelayedReadMissClass::InSpan;
                }
            }
        }
    }

    return decision.useCandidate ? decision.candidateInput : store.fallback();
}

// Per-SIMULATABLE-TYPE (not per-id) neutral input.
// ⛔ A STRUCT KEYED ON THE SIMULATABLE, not a bare `InputType` -- a pack sharing one
//   InputType must still get distinct tuple slots, and `std::get<InputType>` would
//   be silently ill-formed there. §6
// ⛔ `injected` catches a root that injects on the CLIENT ROLE ONLY, which
//   silently reintroduces the (0,0,0) `InputType{}`. It warns AT THE REGISTRATION
//   SITE, still the right place for it. §6
template <typename T>
struct NeutralInputFor
{
    typename T::InputType value{};
    bool                  injected{ false };
};

// ---------------------------------------------------------------------------
// SimulationInputResolution<SimulatableTs...> -- see the ORIENTATION block at the
// top of this file for what it owns, its two-thread roster and its call sites.
// ⛔ Holds refs to SimulationObjectStorage and SimulationReconciliation -- NEVER to
//   SimulationNetSync (the spine, §1). Layer: OGSimulation, UE/Chaos-free.
// ---------------------------------------------------------------------------

template <typename... SimulatableTs>
class SimulationInputResolution
{
public:
    SimulationInputResolution(
        SimulationObjectStorage<SimulatableTs...>& storage,
        SimulationReconciliation<SimulatableTs...>& reconciliation)
        : m_storage(storage)
        , m_reconciliation(reconciliation)
    {}

    void setLogger(std::function<void(const char*)> logger)
    {
        // ⛔ This peer's OWN copy of the split-telemetry logger. The composition root calls
        //   this `setLogger` directly, as it does `m_reconciliation.setLogger` and
        //   `m_netSync.setLogger` -- `SimulationNetSync` has NO forwarding hop for it.
        //   Each sibling gets its OWN copy. §1
        m_inputResolutionTelemetry.setLogger(logger);
        m_logger = std::move(logger);
    }

    // ---------------------------------------------------------------------------
    // Client-side Layer-1 input delay -- ticks the LOCAL client's own capture is held
    // before the integrator sees it. 0 = no delay at all, so an unconfigured instance
    // is unaffected.
    // ---------------------------------------------------------------------------
    // ⛔ ONE SCALAR, NOT A PER-ID MAP: a delay is a property of the local CONNECTION
    //   and a client has exactly one. §14
    // ⛔ GAME->PHYSICS CROSSING: written by OnRep_ConnectionTier (GT), read ONCE per
    //   tick (PT). An atomic suffices because it is a lone independent scalar -- the
    //   worst a stale read does is apply a tier change a tick late. EMPHATICALLY NOT
    //   `ServerInputDelayQueue`'s case, whose shared thing was an unordered_map with
    //   rehash UB, not a stale value. RELAXED is deliberate. §14
    void setClientEffectiveInputDelayTicks(int32 delayTicks)
    {
        m_clientEffectiveInputDelayTicks.store(delayTicks < 0 ? 0 : delayTicks,
                                               std::memory_order_relaxed);
    }

    int32 getClientEffectiveInputDelayTicks() const
    {
        return m_clientEffectiveInputDelayTicks.load(std::memory_order_relaxed);
    }

    // Inject the game's zero input -- fills the [0, effectiveDelay) window at
    // session start and after a resync wipe.
    // ⛔ MUST BE CALLED BY THE COMPOSITION ROOT ON BOTH ROLES -- the default
    //   `InputType{}` is NOT the brawler's zero (`getZeroPlayerInput` builds (0,0,1)
    //   forwards; a value-initialised one carries (0,0,0) into normalisation). §6
    // ⛔ ORDER-INDEPENDENT w.r.t. registration -- lines created later AND already
    //   created. §6
    // ⛔ ONE INJECTION POINT, THREE CONSUMERS, so they can never disagree about "zero":
    //   the delay lines, every relay store's `fallback()`, the AUTHORITY's
    //   underrun substitute. "Client" is not in the name for that reason. §6
    // ⛔ The AUTHORITY path is NOT order-independent, which is why a missed injection
    //   is WARNED about at registerAuthorityOwner. §6
    template <typename SimulatableT>
    void setNeutralInput(const typename SimulatableT::InputType& neutralInput)
    {
        auto& neutral = std::get<NeutralInputFor<SimulatableT>>(m_neutralInputs);
        neutral.value    = neutralInput;
        neutral.injected = true;
        for (auto& [id, line] :
             std::get<LocalInputCacheMapFor<SimulatableT>>(m_localInputCaches))
        {
            line.setNeutralInput(neutralInput);
        }
        for (auto& [id, store] :
             std::get<RemoteInputCacheMapFor<SimulatableT>>(m_remoteInputCaches))
        {
            store.setNeutralInput(neutralInput);
        }
    }

    // Has the composition root injected the game's zero for this type?
    // ⛔ PUBLIC SO THE CONSUMING ROLE CAN BE PINNED BY A TEST -- a missed injection is
    //   silently degraded, never wrong-looking. Read side of registerAuthorityOwner's
    //   warning. §6
    template <typename SimulatableT>
    bool hasNeutralInput() const
    {
        return std::get<NeutralInputFor<SimulatableT>>(m_neutralInputs).injected;
    }

    // ⛔ PUBLIC for the same reason: a test asserting "the authority applied the
    //   GAME's zero" must name the injected value without re-deriving it. §6
    template <typename SimulatableT>
    const typename SimulatableT::InputType& getNeutralInput() const
    {
        return std::get<NeutralInputFor<SimulatableT>>(m_neutralInputs).value;
    }

    // ---------------------------------------------------------------------------
    // Relay-store access -- NULLABLE BY DESIGN. nullptr for every LOCAL character
    // and every id the authority owns.
    // ⛔ NULLABLE RATHER THAN THROWING -- both callers legitimately see both classes of
    //   id. A throwing accessor there is a KNOWN TRAP: the retired
    //   `getLastCorrectionInput` routed through the THROWING `getCacheFor` and threw
    //   on the authority rather than answering "nothing here". §7
    // ---------------------------------------------------------------------------
    template <typename SimulatableT>
    RemoteInputCache<typename SimulatableT::InputType>* findRemoteInputCache(unsigned int id)
    {
        auto& map = std::get<RemoteInputCacheMapFor<SimulatableT>>(m_remoteInputCaches);
        const auto it = map.find(id);
        return it == map.end() ? nullptr : &it->second;
    }

    template <typename SimulatableT>
    const RemoteInputCache<typename SimulatableT::InputType>* findRemoteInputCache(unsigned int id) const
    {
        const auto& map = std::get<RemoteInputCacheMapFor<SimulatableT>>(m_remoteInputCaches);
        const auto it = map.find(id);
        return it == map.end() ? nullptr : &it->second;
    }

    // ---------------------------------------------------------------------------
    // getLastRelayedInput -- LAST-KNOWN, for the GAME-THREAD viz readers. The
    // newest relayed input for a REMOTE character, or nothing if none ever arrived.
    // ---------------------------------------------------------------------------
    // ⛔ THE ONLY SOURCE of a remote character's actual input on the client --
    //   the correction cache's input column does not exist any more. §7
    // ⛔ `findLatest().input`, NOT `fallback()` -- the viz caller must tell "nothing
    //   ever arrived" (viz SKIPPED) from "arrived and was neutral"; `fallback()` would
    //   silently draw an aim indicator at the neutral pose. §7
    // ⛔ NULLABLE FOR TWO SEPARATE REASONS: no store exists for a LOCAL character or on
    //   the AUTHORITY at all, and a store that exists may still be cold. §7
    // ⛔ THREADING -- THE ONE GAME-THREAD READER, and same-thread with the store's
    //   WRITER, so it does NOT participate in the accepted tear. Returns a COPY. §14
    template <typename SimulatableT>
    std::optional<typename SimulatableT::InputType> getLastRelayedInput(unsigned int id) const
    {
        const auto* store = this->template findRemoteInputCache<SimulatableT>(id);
        if (store == nullptr)
            return std::nullopt;

        const auto latest = store->findLatest();
        if (!latest.valid)
            return std::nullopt;

        return std::optional<typename SimulatableT::InputType>(latest.input);
    }

    // ===========================================================================
    // DIAGNOSTIC VIEW -- the one probe this peer owns (`RelayReadProbe`, which arrived
    // here with `InputResolutionTelemetry`).
    // ⛔ `SimulationNetSync::Diagnostics::relayReadProbe()` IS GONE, not delegating.
    //   Callers reach `inputResolution.getDiagnostics().relayReadProbe()`. §1
    // ⛔ CONST-ONLY -- the only writer is the telemetry sibling, on one thread. §14
    // ===========================================================================
    class Diagnostics
    {
    public:
        explicit Diagnostics(const SimulationInputResolution& resolution) : m_resolution(resolution) {}

        const RelayReadProbe& relayReadProbe() const
        { return m_resolution.m_inputResolutionTelemetry.relayReadProbe(); }

    private:
        const SimulationInputResolution& m_resolution;
    };

    Diagnostics getDiagnostics() const { return Diagnostics(*this); }

    // ---------------------------------------------------------------------------
    // Registration / unregistration -- the CONTAINER-LIFECYCLE half of what
    // NetSync's registerPredictionOwner / registerAuthorityOwner /
    // unregisterSimulatable used to do in one method each.
    // ⛔ NETSYNC KEEPS THE BINDING HALF and calls these in a FIXED SEQUENCE (its own
    //   set-invariant comments); two live crashes were fixed to protect it. §7
    // ⛔ These four methods are NEW; every line inside each is a verbatim-moved
    //   fragment of the pre-cut bodies, not a re-derivation. §1
    // ---------------------------------------------------------------------------

    // The provider-present half of the old registerPredictionOwner.
    // ⛔ KEEP m_inputProviders / m_pendingInputQueues IN SYNC with NetSync's
    //   m_localInputSenders -- set-invariant comment at the call site. §7
    template <typename SimulatableT>
    void registerLocalCharacter(
        unsigned int id,
        std::function<typename SimulatableT::InputType(
            const SimulationTimeStep&,
            const LocalInputCache<typename SimulatableT::InputType>&)> inputProvider)
    {
        std::get<InputProviderMapFor<SimulatableT>>(m_inputProviders)
            .emplace(id, std::move(inputProvider));

        // ⛔ try_emplace default-constructs IN PLACE -- PendingInputQueue holds
        //   std::atomic members and is neither copyable nor movable.
        std::get<PendingInputQueueMapFor<SimulatableT>>(m_pendingInputQueues)
            .try_emplace(id);

        // ⛔ The delay line is created for exactly the provider-owning ids. A remote
        //   proxy has no capture of its own to delay, so it MUST NOT get a line --
        //   `collectInputAll`'s proxy branch reads no delay line at all. §2
        std::get<LocalInputCacheMapFor<SimulatableT>>(m_localInputCaches)
            .try_emplace(id,
                std::get<NeutralInputFor<SimulatableT>>(m_neutralInputs).value);
    }

    // The provider-absent half of the old registerPredictionOwner: the neutral-seeded
    // relay store. NetSync binds the callbacks and does the catch-up read after.
    // ⛔ THE EXACT COMPLEMENT: a locally-controlled character MUST NOT get a
    //   relay store -- the server does not relay a character's input back to the
    //   client that produced it. §2
    template <typename SimulatableT>
    void registerRemoteCharacter(unsigned int id)
    {
        std::get<RemoteInputCacheMapFor<SimulatableT>>(m_remoteInputCaches)
            .try_emplace(id,
                std::get<NeutralInputFor<SimulatableT>>(m_neutralInputs).value);
    }

    // The container-lifecycle half of the old registerAuthorityOwner.
    // ⛔ THE INITIAL JOIN-KEY VALUE IS THE SENTINEL, NOT 0 -- before the first
    //   authority tick this id has applied no input at all, and seeding 0 would claim
    //   capture tick 0 was applied. §3
    template <typename SimulatableT>
    void registerAuthorityCharacter(unsigned int id)
    {
        std::get<RemoteMoveQueueMapFor<SimulatableT>>(m_remoteMoveQueues)
            .emplace(id, RemoteMoveQueue<typename SimulatableT::InputType>{});

        std::get<LastUsedCaptureTickMapFor<SimulatableT>>(m_lastUsedCaptureTicks)
            .value.emplace(id, kNoInputCaptureTick);
    }

    // The container-lifecycle half of the old unregisterSimulatable (its step 3).
    // ⛔ NETSYNC CALLS THIS AFTER clearing callbacks and erasing its own writer/sender
    //   maps (steps 1-2) -- the fixed ordering the pre-cut method enforced. §7
    template <typename SimulatableT>
    void unregisterCharacter(unsigned int id)
    {
        std::get<InputProviderMapFor<SimulatableT>>(m_inputProviders).erase(id);
        std::get<RemoteMoveQueueMapFor<SimulatableT>>(m_remoteMoveQueues).erase(id);
        std::get<PendingInputQueueMapFor<SimulatableT>>(m_pendingInputQueues).erase(id);
        std::get<LocalInputCacheMapFor<SimulatableT>>(m_localInputCaches).erase(id);
        // ⛔ Erased on UNREGISTRATION only -- never on resync (wipeAllForResync). §10
        std::get<RemoteInputCacheMapFor<SimulatableT>>(m_remoteInputCaches).erase(id);
        // ⛔ Erased here, populated in registerAuthorityCharacter -- one pairing. §3
        std::get<LastUsedCaptureTickMapFor<SimulatableT>>(m_lastUsedCaptureTicks).value.erase(id);
    }

    // Drops this peer's telemetry sibling's per-id state.
    // ⛔ Called from NetSync::unregisterSimulatable's STEP 4, ALONGSIDE
    //   `m_telemetry.forgetOwner(id)` -- same fixed step, same ordering. §7
    void forgetOwner(unsigned int id)
    {
        m_inputResolutionTelemetry.forgetOwner(id);
    }

    // ---------------------------------------------------------------------------
    // The by-id ingest/drain/query doors.
    // ⛔ NETSYNC'S CALLBACKS CAPTURE `(this, id)` ONLY -- never a reference into a
    //   container this peer owns. That is what stops a callback surviving
    //   `unregisterCharacter`'s erase from dangling; pre-cut it captured `&store` and
    //   only the unregistration ordering kept it out of UB. §7
    // ---------------------------------------------------------------------------

    // Replaces the OnRep-bound relay-arrival callback's body AND the registration-time
    // catch-up read, both by id.
    // ⛔ NetSync's catch-up call DISCARDS the returned report (the
    //   not-fed-to-the-cadence-probe rule); only its OnRep thin-call emits. §7
    template <typename SimulatableT, typename RingT>
    RelayedInputIngestReport ingestRelayRing(unsigned int id, const RingT& ring)
    {
        auto& map = std::get<RemoteInputCacheMapFor<SimulatableT>>(m_remoteInputCaches);
        const auto it = map.find(id);
        if (it == map.end())
        {
            // ⛔ Benign lookup miss -- a late-firing callback after this id's store was
            //   erased. A default-constructed report reads as NeverWritten, the accurate
            //   answer, and NetSync still logs it through the normal path. §7
            return RelayedInputIngestReport{};
        }
        return populateRemoteInputCache<typename SimulatableT::InputType>(it->second, ring);
    }

    // Replaces the RPC-bound remote-move callback's queueing call, by id.
    // ⛔ GUARD CONTEXT CROSSES BY VALUE from NetSync's own members -- the guard stays
    //   NetSync's and this door only ever sees copies. The too-far-future warning and
    //   the `[ReceiveLocalInput]` SIMLOG stay at NetSync's lambda. §7
    template <typename SimulatableT>
    QueueMoveResult queueRemoteMove(unsigned int id, uint32 captureTick,
                                    const typename SimulatableT::InputType& input,
                                    uint32 currentAuthorityTick, int32 rollbackWindowTicks)
    {
        auto& queueMap = std::get<RemoteMoveQueueMapFor<SimulatableT>>(m_remoteMoveQueues);
        const auto it = queueMap.find(id);
        if (it == queueMap.end())
        {
            // ⛔ Benign lookup miss -- see ingestRelayRing; same rule, same door.
            return QueueMoveResult::IdNotRegistered;
        }
        typename SimulatableT::InputType copy = input;
        return it->second.queueMove(std::move(copy), captureTick, currentAuthorityTick, rollbackWindowTicks);
    }

    // Replaces `sendLocalInputToAuthorityAll`'s `.at(id)` with a nullable door; the
    // send `OG_CHECK`s non-null, so today's throw becomes an explicit loud check.
    // ⛔ THE QUEUE TYPE REMAINS THE THREAD SEAM (SimulationQueues.h); this accessor
    //   only hands the consumer end to the consumer thread. §14
    template <typename SimulatableT>
    PendingInputQueue<typename SimulatableT::InputType>* findPendingInputQueue(unsigned int id)
    {
        auto& map = std::get<PendingInputQueueMapFor<SimulatableT>>(m_pendingInputQueues);
        const auto it = map.find(id);
        return it == map.end() ? nullptr : &it->second;
    }

    // THE identity test, replacing the provider-map read
    // `decideCorrectionArrival` used to make on NetSync.
    // ⛔ THE PROVIDER MAP STAYS PRIVATE so a second, cheaper notion of "remote" cannot
    //   grow anywhere else. §2
    template <typename SimulatableT>
    bool isLocallyControlled(unsigned int id) const
    {
        return std::get<InputProviderMapFor<SimulatableT>>(m_inputProviders).count(id) != 0u;
    }

    // ---------------------------------------------------------------------------
    // Applied-capture-tick accessor -- the capture tick behind the input the
    // authority applied for `id`, or kNoInputCaptureTick if that was a substitute or
    // the authority has not ticked this id yet.
    // ⛔ RETURNS THE SENTINEL RATHER THAN THROWING for an unknown id: a caller asking
    //   about an id the authority does not own is in exactly the "no real input
    //   applied" situation the sentinel describes. §3
    // ---------------------------------------------------------------------------
    template <typename SimulatableT>
    uint32 getLastUsedCaptureTick(unsigned int id) const
    {
        const auto& map =
            std::get<LastUsedCaptureTickMapFor<SimulatableT>>(m_lastUsedCaptureTicks).value;
        const auto it = map.find(id);
        return it == map.end() ? kNoInputCaptureTick : it->second;
    }

    // ---------------------------------------------------------------------------
    // Per-tick input resolution (physics thread)
    // ---------------------------------------------------------------------------

    // ---------------------------------------------------------------------------
    // collectInputAll -- resolves every registered id's input for this tick.
    // ALLOCATES NOTHING.
    // ---------------------------------------------------------------------------
    // ⚠ THE SUCCESSOR OBLIGATION (the frontier-pair contract's opening half, stated
    // here because this is the call a future author sees first): THIS CALL MUST BE
    // FOLLOWED BY RECONCILIATION'S FRONTIER-ALLOCATING SWEEP IN THE SAME PHASE,
    // BEFORE ANYTHING ELSE RUNS. Collecting without allocating leaves capture
    // (`postPredictionAll`) free to push state into the PREVIOUS frontier slot -- the
    // detector's uncovered direction (blind spot #2, `CorrectionCache.h`'s
    // `m_frontierSlotAwaitingState`). This class can no longer prevent that by
    // construction: it is CALLER DISCIPLINE, not compiler-enforced, and this sentence
    // is the record of that trade.
    //
    // ⛔ Full contract text: `SimulationReconciliation::allocateFrontierSlotsAll` --
    //   NOT RE-DERIVED HERE. §8
    // ⛔ THE DOCUMENTED DOOR is `preparePredictionSimulationStep`
    //   (`SimulationStepSequencing.h`, NOT this header). Default-correctness, NOT
    //   ENFORCEMENT -- blind spot #2 is unchanged by it. §8
    // ⛔ THE AUTHORITY PATH CALLS THIS ALONE, ON PURPOSE. §8
    // ⛔ NAME REVERSAL ON THE RECORD -- an interim `prepareSimulationStep` name rested
    //   on "PREPARE allocates"; the allocation is gone, so the name went back.
    //   Recorded, not silently undone. §8
    // ⛔ ONE PASS, NOT TWO. The interim sweep split, its boundary fence and its
    //   exception-safety debt paragraph are GONE from here; the debt DECISION carries
    //   forward on `preparePredictionSimulationStep`. §8, §15
    // ⛔ SKELETON -- dispatch is `collectInputForCharacter`, straight fold.
    // ---------------------------------------------------------------------------
    ResolvedInputs<SimulatableTs...> collectInputAll(const SimulationTimeStep& step)
    {
        ResolvedInputs<SimulatableTs...> inputs;

        // ⛔ ONE LOAD PER TICK for every locally-controlled simulatable. Reading it
        //   once rather than per id is what makes a concurrent OnRep_ConnectionTier write
        //   unable to split a single tick across two different delays. §14
        const int32 effectiveDelay = getClientEffectiveInputDelayTicks();

        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            collectInputForCharacter<T>(id, step, effectiveDelay, inputs);
        });

        // PROBES 1 + 3, per-window summary -- driven from here because this is the
        // only place with a monotonic tick AND a guarantee of running once per tick.
        // ⛔ PHYSICS-THREAD WINDOW. The game-thread arrival probe has its OWN window
        //   object; Network/RelayReadProbe.h states why they must not be merged. §14
        // ⛔ This reaches `InputResolutionTelemetry`, not `NetSyncTelemetry` --
        //   see that header for which of ITS methods this call site may reach. §14
        m_inputResolutionTelemetry.emitRelayReadWindowIfDue(step.getTick());

        // ⛔ THIS METHOD ALLOCATES NOTHING AND RETURNS HERE. The former sweep-boundary
        //   fence and the whole-tick frontier sweep call are GONE from this class; the
        //   allocation is `SimulationReconciliation`'s. §8
        return inputs;
    }

    // ---------------------------------------------------------------------------
    // Resim replay input (physics thread) -- THE RESOLUTION TABLE.
    // ---------------------------------------------------------------------------
    // ⛔ ALLOCATES NOTHING, EVER. Resim opens no frontier pair -- verified at all four
    //   call sites: `prepareResimAll`, here, `postResimulationAll`, `applyResimAll`. §9
    // ⛔ ONLY THE JOIN KEY comes from reconciliation (`getAppliedCaptureTickRef`). §9
    //
    // TWO-LEVEL DISPATCH: TICK CLASS x CHARACTER CLASS.
    //
    //                    | LOCAL (provider present)   | REMOTE (proxy)
    //   -----------------+----------------------------+---------------------------
    //   Ref (corrected)  | delayLine.at(ref)          | store.find(ref) -> input
    //   Sentinel         | injected game zero         | injected game zero
    //   ...store miss    | line miss -> its neutral   | store.fallback() (SELF-HEAL)
    //   NoRef (frontier) | delayLine.at(t - d)        | the scheduled read
    //   NoSlot           | no entry at all -- the character is not in this resim
    //
    // ⛔ CHARACTER CLASS IS PROVIDER PRESENCE, NEVER A ROLE CHECK -- couch co-op; the
    //   full reason is at RemoteInputCacheMapFor, not re-derived here. §9
    // ⛔ THE PRECEDENCE RULE (RelayDelaySpectrumDesign.md §5.3): WHEREVER A REF EXISTS,
    //   THE REF WINS and the `dA` stamp is IGNORED -- a capture can be released LATE,
    //   so consulting the stamp replays the wrong one. §9
    // ⛔ NoSlot EMITS NOTHING, AND THAT IS LOAD-BEARING -- emitting for a slotless
    //   character integrates it from an UN-RESTORED state. §9
    // ⛔ THE D2 FRONTIER EDGE -- DOCUMENTED, DELIBERATELY NOT SOLVED: the
    //   NoRef/local row re-derives with the CURRENT delay. DO NOT "fix" it by stashing
    //   the per-tick delay without re-opening the design decision. §9
    // ⛔ RESYNC INTERPLAY, stated so it is not rediscovered as a regression -- the
    //   asymmetry itself is ruled at wipeAllForResync, NOT RE-DERIVED HERE. §10
    // ⛔ SKELETON -- `collectResimInputForCharacter`, straight fold per rung.
    // ---------------------------------------------------------------------------
    ResolvedInputs<SimulatableTs...> collectResimInputAll(uint32 simTick)
    {
        ResolvedInputs<SimulatableTs...> inputs;

        // ⛔ ONE LOAD PER RESIM TICK, mirroring collectInputAll's rule. See the D2
        //   frontier-edge fence on this method's banner for what it does NOT promise.
        const int32 effectiveDelay = getClientEffectiveInputDelayTicks();

        // ⛔ LOG VOLUME, DELIBERATELY BOUNDED: the rungs are mutually exclusive, so
        //   exactly ONE line per character per resim tick -- and it is [Verbose]-prefixed
        //   while its `[Resim.*]` neighbours are not, so an operator can silence this
        //   table trace alone (LogOGSim=Log). §12

        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            collectResimInputForCharacter<T>(id, simTick, effectiveDelay, inputs);
        });

        return inputs;
    }

    // Drops queued local inputs produced against the pre-resync prediction clock.
    // ⛔ Invoked from the ClientPredictionClock resync callback ALONGSIDE the
    //   reconciliation cache wipe -- one pairing. §10
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

        // ⛔ THE DELAY LINE IS KEYED BY CAPTURE TICK against the pre-resync clock,
        //   which has jumped. Dropping it re-enters the neutral-filled window -- the same
        //   state as session start, which is why part 4 is not special-cased to tick 0. §10
        // ⛔ SECOND CONSUMER, SECOND CONSEQUENCE, stated so it is not rediscovered as
        //   a bug: this also blanks the motion matcher for up to
        //   `inputSequence::kHistoryWindowFrames` (30) ticks. CORRECT, not a regression --
        //   that data was keyed to the pre-resync clock. §10
        forEachTypeMap(m_localInputCaches, [&]<typename T>(auto& perTypeMap) {
            for (auto& [id, line] : perTypeMap)
            {
                SIMLOG(m_logger,
                    "[TimeResync.WipeLocalInputCache] id=%u newPredictionTick=%u",
                    id, newPredictionTick);
                line.clear();
            }
        });

        // m_remoteInputCaches is DELIBERATELY NOT WIPED HERE,
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
        // type rather than a reused LocalInputCache — see the naming ruling in
        // Network/RemoteInputCache.h. A reused type would have been swept by
        // whoever next mirrored these per-id map loops.)
    }

private:
    // `collectInputAll`'s per-character body, lifted out verbatim -- three
    // mutually exclusive branches (local provider / remote queue / simulated proxy),
    // each ending in a `map.emplace`, none containing a `return`.
    // ⛔ PATTERN 1, STRAIGHT FOLD: exactly ONE `return` here, never interleaved BETWEEN
    //   two probe/log calls the way `decideCorrectionArrival` had to guard. §11
    // ⛔ FRONTIER ALLOCATION DOES NOT HAPPEN HERE -- this class has no
    //   allocating sweep at all. What stays is the delay-line capture gate and the
    //   send enqueue, re-gated on the SAME captured predicate, not a second call. §8
    // ⛔ `T& simulatable` DROPPED FROM THE SIGNATURE with the allocation. §8
    //
    // ⚠ MISATTRIBUTION CORRECTED -- TWO of this function's
    // `.at(id)` lookups are in the LOCAL-PROVIDER branch and can throw a real,
    // unwinding exception under `/EHsc` if provider-present-implies-entry-present has
    // already been broken elsewhere: the delay-line fetch and the send enqueue. The
    // THIRD -- the last-used-capture-tick write -- is NOT; it is in the
    // AUTHORITY/QUEUE branch, guarding `registerAuthorityCharacter`'s pairing. An
    // earlier version misattributed all three, through two reviews. What happens at
    // the CALLER when one throws -- the debt-acceptance decision -- is at
    // `preparePredictionSimulationStep` (`SimulationStepSequencing.h`). §8, §15
    template <typename T>
    void collectInputForCharacter(unsigned int id, const SimulationTimeStep& step,
                                  int32 effectiveDelay, ResolvedInputs<SimulatableTs...>& inputs)
    {
        auto& providerMap = std::get<InputProviderMapFor<T>>(m_inputProviders);
        auto& queueMap    = std::get<RemoteMoveQueueMapFor<T>>(m_remoteMoveQueues);
        auto& map         = std::get<std::unordered_map<unsigned int, typename T::InputType>>(inputs);

        if (auto it = providerMap.find(id); it != providerMap.end())
        {
            // HOISTED ABOVE THE PROVIDER CALL -- the line is passed INTO the provider,
            // which runs the game's motion-sequence matcher. The provider sees history up to
            // `tick - 1` ONLY; the current tick's sample is its own return value.
            // ⛔ `.at(id)` RATHER THAN A NULLABLE LOOKUP, ON PURPOSE: the line is created iff a
            //   provider is registered -- by `registerLocalCharacter`, THIS class, NOT by
            //   `SimulationNetSync::registerPredictionOwner` -- so a throw here means that
            //   invariant is already broken, exactly when a silent fallback would be wrong. §2
            auto& delayLine =
                std::get<LocalInputCacheMapFor<T>>(m_localInputCaches).at(id);

            // The RAW capture, as the local player produced it THIS tick.
            const auto capture = it->second(step, delayLine);

            // Layer-1 client input delay. Two values leave this branch: `capture` is the
            // ORIGINAL, undelayed input at the CURRENT tick and is what goes on the wire;
            // `applied` is the capture from `tick - effectiveDelay` and is what the integrator
            // predicts with. At delay 0 the two are the same input.
            // ⛔ SENDING AN ALREADY-DELAYED INPUT has the server delay it a SECOND time and the
            //   two ends diverge by exactly `effectiveDelay` ticks. §11
            // ⛔ `applied` is consumed by the integrator and nothing else; the RAW
            //   capture is what persists, in the delay line. §11
            // ⛔ THE THIRD `!= Stall` GATE, RULED RATHER THAN LEFT LITERAL --
            //   capture admission and frontier allocation are today the SAME decision. A
            //   future StepKind wanting them to differ introduces a second, NAMED predicate.
            //   DO NOT "simplify" this back to `!= StepKind::Stall`. §11
            // ⛔ CAPTURED ONCE, USED TWICE -- this class's ONLY remaining call to
            //   `stepAllocatesFrontierSlot`; `SimulationReconciliation.h` holds the other two
            //   of the THREE production call sites its own banner counts. §8
            const bool allocatesFrontierSlotThisStep = stepAllocatesFrontierSlot(step.getStepKind());
            if (allocatesFrontierSlotThisStep)
            {
                delayLine.push(static_cast<int32>(step.getTick()), capture);
            }

            typename T::InputType applied = resolveDelayedInput(
                delayLine, static_cast<int32>(step.getTick()), effectiveDelay, capture);

            m_inputResolutionTelemetry.emitLocalInputRead(id, step.getTick(), step.getStepKind(), effectiveDelay);

            // ⛔ `backfillSkippedTick` and `pushPredictionTick` do not run in this
            //   class at all any more; the replacement is on `SimulationReconciliation`. The
            //   `pushPredictionInput` removal note moved with the tick push, all the way
            //   to `CorrectionCache.h`. §8
            if (allocatesFrontierSlotThisStep)
            {
                // ⛔ ORIGINAL capture, current tick -- the send channel carries the UNDELAYED
                //   value. DO NOT PASS `applied` HERE. §11
                std::get<PendingInputQueueMapFor<T>>(m_pendingInputQueues)
                    .at(id).enqueue(step.getTick(), capture);
            }

            map.emplace(id, std::move(applied));
        }
        else if (auto qit = queueMap.find(id); qit != queueMap.end())
        {
            // ⛔ UNDERRUN MUST BE DETECTED HERE, BEFORE THE DEQUEUE. An empty queue returns
            //   a Move whose `tick` is 0 -- an ordinary capture tick at session start -- so the
            //   RETURNED tick cannot tell a substitute from a real tick-0 capture. Only the
            //   pre-dequeue `empty()` gate can. §11
            const bool underrun = qit->second.empty();
            auto move = qit->second.dequeueMove();
            m_inputResolutionTelemetry.emitRemoteQueueRead(id, step.getTick(), move.tick);
            // ⛔ THE RELAY'S JOIN KEY is the ORIGINAL capture tick of the input just
            //   applied. The drain delivers the entry's STORED captureTick rather than a
            //   reconstructed `simTick - delay`, so it survives an overdue release. §3
            // ⛔ THE REF STAYS THE SENTINEL ON AN UNDERRUN: no client capture stands
            //   behind the game's zero either. Neutral injection fixed WHICH input is
            //   substituted, not how it is classified. §3
            std::get<LastUsedCaptureTickMapFor<T>>(m_lastUsedCaptureTicks).value.at(id) =
                underrun ? kNoInputCaptureTick : move.tick;

            if (underrun)
            {
                // ⛔ THE SUBSTITUTE IS THE GAME'S ZERO INPUT, not `move.input` -- which on an
                //   empty queue is an `InputType{}`, the (0,0,0) forwards LocalInputCache.h names
                //   as normalisation-breaking. §6
                // ⛔ NOT A LOSS-ONLY PATH: every tick between registration and the client's first
                //   input underruns -- every join window, in ordinary play. §6
                const auto& neutral = std::get<NeutralInputFor<T>>(m_neutralInputs).value;
                map.emplace(id, neutral);
            }
            else
            {
                // ⛔ The dequeued input passes through UNTOUCHED on this arm -- neutral
                //   injection changed the UNDERRUN arm only, never this one.
                map.emplace(id, std::move(move.input));
            }
        }
        else
        {
            // -----------------------------------------------------------
            // SIMULATED-PROXY BRANCH -- THE UNIFIED SCHEDULED READ.
            // -----------------------------------------------------------
            // The client is predicting a character it does not control. This branch used to
            // read the CORRECTION CACHE's last server-reported input -- about one RTT stale.
            // ⛔ ONE CODE PATH, NO REGIME FLAG -- the regime is decided per input, per
            //   receiver, by whether the data is there. Argued at
            //   `resolveScheduledRelayedInput`. §12
            // ⛔ "NEARLY ALWAYS" IS DELIBERATE ON BOTH ENDS -- a hit at floor 0 is legitimate,
            //   which is why the degenerate-equivalence test pins byte-equivalence to
            //   empty/behind-store conditions ONLY, not absolutely at floor 0 (review A4). §12
            // ⛔ THE SAME FUNCTION THE RESIM PATH CALLS, on purpose. DO NOT INLINE A SECOND
            //   COPY OF THE LADDER HERE. §12
            // ⛔ THE TERMINAL VALUE IS THE INJECTED GAME ZERO, never `InputType{}` -- this
            //   branch used to read `cached.value_or(InputType{})`, the (0,0,0) poison that
            //   left the authority path, on EVERY tick of the idle window. §12
            // ⛔ NULLABLE STORE LOOKUP -- a character can be seen here before registration
            //   completes. §7
            // ⛔ THE READ IS CLASSIFIED HERE (probes 1+3), which keeps prediction and
            //   resim counted separately. A MISSING STORE IS NOT A READ and is deliberately
            //   not counted -- folding it in makes an unregistered proxy look starved. §12
            const auto* store = this->template findRemoteInputCache<T>(id);
            ScheduledRelayedReadReport readReport;
            typename T::InputType input =
                store != nullptr
                    ? resolveScheduledRelayedInput(*store, step.getTick(), &readReport)
                    : std::get<NeutralInputFor<T>>(m_neutralInputs).value;

            // ⛔ The probe/log block and the `[CollectInput]` line fold into ONE
            //   emit* call -- safe because nothing after this point branches on whether the
            //   fold happened. §11
            // ⛔ `store != nullptr` rather than `store`: the helper never
            //   dereferenced it, so the telemetry sibling gets the one bit it needs instead of
            //   a reference into this class's own map. §13
            m_inputResolutionTelemetry.emitPredictionInputRead(id, step.getTick(), store != nullptr, readReport);

            // ⛔ `backfillSkippedTick` and `pushPredictionTick` do not run in this
            //   class at all. This branch never allocated and never wrote an input column.
            //   The relay store holds what the proxy actually sent; nothing here
            //   stores this client's guess. §8

            map.emplace(id, std::move(input));
        }
    }

    // -----------------------------------------------------------------------
    // FRONTIER ALLOCATION -- THE WHOLE-TICK SWEEP -- IS GONE FROM THIS CLASS.
    // RETIRED, NOT MOVED VERBATIM. §13
    // -----------------------------------------------------------------------
    // The whole-tick sweep method and its per-character helper used to
    // live here, gated on a `queueMap` lookup (this class's own
    // `m_remoteMoveQueues`) plus a loud-failure `OG_CHECK` against
    // `m_reconciliation.findCorrectionCache`. Both are DELETED, not moved
    // verbatim: the replacement, Reconciliation's own frontier-allocating
    // sweep, filters on reconciliation's OWN cache population
    // (`findCorrectionCache` alone, no `queueMap` reference — see that method's
    // banner for why this class's `m_remoteMoveQueues` was never
    // load-bearing for the exclusion, only a second, redundant way of
    // saying "has a cache") and silently skips rather than aborting loudly
    // (that guard is traded away — priced, not hidden, at the new
    // site's banner). Grep proof this file no longer defines or calls the
    // relocated sweep by name — its acceptance criterion is a case-
    // insensitive zero-hit grep for the retired names across this whole
    // TU, which is why this note is deliberately written without spelling
    // them literally.
    //
    // ⛔ The three collect branches' classification lines and probing
    //   tails live on the PT telemetry sibling as
    //   `InputResolutionTelemetry::emitLocalInputRead` / `emitRemoteQueueRead` /
    //   `emitPredictionInputRead`; the last takes `bool hasStore`, not a
    //   `RemoteInputCache<InputT>*`. §13

    // `collectResimInputAll`'s per-character body, lifted out verbatim -- the
    // TWO-LEVEL RESOLUTION TABLE from that method's banner, as mutually exclusive rungs.
    // ⛔ PATTERN 1 AT PER-RUNG GRANULARITY -- within a rung the diagnostics are never
    //   split by a `return`. §11
    // ⛔ ONLY ONE PROBE WRITE here (`noteResimRead`), sharing no counter with a
    //   sibling, so there is no discard-population hazard. §11
    template <typename T>
    void collectResimInputForCharacter(unsigned int id, uint32 simTick, int32 effectiveDelay,
                                       ResolvedInputs<SimulatableTs...>& inputs)
    {
        auto& map = std::get<std::unordered_map<unsigned int, typename T::InputType>>(inputs);

        const AppliedCaptureRef ref =
            m_reconciliation.template getAppliedCaptureTickRef<T>(id, simTick);

        if (ref.kind == AppliedCaptureRefKind::NoSlot)
        {
            m_inputResolutionTelemetry.emitResimNoSlot(id, simTick);
            return;
        }

        const auto& neutral = std::get<NeutralInputFor<T>>(m_neutralInputs).value;

        if (ref.kind == AppliedCaptureRefKind::Sentinel)
        {
            // ⛔ BOTH CHARACTER CLASSES, ONE ANSWER: the authority applied no client capture,
            //   and its substitute is the INJECTED GAME ZERO. Reproducing an authority
            //   decision means applying what the authority applied -- never `InputType{}`. §9
            m_inputResolutionTelemetry.emitResimSentinel(id, simTick);
            map.emplace(id, neutral);
            return;
        }

        auto& providerMap  = std::get<InputProviderMapFor<T>>(m_inputProviders);
        const bool isLocal = providerMap.find(id) != providerMap.end();

        if (isLocal)
        {
            // ⛔ `.at(id)` for the reason collectInputAll's local-provider branch uses it:
            //   provider-present and line-present are the same condition. §2
            const auto& delayLine =
                std::get<LocalInputCacheMapFor<T>>(m_localInputCaches).at(id);

            const int32 captureTick = (ref.kind == AppliedCaptureRefKind::Ref)
                ? static_cast<int32>(ref.captureTick)
                : static_cast<int32>(simTick) - effectiveDelay;

            // ⛔ A LINE MISS ANSWERS WITH THE LINE'S OWN INJECTED NEUTRAL, never `InputT{}`.
            //   Post-resync that is the only reachable way to get here. §9
            // ⛔ NOT `resolveDelayedInput`: that helper protects the LIVE capture at delay 0,
            //   and a resim has no live capture. `at(simTick)` at delay 0 is right precisely
            //   because the original prediction pass pushed that tick's capture. §9
            m_inputResolutionTelemetry.emitResimLocalRead(id, simTick, ref.kind, captureTick);
            map.emplace(id, delayLine.at(captureTick));
            return;
        }

        // ⛔ REMOTE, NULLABLE BY DESIGN: the authority allocates no stores for ids it
        //   owns, and a character can be iterated before registration completes. Neutral
        //   rather than a throw, matching every other reader. §7
        const auto* store = findRemoteInputCache<T>(id);
        if (store == nullptr)
        {
            m_inputResolutionTelemetry.emitResimNoStore(id, simTick);
            map.emplace(id, neutral);
            return;
        }

        if (ref.kind == AppliedCaptureRefKind::Ref)
        {
            // ⛔ THE REF WINS -- precedence fence on this method's banner. `outDA` is bound
            //   only because `find` requires an out-param; DELIBERATELY NEVER READ. §9
            std::uint8_t   ignoredScheduleStamp = 0u;
            typename T::InputType relayed{};
            const bool hit = store->find(ref.captureTick, ignoredScheduleStamp, relayed);
            m_inputResolutionTelemetry.emitResimRefRead(id, simTick, ref.captureTick, hit);
            // ⛔ THE SELF-HEAL: a miss degrades THIS TICK's replay input to last-known, never
            //   the injected state. The state is a complete anchor. §9
            map.emplace(id, hit ? relayed : store->fallback());
            return;
        }

        // NoRef / REMOTE -- no authoritative answer, so run the same scheduled read the
        // prediction pass runs.
        // ⛔ SHARING THE LADDER stops a resim disagreeing with the prediction it replays.
        // ⛔ SECOND CALL SITE OF PROBE 1 -- its OWN counter block, NEVER summed with
        //   the prediction one; it does NOT feed the stale run (resim revisits ticks out
        //   of order) and does NOT advance the window (`simTick` walks backwards). §12
        ScheduledRelayedReadReport readReport;
        typename T::InputType scheduled =
            resolveScheduledRelayedInput(*store, simTick, &readReport);
        m_inputResolutionTelemetry.emitResimScheduledRead(id, simTick, readReport);
        map.emplace(id, std::move(scheduled));
    }

    // ⛔ The five resim rungs' classification lines and the NoRef rung's
    //   probe write live on the PT telemetry sibling as `emitResimNoSlot` /
    //   `emitResimSentinel` / `emitResimLocalRead` / `emitResimNoStore` /
    //   `emitResimRefRead` / `emitResimScheduledRead`. §13

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

    // ⛔ `std::function` INTENTIONALLY -- a pointer struct would need
    //   PredictionSyncedBufferOwnerConcept extended with a typed
    //   `getLocalInputFor<T>(step)`; tracked as a post-cutover follow-up.
    std::tuple<InputProviderMapFor<SimulatableTs>...>    m_inputProviders;
    std::tuple<RemoteMoveQueueMapFor<SimulatableTs>...>  m_remoteMoveQueues;
    std::tuple<PendingInputQueueMapFor<SimulatableTs>...> m_pendingInputQueues;

    // The capture tick behind each applied remote input, or kNoInputCaptureTick
    // on an underrun substitution.
    // ⛔ WRITTEN IN collectInputAll (PHYSICS), READ IN sendCorrectionAll (GAME, on
    //   SimulationNetSync). §14
    // ⛔ AN INHERITED PATTERN, NOT A NEW ONE -- the strict parallel of the retired
    //   `m_lastUsedInputs`. Tolerable because it is a plain scalar per id: a torn
    //   schedule carries the previous tick's value for one tick, healed by the
    //   every-frame correction that transports it. §14
    std::tuple<LastUsedCaptureTickMapFor<SimulatableTs>...> m_lastUsedCaptureTicks;

    // Client Layer-1 input delay lines.
    // ⛔ Populated for provider-owning ids ONLY; touched EXCLUSIVELY from
    //   collectInputAll (physics) and wipeAllForResync. §14
    std::tuple<LocalInputCacheMapFor<SimulatableTs>...> m_localInputCaches;

    // The game's zero input, per simulatable type.
    // ⛔ NOT "client" neutrals any more -- the AUTHORITY reads this too. The old name
    //   described one of several consumers and invited exactly the reasoning that left
    //   the authority path integrating `InputType{}`. §6
    // ⛔ Written ONCE by the composition root (setNeutralInput, game thread), read on
    //   the physics thread thereafter. §14
    std::tuple<NeutralInputFor<SimulatableTs>...>            m_neutralInputs;

    // The client's relayed-input stores, one per REMOTE character
    // (provider-ABSENT ids -- the complement of m_localInputCaches).
    // ⛔ WRITTEN ON THE GAME THREAD (OnRep_RelayedInputRing), READ ON THE PHYSICS
    //   THREAD (both collects). The crossing is INHERITED from the correction path and
    //   its full rationale is the THREADING section of Network/RemoteInputCache.h.
    //   DO NOT RESTATE IT HERE; DO NOT WEAKEN IT THERE. §14
    std::tuple<RemoteInputCacheMapFor<SimulatableTs>...> m_remoteInputCaches;

    // OWNED HERE, NOT ON NETSYNC. The PHYSICS-THREAD-ONLY telemetry
    // sibling: `RelayReadProbe` and its ten `emit*` helpers.
    // ⛔ `NetSyncTelemetry` -- the other, GAME-THREAD-ONLY sibling -- stays on
    //   `SimulationNetSync`. `InputResolutionTelemetry.h` holds the probe
    //   declaration, the fence on each method, and that class's two-thread rule. §14
    InputResolutionTelemetry m_inputResolutionTelemetry;

    // Client effective input delay.
    // ⛔ Written on the GAME thread (OnRep_ConnectionTier, via the adapter calling this
    //   class directly -- there is no `SimulationNetSync` forwarder), read once per
    //   tick on the PHYSICS thread. ATOMIC -- AND ONLY ATOMIC -- for the reasons at
    //   setClientEffectiveInputDelayTicks. 0 = no delay. §14
    std::atomic<int32> m_clientEffectiveInputDelayTicks{ 0 };

    SimulationObjectStorage<SimulatableTs...>&   m_storage;
    SimulationReconciliation<SimulatableTs...>&  m_reconciliation;
    std::function<void(const char*)>             m_logger;
};

// ---------------------------------------------------------------------------
// SimulationInputResolutionConcept -- the SimulatableTs-exact peer concept,
// mirroring SimulationNetSyncConcept's shape and placement.
// ⛔ CHECKS EXACTLY THE THREE MEMBERS design §C.7 names as having LEFT
//   `SimulationNetSyncConcept` (`collectInputAll` -- named `prepareSimulationStep`
//   for one interim revision -- `collectResimInputAll`, `wipeAllForResync`). §1
// ⛔ THE AD-HOC, CONCRETE-SimulatableTs CHECK. The manager-facing, PACK-INVISIBLE
//   split is `SimulationInputResolutionTickConcept` (`SimulationManager.h`) -- do
//   not duplicate it here. §14
// ---------------------------------------------------------------------------

template <typename T, typename... SimulatableTs>
concept SimulationInputResolutionConcept = requires(T& t, const SimulationTimeStep& step, uint32 tick)
{
    { t.collectInputAll(step) } -> std::convertible_to<ResolvedInputs<SimulatableTs...>>;
    { t.collectResimInputAll(tick) } -> std::convertible_to<ResolvedInputs<SimulatableTs...>>;
    { t.wipeAllForResync(tick) };
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
