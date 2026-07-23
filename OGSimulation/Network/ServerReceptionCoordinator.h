#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "OGSimulation/InputRedundancyBundleCodec.h"
#include "OGSimulation/Network/ConnectionSlotKey.h"
#include "OGSimulation/Network/ConnectionTierTable.h"
#include "OGSimulation/Network/ServerInputDelayQueue.h"
#include "OGSimulation/PCTimeManagement/TimeConfig.h"
#include "OGSimulation/SimulationLog.h"

// ---------------------------------------------------------------------------
// ServerReceptionCoordinator<Address, SimulatableTs...> — the engine-agnostic
// OWNER and ORCHESTRATOR of the server's per-connection reception state.
// (og-netcode-v2-arch-latency / T20; the engine-boundary refactor.)
//
// WHY THIS TYPE EXISTS. Stage 3/5 delivered the per-connection primitives
// (`ConnectionTierTable`, `ServerInputDelayQueue`, `ConnectionSlotKey`) into the
// sim core — but their OWNERSHIP and the logic that ORCHESTRATES them (RTT
// sample -> tier derivation, park input, drain due input, reap dropped
// connections) stayed UE-side on `ASimulationManagerUImpl`. That reception
// policy is entirely engine-agnostic, so leaving it in the UE glue blocked a
// second engine (Godot) from reusing it. This type pulls that ownership and
// orchestration down into the core, leaving the UE layer a THIN TRANSPORT
// ADAPTER that only acquires engine primitives (Address, playerSlot, RTT,
// sim-tick, wire decode) and forwards them here.
//
// A SEPARATE TYPE, not folded into SimulationNetSync. SimulationNetSync is
// client+server-mixed and physics-thread-adjacent; the reception state here is
// AUTHORITY-ONLY and GAME-THREAD-ONLY. Keeping it in its own type is what makes
// that contract legible (see the threading note below).
//
// ---------------------------------------------------------------------------
// THREADING CONTRACT — AUTHORITY-ONLY, GAME-THREAD-ONLY. (Migrated verbatim in
// intent from ASimulationManagerUImpl.h:357-371.)
//
// Every member here is touched ONLY from the game thread. The tier table is fed
// in `noteRttSample` (input-RPC receive path), input is parked in
// `receiveRemoteInput` (also the RPC path), and the queue is drained in
// `releaseDelayedInputs` (the Chaos game-thread hook immediately before the
// physics step). None of the owned containers has any internal synchronization
// (plain `std::unordered_map` / `std::deque`), so NONE may be touched from the
// physics thread, where `onGameSimulationAuthority` runs under
// `bTickPhysicsAsync=True`. The game -> physics transition stays where it always
// was: `releaseDelayedInputs` hands each released input to the adapter's
// `deliver` callback, which feeds RemoteMoveQueue — the seam the server input
// path already crosses (lead resolution R2, 2026-07-20).
//
// ---------------------------------------------------------------------------
// OWNERSHIP + LIFETIME. This type OWNS the tier table and the delay queue
// directly (they were `std::optional` members on the UE manager). Construction
// order is load-bearing and enforced by member-declaration order: the queue
// borrows the tier table by reference, so the table is declared — and therefore
// constructed — BEFORE the queue, and destroyed AFTER it (reverse order). Both
// borrow `const TimeConfig&`; the coordinator itself must not outlive that
// config. On the UE side the whole coordinator is a `std::optional` emplaced in
// BeginPlay's authority branch (after the core manager, whose TimeConfig it
// borrows) and reset in EndPlay.
//
// ---------------------------------------------------------------------------
// THE CLAIM MAP IS id-KEYED, NOT engine-pointer-keyed. (fable review B2'.) The
// delivery-routing map stores `ConnectionSlotKey -> id` — a plain simulatable
// id, not a `TWeakObjectPtr` the core cannot own. GC-liveness of the owning
// component is therefore NOT read here; component death is handled by the
// established unregister contract (`forgetOwner`, called from the adapter's
// unregister path — the same lifecycle seam SimulationNetSync::unregisterSimulatable
// rides) plus the `deliver` callback reporting a dead owner. WIRE death is still
// handled here, by `reapConnections`, off the Address half of the key.
//
// ENGINE-AGNOSTIC. Sim-core header: includes ONLY other `OGSimulation/` headers
// and the STL. The `Address` half stays opaque, bound in production to
// `FUEConnectionHandle` (OGSimulationUnreal) and in the Catch2 suite to
// `FStandaloneTestHandle`. A grep for UE/Unreal types in this header must be
// empty.
//
// NAMESPACE NOTE: declared in the GLOBAL namespace, matching the rest of the
// OGSim core (see the same note on ConnectionTierTable.h / ServerInputDelayQueue.h
// / ConnectionSlotKey.h). The design corpus writes `ogsim::` but no such
// namespace exists in this tree.
// ---------------------------------------------------------------------------

// Reap deadline expressed as a multiple of the dwell period. Migrated from the
// former UE file-local constant (SimulationManagerUImpl.cpp): a connection whose
// last sample is older than `tierMinDwellTicks * kTierReapDeadlineDwellPeriods`
// is considered gone even if its handle never reported dead (half-open socket).
inline constexpr int32_t kTierReapDeadlineDwellPeriods = 8;

// ---------------------------------------------------------------------------
// ConnectionTierSink — the SEND boundary for the server->owning-client tier
// publish. (og-netcode-v2-arch-latency / T23.)
//
// The tier SEND used to be a two-step the adapter could silently half-complete:
// derive the tier here, then separately call a UE publish method. Nothing forced
// the second call, and the no-reading / dedup POLICY leaked UE-side. Inverting it
// so `noteRttSample` FIRES the send itself (below) makes "derived but never sent"
// structurally impossible, but the send target must stay engine-agnostic — the
// core cannot name a UE type. So the sink is a compile-time CONCEPT: any type that
// can transport a (id, tier) to the owning client satisfies it. UE binds it to
// `USimmableUpdateComponent` (owner-only replicated uint8); a second engine binds
// its own owner-only RPC / replicated var. A sink missing the method is a compile
// error at the `noteRttSample` call site, exactly like the buffer-owner concepts
// in SimulationNetSync.h. `id` identifies the target entity so a sink that is a
// central manager (rather than the entity itself) can route on it.
template <typename T>
concept ConnectionTierSink = requires(T& t, unsigned int id, uint8_t tier) {
    { t.sendConnectionTierToOwningClient(id, tier) } -> std::same_as<void>;
};

// ---------------------------------------------------------------------------
// RemoteInputDeliverySink — the DELIVERY boundary for a received remote input.
// (og-netcode-v2-arch-latency / T24, symmetric with T23's ConnectionTierSink.)
//
// The per-slot receive loop used to live UE-side on ServerReceiveRemoteMove: it
// decoded the bundle, parked each slot, and — when a slot could not be parked —
// fell back to legacy direct delivery. That loop is engine-agnostic policy, so
// it moves into `receiveInputBundle` below. But the DELIVERY itself must stay
// engine-bound (a UE component's RemoteMoveQueue, a Godot node's equivalent), so
// like the tier send it is expressed as a compile-time CONCEPT. Any type able to
// route a (id, captureTick, input) to the owning entity satisfies it. The core
// invokes it on the ONE non-parked path (a malformed slot the delay queue
// refused), and the SAME sink type is the one the drain's UE-side delivery routes
// through (ASimulationManagerUImpl::deliverRemoteInput) — unifying the two
// delivery paths onto a single method. A sink missing the method is a compile
// error at the `receiveInputBundle` call site.
//
// TWO-PARAMETER (T, InputT), unlike ConnectionTierSink: the delivered payload is
// typed, and the core is variadic over the simulatable pack, so the input type is
// supplied at the call site as `InputFor<SimT>` rather than baked into the
// concept. `id` identifies the target entity so a central-manager sink (rather
// than the entity itself) can route on it — which is exactly how the UE binding
// works (the manager owns the id->component map the drain also resolves against).
template <typename T, typename InputT>
concept RemoteInputDeliverySink =
    requires(T& t, unsigned int id, uint32_t captureTick, const InputT& in) {
        { t.deliverRemoteInput(id, captureTick, in) } -> std::same_as<void>;
    };

// Outcome of a single `receiveRemoteInput` call.
//
//   parked      — TRUE when the input was accepted into the delay queue. FALSE
//                 means the ADAPTER must fall back to legacy direct delivery so
//                 no player input is ever silently dropped (the malformed-slot
//                 fence is the only in-core false path; the adapter early-outs on
//                 no-wire before ever calling in).
//   acceptedNew — TRUE when this (id, captureTick) is the first time the
//                 coordinator has seen that capture tick for that id; FALSE for a
//                 redundancy-bundle re-send of an already-seen tick. This is a
//                 pure SIGNAL surfaced for the future input-relay write (so its
//                 relay becomes one line and does not re-import the duplicate
//                 hazard) — it gates NOTHING in this task; parking/delivery are
//                 unchanged by it. (fable review B1 residual.)
struct ReceiveRemoteInputResult
{
    bool parked      = false;
    bool acceptedNew = false;
};

template <ConnectionAddress Address, SimulatableWithInput... SimulatableTs>
class ServerReceptionCoordinator
{
public:
    using TierTable   = ConnectionTierTable<Address>;
    using DelayQueue  = ServerInputDelayQueue<Address, SimulatableTs...>;
    using SlotKey     = ConnectionSlotKey<Address>;

    template <typename SimT>
    using InputFor = SimulatableInputOf_t<SimT>;

    // Borrows `const TimeConfig&` — never owned; the caller (the UE manager's
    // core SimulationManager) must outlive this coordinator. The tier table and
    // delay queue are constructed here in the load-bearing order: table first,
    // queue second (the queue binds the table by reference). Reverse teardown is
    // automatic from member-declaration order.
    explicit ServerReceptionCoordinator(const TimeConfig& cfg)
        : m_config(cfg)
        , m_tierTable(cfg)
        , m_inputDelayQueue(cfg, m_tierTable)
    {
    }

    ServerReceptionCoordinator(const ServerReceptionCoordinator&)            = delete;
    ServerReceptionCoordinator& operator=(const ServerReceptionCoordinator&) = delete;

    // Optional structured-log sink; routes the one-shot malformed-slot warning.
    // Prefix "[Warning]" is honoured by the UE logger route (see SimulationLog.h).
    void setLogger(std::function<void(const char*)> logger) { m_logger = std::move(logger); }

    // -----------------------------------------------------------------------
    // (1) noteRttSample — ONCE PER BUNDLE. (fable review B1'.)
    //
    // A bundle is one datagram and therefore one arrival event; FNetPing's
    // RoundTrip value only advances on ack receipt anyway. Sampling per SLOT
    // would feed the same reading into the tier EMA up to kMaxSlots times and
    // couple the effective smoothing rate to redundancy depth — the exact bug the
    // former per-bundle sample site guarded against.
    //
    // `rttMs` is `double` — narrowing to int32 would shift every EMA update
    // sub-millisecond (ConnectionTierTable feeds `onRttSample` a `double`), a
    // silent tier-behaviour change. The value is the engine's RAW RoundTrip in
    // milliseconds; the tier table's own EMA is the only smoothing (single-
    // smoothing, Option A).
    //
    // FIRES THE SEND ITSELF — returns nothing (T23). The two policies that used to
    // live UE-side move INTO the core here:
    //   * the rttMs < 0 "no reading yet" skip — a negative reading is not sampled
    //     and publishes nothing, exactly as the old adapter sentinel did; and
    //   * publish-only-on-change — the sink is invoked ONLY when the tier the
    //     owning client should now hold differs from what it was last told.
    //
    // WHY THE DEDUP IS KEYED ON ownerId, NOT Address (lead spec correction
    // 2026-07-23). Two couch-coop characters can share ONE root connection (one
    // Address) while each owns its OWN owner-only replicated tier property. A
    // per-Address last-published would publish a transition to the first sibling
    // and skip the other forever. Keying on ownerId — the SINK TARGET — faithfully
    // mirrors the old per-component dedup: both siblings converge. It is also
    // symmetric with the per-id captureTick watermark, and cleared in the same
    // place (forgetOwner). A missing entry means "never told", whose baseline is
    // the replicated property's default of 0 — so a first sample that derives tier
    // 0 correctly publishes nothing.
    //
    // WHY WE COMPARE result.newTierIndex, NOT the transition delta. onRttSample
    // reports the wire's CURRENT tier on every call, and the transition it signals
    // belongs to whichever owner's sample happened to cross the dwell gate. A
    // sibling that did not trigger the transition still sees `newTierIndex` = the
    // new tier and, having last been told 0, must be published to. Gating on
    // "did THIS sample transition" would starve that sibling — the couch-coop bug
    // above, restated at the publish decision.
    //
    // `rttMs` is `double`; see the once-per-bundle + single-smoothing notes above.
    template <ConnectionTierSink Sink>
    void noteRttSample(const Address& addr, unsigned int ownerId, int32_t serverTick,
                       double rttMs, Sink&& sink)
    {
        if (rttMs < 0.0)
        {
            return;         // engine "no reading yet" sentinel — do not sample it
        }
        const TierSampleResult result = m_tierTable.onRttSample(addr, serverTick, rttMs);
        const int32_t newTier = result.newTierIndex;

        const auto it = m_lastPublishedTier.find(ownerId);
        const int32_t lastPublished = (it == m_lastPublishedTier.end()) ? 0 : it->second;
        if (newTier == lastPublished)
        {
            return;         // unchanged for this owner — do not dirty the wire
        }

        if (it == m_lastPublishedTier.end())
        {
            m_lastPublishedTier.emplace(ownerId, newTier);
        }
        else
        {
            it->second = newTier;
        }

        sink.sendConnectionTierToOwningClient(ownerId, static_cast<uint8_t>(newTier));
    }

    // -----------------------------------------------------------------------
    // (2) receiveRemoteInput — PER SLOT.
    //
    // `playerSlot` is a PARAMETER (fable B4'): it is an engine primitive (the UE
    // child-connection id via GetPlayerSlotForActor), exactly like Address and
    // RTT, and the core cannot derive it. Parks `input` under (addr, playerSlot)
    // to be released on `captureTick + effectiveDelay`. Returns the fallback
    // decision + the dedup signal (see ReceiveRemoteInputResult).
    //
    // The malformed-slot fence is the only in-core false path: a slot outside the
    // uint8 substitution-mask range means a malformed topology, not a supported
    // configuration. It is warned ONCE per (id, slot) — never per tick — because
    // the un-throttled UE predecessor produced 28,192 lines / 6.4 MB in 94 s of a
    // single PIE session. The input still takes the legacy undelayed path.
    template <typename SimT>
    ReceiveRemoteInputResult receiveRemoteInput(unsigned int id,
                                                const Address& addr,
                                                uint8_t playerSlot,
                                                int32_t captureTick,
                                                const InputFor<SimT>& input)
    {
        // Dedup watermark: a pure signal for the future relay write, computed and
        // updated BEFORE the parked/fallback split so it covers both paths.
        // Reaped in forgetOwner, so it is bounded by live component ids.
        const bool acceptedNew = noteCaptureTick(id, captureTick);

        const SlotKey key(addr, playerSlot);

        if (!key.hasValidSlot())
        {
            warnOutOfRangeSlotOnce(id, playerSlot);
            return ReceiveRemoteInputResult{ /*parked=*/false, acceptedNew };
        }

        // Delivery routing: which owner id this slot's released input goes to. A
        // plain overwrite — re-registering the same id is a no-op, and an id
        // legitimately replacing a dead one on the same slot (respawn / seamless
        // travel) takes the slot over. Two characters on one machine are two
        // distinct keys, so there is nothing to conflate.
        m_delayedInputTargets[key] = id;

        m_inputDelayQueue.template enqueue<SimT>(key, captureTick, input);

        // [Park] (Log severity — on-demand under LogOGNet Verbose). The full
        // per-tick input timeline for deep dives; its companion is [Release] at
        // the drain. Gated on `acceptedNew` so a redundancy bundle re-sending an
        // already-seen capture tick (the common case) does not spam the timeline —
        // a genuinely new park is logged exactly once. No [Warning] prefix, so it
        // stays at Log severity and is hidden under the default LogOGNet=Warning.
        if (acceptedNew)
        {
            const int32_t delay = m_inputDelayQueue.effectiveDelay(key);
            SIMLOG(m_logger, "[Park] id=%u captureTick=%d delay=%d releaseTick=%d",
                id, captureTick, delay, captureTick + delay);
        }

        return ReceiveRemoteInputResult{ /*parked=*/true, acceptedNew };
    }

    // -----------------------------------------------------------------------
    // (2b) receiveInputBundle — the whole PER-BUNDLE receive loop. (T24.)
    //
    // Relocated verbatim in intent from the UE ServerReceiveRemoteMove per-slot
    // loop: decode the wire bundle with the (already-core) redundancy codec, park
    // each slot via receiveRemoteInput, and on the ONE non-parked path (a malformed
    // slot the delay queue refused) deliver immediately through `deliver` — the
    // SAME RemoteInputDeliverySink the drain's UE delivery routes through, so the
    // park-then-drain path and the deliver-now fallback share one delivery method.
    //
    // `wire` is any type satisfying the codec's Buffer concept (bundleByteNum /
    // readFromBuffer / ...); in production it is the FInputRedundancyBundle USTRUCT
    // itself (which IS a Buffer), in the Catch2 suite a std::vector-backed test
    // buffer — the core never names either. The bundle was written for one input
    // type, so it is decoded as `InputFor<SimT>`, matching how it was appended.
    //
    // The NO-WIRE early-out stays ADAPTER-side (fable ruling): the adapter only
    // reaches here when it has a live wire + coordinator, so the core handles only
    // `hasValidSlot`. The once-per-bundle RTT sample (noteRttSample) is the
    // adapter's responsibility BEFORE this call — this method is purely the slot
    // loop. The genericized `[ServerReceive] id tick` trace replaces the former
    // brawler-specific attackLeft log (the core is templated over SimT and cannot
    // read a game-specific input field); it routes through the coordinator logger.
    template <typename SimT, typename Buffer, typename Sink>
        requires RemoteInputDeliverySink<Sink, InputFor<SimT>>
    void receiveInputBundle(unsigned int id, const Address& addr, uint8_t playerSlot,
                            const Buffer& wire, Sink&& deliver)
    {
        inputRedundancyBundle::forEachSlot<InputFor<SimT>>(
            wire,
            [&](std::uint32_t captureTick, const InputFor<SimT>& input)
            {
                SIMLOG(m_logger, "[ServerReceive] id=%u tick=%u",
                    id, static_cast<unsigned int>(captureTick));

                if (!receiveRemoteInput<SimT>(id, addr, playerSlot,
                        static_cast<int32_t>(captureTick), input).parked)
                {
                    // Malformed slot — the delay queue refused it (already warned
                    // once by receiveRemoteInput). Deliver undelayed so no player
                    // input is ever silently dropped, through the same sink the
                    // drain uses.
                    deliver.deliverRemoteInput(id, captureTick, input);
                }
            });
    }

    // -----------------------------------------------------------------------
    // (3) releaseDelayedInputs — THE DRAIN. (fable review B2'; relocated from
    // ASimulationManagerUImpl::releaseDelayedInputsForStep.)
    //
    // Drains every claimed slot's due input for the `numSteps` sim ticks the
    // upcoming physics step(s) will simulate, and delivers each through the
    // per-id `deliver` callback — NOT a TWeakObjectPtr. `firstUpcomingSimTick`
    // (the ChaosTickMapper `+1` derivation) and `numSteps` are supplied by the
    // adapter, which owns the game-thread-safe tick source; the core never reads
    // a physics-thread clock.
    //
    // `deliver(id, captureTick, input) -> bool`. Returning FALSE means the owner
    // is gone (component GC'd without an unregister); the claim entry is then
    // dropped, mirroring the old drain's `target.Get()==nullptr` prune. The
    // delivered capture tick is the entry's STORED captureTick, surfaced by
    // `tryDequeueForTick` (F1) — NOT a reconstructed `simTick - delay`. Under the
    // T26 due-or-overdue release an overdue entry is released a tick or more late,
    // so `simTick - delay` would name a FUTURE input's tick and collide with
    // RemoteMoveQueue's capture-tick dedup; delivering the true stored tick keeps
    // that dedup and the [InputGap] watermark meaningful — the delay is expressed
    // purely as WHEN this fires, and any lateness is reported as `late=N`.
    template <typename SimT, typename DeliverFn>
    void releaseDelayedInputs(int32_t firstUpcomingSimTick, int32_t numSteps, DeliverFn&& deliver)
    {
        if (m_delayedInputTargets.empty())
        {
            return;
        }

        // The rollback-window lower bound, shared by BOTH the release gate (F3,
        // passed into tryDequeueForTick) and the purge below. Using the one value
        // for both is what keeps the purge the SINGLE drop point: tryDequeueForTick
        // never releases an entry the purge would reclaim, and the purge reclaims
        // exactly what the release gate skipped. May be non-positive early in a
        // session (before tick `rollbackWindowHardCap`), where it harmlessly gates
        // nothing — captureTicks are non-negative — and the purge below is skipped.
        const int32_t staleBefore = firstUpcomingSimTick - m_config.rollbackWindowHardCap;

        for (auto it = m_delayedInputTargets.begin(); it != m_delayedInputTargets.end();)
        {
            const SlotKey&     key = it->first;
            const unsigned int id  = it->second;

            // Read the delay ONCE per slot per drain. Every slot on a wire shares
            // that wire's tier by design, so this resolves through the Address
            // half. It is used only for the [DelayShift] memo and the `late=N`
            // lateness measurement; the delivered captureTick comes from the queue
            // (F1), so this side no longer reconstructs it from the release tick.
            const int32_t delay = m_inputDelayQueue.effectiveDelay(key);

            // [DelayShift] (Warning) — correlate drops with tier moves. Keyed on
            // the WIRE (Address): every slot on a wire shares its tier, so this
            // fires at most once per wire per drain, not once per slot.
            noteDelayShift(key.address, delay, firstUpcomingSimTick);

            bool ownerAlive = true;
            for (int32_t s = 0; s < numSteps; ++s)
            {
                const int32_t simTick = firstUpcomingSimTick + s;

                InputFor<SimT> released{};
                int32_t deliveredCaptureTick = 0;
                if (!m_inputDelayQueue.template tryDequeueForTick<SimT>(
                        key, simTick, released, staleBefore, &deliveredCaptureTick))
                {
                    continue;   // nothing due-and-in-window for this slot on this tick
                }

                // F3 lateness: 0 in the in-time steady state; >0 when this input is
                // released after its exact due tick. Logged so the next PIE run
                // MEASURES the plateau instead of assuming it.
                const int32_t late = simTick - (deliveredCaptureTick + delay);

                if (!deliver(id, static_cast<uint32_t>(deliveredCaptureTick), released))
                {
                    ownerAlive = false;     // owner gone — drop the claim below
                    break;
                }

                // [Release] (Log — the per-tick timeline companion to [Park]) then
                // [InputGap] (Warning) + window-stats accounting: the drain deliver
                // point observes every released capture tick per id, so it is the
                // natural home for both the timeline trace and the gap watermark.
                SIMLOG(m_logger, "[Release] id=%u captureTick=%d releaseTick=%d late=%d",
                    id, deliveredCaptureTick, simTick, late);
                noteDeliveredForGap(id, deliveredCaptureTick);
            }

            it = ownerAlive ? std::next(it) : m_delayedInputTargets.erase(it);
        }

        // Reclaim input parked but never released — beyond the rollback window (the
        // release gate above left it for the purge, so this is the ONE drop point).
        // The window is the span over which a capture tick can still legitimately
        // matter. Same `staleBefore` as the release gate, by construction.
        if (staleBefore > 0)
        {
            // [InputDrop] (Warning) — name the strand. purgeOlderThan now RETURNS
            // the reclaimed entries instead of erasing them silently; each one is
            // a remote input that was parked but never released. The window drop
            // counter is deliberately NOT bumped here: [InputGap] is the single,
            // cause-agnostic source of truth for the [InputStats] aggregate (a
            // stranded-then-purged tick generally re-surfaces as a gap at the next
            // delivery, so counting it here too would double-count). The per-line
            // [InputDrop] is the precise, attributable record.
            const std::vector<typename DelayQueue::PurgedEntry> dropped =
                m_inputDelayQueue.template purgeOlderThan<SimT>(staleBefore);
            for (const typename DelayQueue::PurgedEntry& d : dropped)
            {
                const int32_t dropDelay = m_inputDelayQueue.effectiveDelay(d.slotKey);
                SIMLOG(m_logger,
                    "[Warning][InputDrop] slot=%u captureTick=%d never released; delay=%d staleBefore=%d",
                    static_cast<unsigned int>(d.slotKey.playerSlot), d.captureTick,
                    dropDelay, staleBefore);
            }
        }
    }

    // -----------------------------------------------------------------------
    // (4) reapConnections — evict dropped WIRES. (Relocated from the arrival-
    // gated reap block in sampleAndDeriveConnectionTier.)
    //
    // CADENCE CHANGE, documented and benign (fable concern). The old reap fired
    // only when a bundle arrived on a dwell-boundary tick, so an idle server
    // NEVER reaped — a latent leak. This runs on EVERY dwell-boundary tick
    // regardless of traffic (the adapter calls it once per tick). Strictly more
    // reaps; the internal `serverTick % dwell` gate keeps the frequency the same
    // as before on a busy server. T22 watches it.
    //
    // The production-relevant branch is the DEAD one (`!isAlive()`): the engine
    // GCs the connection and the Address liveness goes stale IN PLACE under an
    // unchanged key. The Catch2 suite cannot model that in-place transition (T4's
    // finding), so that path is proven in the PIE smoke test, not by unit tests.
    void reapConnections(int32_t serverTick)
    {
        // [InputStats] (Warning) — the periodic drop-rate summary. Driven from
        // this once-per-tick hook and evaluated BEFORE the dwell gate below, so
        // the ~2s window is measured purely in SERVER-TICK time (no wall-clock —
        // the core forbids Date/chrono in this path; the window is derived from
        // TimeConfig::tickFrequency).
        maybeEmitInputStats(serverTick);

        const int32_t dwell = m_config.tierMinDwellTicks;
        if (dwell <= 0 || (serverTick % dwell) != 0)
        {
            return;
        }

        const int32_t deadline = dwell * kTierReapDeadlineDwellPeriods;

        m_tierTable.reapDeadHandles(serverTick, deadline);
        m_inputDelayQueue.reapDeadHandles(serverTick, deadline);

        // Claim map: liveness is read off the Address (wire) half only — a dead
        // wire drops every one of its slots, matching the queue's reap. Component
        // (owner) death is NOT read here; it rides the unregister contract
        // (forgetOwner) and the deliver callback, per the id-keyed design.
        for (auto it = m_delayedInputTargets.begin(); it != m_delayedInputTargets.end();)
        {
            it = (!it->first.address.isAlive())
                ? m_delayedInputTargets.erase(it)
                : std::next(it);
        }

        // [DelayShift] memo (T25): prune dead wires alongside the claim map, off
        // the same Address liveness. Keyed on Address, so a wire that goes away
        // does not keep its last-delay entry alive forever.
        for (auto it = m_lastDrainDelay.begin(); it != m_lastDrainDelay.end();)
        {
            it = (!it->first.isAlive()) ? m_lastDrainDelay.erase(it) : std::next(it);
        }

        // m_loggedOutOfRangeSlots is deliberately NOT pruned here: clearing it on
        // the reap cadence would let the warning re-fire every dwell period for a
        // persistently malformed topology. Truly one-shot is the intent.
    }

    // -----------------------------------------------------------------------
    // Lifecycle: forget an owner id. (The unregister-contract half of the id-keyed
    // claim map — replaces the TWeakObjectPtr GC liveness the core cannot hold.)
    // Called from the adapter's unregister path so a component's claim + dedup
    // watermark are dropped promptly on unregister, rather than waiting for GC to
    // make an engine handle stale. Any input still parked under the connection is
    // reclaimed by purgeOlderThan / reapConnections on the usual cadence.
    void forgetOwner(unsigned int id)
    {
        for (auto it = m_delayedInputTargets.begin(); it != m_delayedInputTargets.end();)
        {
            it = (it->second == id) ? m_delayedInputTargets.erase(it) : std::next(it);
        }
        m_captureTickWatermark.erase(id);
        m_lastPublishedTier.erase(id);
        m_lastDeliveredCaptureTick.erase(id);   // [InputGap] watermark (T25)
    }

    // -----------------------------------------------------------------------
    // Introspection — read-only, for the adapter and the Catch2 suite.
    const TierTable&  tierTable() const  { return m_tierTable; }
    const DelayQueue& delayQueue() const { return m_inputDelayQueue; }

    int32_t lookupTierIndex(const Address& addr) const { return m_tierTable.lookupTierIndex(addr); }

    std::size_t claimCount() const { return m_delayedInputTargets.size(); }

    bool hasClaim(const SlotKey& key) const
    {
        return m_delayedInputTargets.find(key) != m_delayedInputTargets.end();
    }

private:
    // First-seen watermark per owner id. `captureTick > seen` => newly accepted.
    // Missing id => first sample, accepted. Monotonic max; NOT a gate on delivery.
    bool noteCaptureTick(unsigned int id, int32_t captureTick)
    {
        const auto it = m_captureTickWatermark.find(id);
        if (it == m_captureTickWatermark.end())
        {
            m_captureTickWatermark.emplace(id, captureTick);
            return true;
        }
        const bool acceptedNew = captureTick > it->second;
        if (acceptedNew)
        {
            it->second = captureTick;
        }
        return acceptedNew;
    }

    // [InputGap] (Warning) — the primary, cause-agnostic drop signal. (T25.)
    // Called at the drain deliver point with the ORIGINAL capture tick. It tracks
    // the last-delivered capture tick per owner id; when the next delivered tick
    // jumps by more than 1, a hole opened in that id's input stream from SOME
    // cause (strand, wire loss, dedup) and it is logged with the count of missing
    // ticks. Also feeds the [InputStats] window: every delivery bumps the total,
    // every gap the drop count. The watermark is reset in forgetOwner and is
    // therefore bounded by live owner ids.
    void noteDeliveredForGap(unsigned int id, int32_t deliveredCaptureTick)
    {
        ++m_windowDelivered;

        const auto it = m_lastDeliveredCaptureTick.find(id);
        if (it == m_lastDeliveredCaptureTick.end())
        {
            m_lastDeliveredCaptureTick.emplace(id, deliveredCaptureTick);
            return;         // first delivery for this id — no baseline to gap from
        }

        const int32_t last = it->second;
        if (deliveredCaptureTick > last + 1)
        {
            const int32_t dropped = deliveredCaptureTick - last - 1;
            SIMLOG(m_logger, "[Warning][InputGap] id=%u lastDelivered=%d now=%d dropped=%d",
                id, last, deliveredCaptureTick, dropped);
            m_windowDropped += dropped;
        }
        // A monotonic max — a late/reordered older tick neither logs a gap nor
        // rewinds the watermark.
        if (deliveredCaptureTick > last)
        {
            it->second = deliveredCaptureTick;
        }
    }

    // [DelayShift] (Warning) — correlate drops with tier moves. (T25.) Keyed on
    // the WIRE: a first observation records the delay and logs nothing; a later
    // drain that sees a different effective delay for that wire logs the shift.
    // Address is opaque, so the log identifies the wire by its stable hash.
    void noteDelayShift(const Address& wire, int32_t delay, int32_t tick)
    {
        const auto it = m_lastDrainDelay.find(wire);
        if (it == m_lastDrainDelay.end())
        {
            m_lastDrainDelay.emplace(wire, delay);
            return;
        }
        if (it->second != delay)
        {
            SIMLOG(m_logger, "[Warning][DelayShift] addr=%zu delay %d->%d tick=%d",
                static_cast<std::size_t>(std::hash<Address>{}(wire)),
                it->second, delay, tick);
            it->second = delay;
        }
    }

    // [InputStats] (Warning) — periodic drop-rate summary. (T25.) Called once per
    // tick from reapConnections; emits at most one line per ~2s window of SERVER
    // TICK time (window length derived from TimeConfig::tickFrequency — no
    // wall-clock). Emits only when the window actually carried remote input, so an
    // idle server does not heartbeat a Warning line every 2s. Counters reset each
    // window.
    void maybeEmitInputStats(int32_t serverTick)
    {
        const int32_t window = static_cast<int32_t>(2.0 * m_config.tickFrequency);
        if (window <= 0)
        {
            return;         // degenerate config — no meaningful window
        }

        if (!m_statsWindowStarted)
        {
            m_statsWindowStarted  = true;
            m_statsWindowStartTick = serverTick;
            return;
        }
        if (serverTick - m_statsWindowStartTick < window)
        {
            return;         // window still open
        }

        const int32_t total = m_windowDelivered + m_windowDropped;
        if (total > 0)
        {
            const int32_t pct = (m_windowDropped * 100) / total;
            SIMLOG(m_logger, "[Warning][InputStats] dropped %d / %d remote inputs = %d%%",
                m_windowDropped, total, pct);
        }

        m_windowDelivered      = 0;
        m_windowDropped        = 0;
        m_statsWindowStartTick = serverTick;
    }

    // One-shot (id, slot) warning. Keyed on a packed 64-bit (id<<8 | slot) so a
    // component is warned about a given offending slot exactly once ever.
    void warnOutOfRangeSlotOnce(unsigned int id, uint8_t playerSlot)
    {
        const uint64_t logKey = (static_cast<uint64_t>(id) << 8) | static_cast<uint64_t>(playerSlot);
        if (m_loggedOutOfRangeSlots.find(logKey) != m_loggedOutOfRangeSlots.end())
        {
            return;
        }
        m_loggedOutOfRangeSlots.insert(logKey);
        SIMLOG(m_logger,
            "[Warning][InputDelay] id=%u derived player slot %u exceeds the supported "
            "maximum of %u -- falling back to undelayed delivery for this owner. This "
            "indicates a malformed connection topology; see ConnectionSlotKey.h. "
            "(Logged once per owner/slot pair.)",
            id, static_cast<unsigned int>(playerSlot),
            static_cast<unsigned int>(SlotKey::kMaxPlayerSlot));
    }

    const TimeConfig& m_config;

    // Declaration order is load-bearing: table before queue (queue binds the
    // table by reference), destroyed in reverse.
    TierTable  m_tierTable;
    DelayQueue m_inputDelayQueue;

    // Delivery routing table + the set the drain iterates. id-keyed (B2').
    std::unordered_map<SlotKey, unsigned int> m_delayedInputTargets;

    // Dedup watermark, per owner id. Bounded by live ids (reaped in forgetOwner).
    std::unordered_map<unsigned int, int32_t> m_captureTickWatermark;

    // Last tier PUBLISHED to each owner id (the publish-only-on-change dedup, T23).
    // Keyed on ownerId (the sink target), not Address, so couch-coop siblings on
    // one wire each converge. A missing entry => never published => baseline tier 0
    // (the replicated property default). Bounded by live ids (reaped in forgetOwner).
    std::unordered_map<unsigned int, int32_t> m_lastPublishedTier;

    // One-shot malformed-slot memo, keyed on packed (id<<8 | slot). Never pruned.
    std::unordered_set<uint64_t> m_loggedOutOfRangeSlots;

    // --- Input-path diagnostics (T25) --------------------------------------
    // [InputGap] per-owner last-delivered capture-tick watermark. id-keyed; reset
    // in forgetOwner, so bounded by live owner ids.
    std::unordered_map<unsigned int, int32_t> m_lastDeliveredCaptureTick;

    // [DelayShift] per-wire last-seen effective delay. Address-keyed; pruned for
    // dead wires in reapConnections.
    std::unordered_map<Address, int32_t> m_lastDrainDelay;

    // [InputStats] server-tick window accounting. No wall-clock: the window is a
    // span of server ticks (see maybeEmitInputStats).
    bool    m_statsWindowStarted   = false;
    int32_t m_statsWindowStartTick = 0;
    int32_t m_windowDelivered      = 0;
    int32_t m_windowDropped        = 0;

    std::function<void(const char*)> m_logger;
};
