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

// ===========================================================================
// ORIENTATION - WHAT THIS TYPE OWNS, WHO CALLS IT, AND ON WHICH THREAD
//
// Read this first. Every fence below states one rule at the declaration it
// guards; none of them restates this map, and this map states no rule.
//
// The narrative, the derivations, the rejected alternatives and the record of
// what was found false here are in
// `docs/ServerReceptionCoordinator-rationale.md`; `§N` points into it.
//
//   * WHAT IT IS. The engine-agnostic OWNER and ORCHESTRATOR of the server's
//     per-connection reception state. It owns a `ConnectionTierTable` and a
//     `ServerInputDelayQueue`, and it turns RTT samples into tiers, parks
//     inbound input, releases it when due, and reaps dropped wires. §1
//
//   * ⛔ AUTHORITY-ONLY AND GAME-THREAD-ONLY. Nothing owned here has internal
//     synchronization - plain `std::unordered_map`, and `std::deque` inside the
//     queue - so no member may be touched from the physics thread, where
//     `onGameSimulationAuthority` runs. §2
//
//   * THE FOUR ENTRY POINTS, and the cadence each is driven at:
//
//       noteRttSample         per arriving BUNDLE   the RPC receive path
//       receiveInputBundle    per arriving BUNDLE   the same path, after it
//       releaseDelayedInputs  per PHYSICS FRAME     the pre-step drain hook
//       reapConnections       per PHYSICS FRAME     the same hook, after it
//
//     A frame simulates `numSteps` sim ticks, so the last two run per FRAME and
//     not per tick. That is load-bearing for the dwell gate - see
//     `reapConnections`. §5
//
//   * THREE SINKS, all compile-time CONCEPTS, so no declaration here names an
//     engine type: `ConnectionTierSink` (a tier out to the owning client),
//     `RemoteInputDeliverySink` (an input INTO this simulation) and
//     `RemoteInputRelaySink` (the same input OUT to the other clients). §4
//
//   * ⛔ ONE RECEIVED INPUT CAN LEAVE ON THREE PATHS, and confusing the last
//     two either loses player input or defeats the receipt gate:
//
//       parked                        the steady state - released when due
//       !parked, !rejectedOutOfDomain malformed slot: the ADAPTER must deliver
//                                     it undelayed or that input is lost
//       rejectedOutOfDomain           outside the server's tick domain:
//                                     DISCARDED, and it must NOT fall back
//
//   * ⛔ THE CLAIM MAP IS id-KEYED, never an engine object handle. OWNER death
//     rides `forgetOwner` plus the `deliver` callback, never GC liveness; WIRE
//     death rides `reapConnections` off the `Address` half. §3
//
//   * ENGINE-FREE CODE. No declaration here names an engine type and the
//     includes are `OGSimulation/` headers plus the STL. Comments DO name one
//     adapter's bindings, as examples and on purpose. §7
//
//   * NAMESPACE. Global, like the rest of this core. The design corpus writes
//     `ogsim::`; no such namespace exists in this tree.
//
//   * TEN LOG TAGS leave this file, all through `setLogger`. Their severities,
//     their windows and what each is for are tabulated in §8.
// ===========================================================================

// Reap deadline = `tierMinDwellTicks * kTierReapDeadlineDwellPeriods`. §5
// ⛔ FOR THE HALF-OPEN SOCKET: a wire goes quiet without ever going !isAlive(). §5
inline constexpr int32_t kTierReapDeadlineDwellPeriods = 8;

// ---------------------------------------------------------------------------
// ConnectionTierSink - the SEND boundary for the server -> owning-client tier
// publish. Any type transporting a (id, tier); `id` names the TARGET, so a
// central-manager sink rather than the entity itself can route on it. §4
//
// ⛔ `noteRttSample` FIRES THE SEND ITSELF - never re-split derive from publish. §4
// ⛔ A missing method is a compile error at the call site, as in `SimulationNetSync.h`.
// ⚠ One adapter binds it to a per-character component that forwards the tier
// to an owner-relevant relay actor, NOT to a property of its own. §4 §9
template <typename T>
concept ConnectionTierSink = requires(T& t, unsigned int id, uint8_t tier) {
    { t.sendConnectionTierToOwningClient(id, tier) } -> std::same_as<void>;
};

// ---------------------------------------------------------------------------
// RemoteInputDeliverySink - the DELIVERY boundary: a received input routed INTO
// the simulation for the character that sent it. Two type parameters, because
// the core is variadic and the payload type comes from the call site. §4
//
// ⛔ THE CORE INVOKES IT ON ONE PATH ONLY - a malformed slot the queue refused. §4
// ⛔ The drain's adapter-side delivery routes through the SAME sink type. §4
// ⚠ A missing method is a compile error at the `receiveInputBundle` call site.
template <typename T, typename InputT>
concept RemoteInputDeliverySink =
    requires(T& t, unsigned int id, uint32_t captureTick, const InputT& in) {
        { t.deliverRemoteInput(id, captureTick, in) } -> std::same_as<void>;
    };

// ---------------------------------------------------------------------------
// RemoteInputRelaySink - the RELAY boundary: the same input forwarded OUT to the
// OTHER clients, so each peer simulates that character with its real input
// instead of extrapolating. Separate from delivery, so a second engine can bind
// one boundary and not the other. §6
//
// ⛔ `dA` IS A SCHEDULE, NOT A DELAY: the receiver DERIVES `captureTick + dA`. §6
// ⛔ IT IS THE DELAY HELD FOR THAT WIRE AT RECEIPT, never sent as a tick. §6
// ⛔ A PEER CANNOT COMPUTE `dA` ITSELF - the sender's tier is owner-only. §6
// ⚠ `uint8_t` is deliberate: one byte per entry is the whole wire cost, and the
// value is bounded by `relayDelayFloorHardCapTicks`. §6
template <typename T, typename InputT>
concept RemoteInputRelaySink =
    requires(T& t, unsigned int id, uint32_t captureTick, uint8_t dA, const InputT& in) {
        { t.relayRemoteInput(id, captureTick, dA, in) } -> std::same_as<void>;
    };

// The DEFAULT relay sink, so every call site that predates the relay compiles
// and behaves exactly as before. §6
// ⛔ A SINK LACKING `relayRemoteInput` IS A COMPILE ERROR, not a silent no-op. §6
struct NullRemoteInputRelaySink
{
    template <typename InputT>
    void relayRemoteInput(unsigned int, uint32_t, uint8_t, const InputT&) const {}
};

// Outcome of one `receiveRemoteInput` call. §2
//
// ⛔ `parked == false` MEANS THE ADAPTER MUST DELIVER UNDELAYED, or input is lost. §2
// ⛔ EXCEPT `rejectedOutOfDomain`, which must NOT be delivered - check it first. §5
// ⛔ `acceptedNew` IS FIRST-SIGHT of (id, captureTick): false for a redundancy
// re-send AND for a genuinely-new out-of-order-OLDER tick. §6
// ⛔ IT GATES THE RELAY TAP, and gates nothing about parking or delivery. §6
// ⛔ `rejectedOutOfDomain` = DISCARDED: not parked, claimed, relayed or delivered. §5
struct ReceiveRemoteInputResult
{
    bool parked              = false;
    bool acceptedNew         = false;
    bool rejectedOutOfDomain = false;
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

    // ⛔ BORROWS `const TimeConfig&` - the caller must outlive this coordinator. §1
    // ⛔ TABLE BEFORE QUEUE - the queue binds the table by reference. §1
    explicit ServerReceptionCoordinator(const TimeConfig& cfg)
        : m_config(cfg)
        , m_tierTable(cfg)
        , m_inputDelayQueue(cfg, m_tierTable)
    {
    }

    ServerReceptionCoordinator(const ServerReceptionCoordinator&)            = delete;
    ServerReceptionCoordinator& operator=(const ServerReceptionCoordinator&) = delete;

    // Optional structured-log sink, carrying EVERY line this file emits - all
    // eleven `SIMLOG` sites across ten tags, not just the slot warning. §8
    // ⚠ A leading `[Warning]` or `[Verbose]` picks the severity; see `SimulationLog.h`.
    void setLogger(std::function<void(const char*)> logger) { m_logger = std::move(logger); }

    // -----------------------------------------------------------------------
    // noteServerTick - the game-thread server sim-tick reference the receipt
    // gate judges an inbound capture tick against. §5
    //
    // ⛔ EXACTLY ONE FEED, AND IT IS `reapConnections`. §5
    // ⛔ NEVER ALSO FEED THIS FROM `noteRttSample`: its tick is an unsynchronized
    // read of the physics-thread clock, and this gate DISCARDS player input. §5
    // ⛔ ONE FEED ALSO KEEPS IT MONOTONIC ENOUGH - two feeds could jitter it back. §5
    // ⛔ LAST-WRITE-WINS, NOT A MAX: a max sticks forever after a clock restart. §5
    // ⛔ FAIL-OPEN UNTIL ARMED - failing CLOSED unset would discard every input. §5
    // ⛔ `m_serverTickKnown` KEEPS "never armed" DISTINCT FROM "armed at tick 0". §5
    // ⚠ PUBLIC, BUT NO EXTERNAL CALLER TODAY - both arm through `reapConnections`. §5
    // ⚠ Unarmed until the first physics frame, where warm-up ticks are in-domain. §5
    void noteServerTick(int32_t serverTick)
    {
        m_serverTickKnown = true;
        m_serverTick      = serverTick;
    }

    // -----------------------------------------------------------------------
    // (1) noteRttSample - ONCE PER BUNDLE, and it publishes the tier itself. §4
    //
    // ⛔ ONCE PER BUNDLE, NEVER PER SLOT - per-slot would feed one reading into
    // the tier EMA up to `kMaxSlots` times and couple smoothing to depth. §4
    // ⛔ `rttMs` STAYS `double` - narrowing is a silent tier-behaviour change. §4
    // ⛔ A NEGATIVE READING IS THE "no reading yet" SENTINEL: never sampled. §4
    // ⛔ DEDUP KEYED ON ownerId, NOT Address - couch-coop siblings share one wire,
    // and a per-Address key would starve the second sibling forever. §4
    // ⛔ COMPARE `result.newTierIndex`, NEVER "did THIS sample transition" - that
    // starves the sibling whose sample did not cross the dwell gate. §4
    // ⚠ A missing entry means never told, baseline 0 - so tier 0 publishes nothing. §4
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
    // (2) receiveRemoteInput - PER SLOT. Parks `input` under (addr, playerSlot)
    // for release on `captureTick + effectiveDelay`, and returns the fallback
    // decision plus the dedup signal. §2
    //
    // `playerSlot` is a PARAMETER because it is an engine primitive, like Address
    // and RTT, and the core cannot derive it. §2
    //
    // ⛔ THE MALFORMED-SLOT FENCE IS THE ONLY IN-CORE FALSE PATH THAT DELIVERS. §2
    // ⛔ ONCE PER (id, slot), NEVER PER TICK - 28,192 lines / 6.4 MB in 94 s once. §8
    //
    // -----------------------------------------------------------------------
    // THE OUT-OF-DOMAIN RECEIPT GATE. §5
    //
    // ⛔ IT RUNS FIRST - before the watermark, the fence, park/claim and the tap. §5
    // ⛔ THE WINDOW IS `[serverTick - rollbackWindowHardCap, serverTick +
    // hardResyncThresholdTicks]`, both bounds INCLUSIVE, both from TimeConfig. §5
    // ⛔ WHY: a warm-up client emits its OWN counter while the server is at 600+,
    // and without this gate the relay would broadcast that to every peer. §5
    // ⚠ The lower bound is exactly the drain's `staleBefore` - one window. §5
    // ⚠ The upper bound is the failsafe drift threshold; beyond it a snap is due. §5
    // ⛔ `hardResyncThresholdTicks > rollbackWindowHardCap`, so it cannot invert. §5
    // ⛔ `TimeConfigOrderingTest.cpp` asserts that strict inequality. §5
    // ⛔ BEFORE THE WATERMARK, not merely before the park. §5
    // ⛔ `noteCaptureTick` IS A MAX - one garbage future tick suppresses the rest. §5
    // ⛔ A REJECTED INPUT IS DISCARDED, NOT DELIVERED - a fallback would defeat it. §5
    // ⚠ AUDIT - `RemoteMoveQueue::queueMove` (`SimulationQueues.h`) applies a
    // TIGHTER future guard at physics-thread DELIVERY; this one guards RECEIPT. §5
    // ⚠ NEITHER SUBSUMES THE OTHER; on a fallback path both can act. Not silent. §5
    //
    // -----------------------------------------------------------------------
    // THE RELAY TAP. §6
    //
    // ⛔ IT IS A TAP: it reads what this path computed and changes nothing. §6
    // ⛔ AT RECEIPT, NOT AT RELEASE - holding shortens every peer's runway. §6
    // ⚠ We relay the RECEIVED input, not the APPLIED one; the state channel heals. §6
    // ⛔ THE GATE IS `parked && acceptedNew`, AND IT IS STRUCTURAL, not re-tested. §6
    // ⛔ BARE `acceptedNew` IS WRONG: it is computed before the parked/fallback
    // split, so an UNDELAYED malformed slot would be stamped and mis-scheduled. §6
    // ⛔ THERE IS NO RELAY ON ANY FALLBACK PATH. §6
    // ⛔ `parked && !acceptedNew` IS TWO POPULATIONS: a re-send, and an older tick. §6
    // ⛔ THE OLDER TICK IS APPLIED BUT NOT RELAYED - at the shipped depth of 1 the
    // payload is replace-latest, so an older write moves every peer's latest BACK. §6
    // ⚠ The peer sees a HOLE instead, healed by the every-frame state anchor. §6
    // ⚠ The two are told apart by `enqueue`'s return value. §6
    // ⚠ The second is counted as `relayOooSkipCount()`, because a depth>1 future
    // must reopen this gate on a measured rate rather than on argument. §6
    template <typename SimT, typename RelaySink = NullRemoteInputRelaySink>
        requires RemoteInputRelaySink<RelaySink, InputFor<SimT>>
    ReceiveRemoteInputResult receiveRemoteInput(unsigned int id,
                                                const Address& addr,
                                                uint8_t playerSlot,
                                                int32_t captureTick,
                                                const InputFor<SimT>& input,
                                                RelaySink&& relay = RelaySink{})
    {
        if (rejectOutOfDomainCaptureTick(id, captureTick))
        {
            return ReceiveRemoteInputResult{ /*parked=*/false, /*acceptedNew=*/false,
                                             /*rejectedOutOfDomain=*/true };
        }

        // Dedup watermark, computed BEFORE the parked/fallback split so it covers
        // both paths. Reaped in `forgetOwner`, so bounded by live ids. §3
        const bool acceptedNew = noteCaptureTick(id, captureTick);

        const SlotKey key(addr, playerSlot);

        if (!key.hasValidSlot())
        {
            warnOutOfRangeSlotOnce(id, playerSlot);
            return ReceiveRemoteInputResult{ /*parked=*/false, acceptedNew,
                                             /*rejectedOutOfDomain=*/false };
        }

        // ⛔ A PLAIN OVERWRITE IS DELIBERATE - a respawn takes the slot over. §3
        // ⚠ Two characters on one machine are two distinct keys, never conflated. §3
        m_delayedInputTargets[key] = id;

        // TRUE when this capture tick was not already resident in the slot's deque
        // - the discriminator the relay-tap gate (`parked && !acceptedNew`) turns on. §6
        const bool queuedNewEntry =
            m_inputDelayQueue.template enqueue<SimT>(key, captureTick, input);

        // ⛔ [Park] IS GATED ON `acceptedNew`, so a re-send does not spam it. §8
        // ⚠ No prefix, so it stays at Log, hidden under the shipped `LogOGNet=Warning`. §8
        if (acceptedNew)
        {
            const int32_t delay = m_inputDelayQueue.effectiveDelay(key);
            SIMLOG(m_logger, "[Park] id=%u captureTick=%d delay=%d releaseTick=%d",
                id, captureTick, delay, captureTick + delay);

            // ⛔ THE STAMP READS THE SAME `delay` [Park] REPORTS - no drift. §6
            relay.relayRemoteInput(id, static_cast<uint32_t>(captureTick),
                                   stampFromDelay(delay), input);
        }
        else if (queuedNewEntry)
        {

            // Genuinely new but older than this id's watermark: applied, not
            // relayed. Counted and traced - see the relay-tap gate notes. §6
            noteRelayOooSkip(id, captureTick);
        }

        return ReceiveRemoteInputResult{ /*parked=*/true, acceptedNew,
                                         /*rejectedOutOfDomain=*/false };
    }

    // -----------------------------------------------------------------------
    // (2b) receiveInputBundle - the whole per-bundle receive loop: decode the
    // wire with the redundancy codec, park each slot, and deliver the one
    // non-parked slot immediately. §4
    //
    // `wire` is any type satisfying the codec's Buffer concept; the bundle was
    // written for one input type, so it is decoded as `InputFor<SimT>`. §4
    //
    // ⛔ THE NO-WIRE EARLY-OUT STAYS ADAPTER-SIDE; the core does `hasValidSlot`. §4
    // ⛔ THE ONCE-PER-BUNDLE RTT SAMPLE IS THE ADAPTER'S, BEFORE THIS CALL. §4
    // ⛔ `relay` IS THREADED STRAIGHT THROUGH; no relay policy of its own. §6
    // ⚠ ONE object, two sink parameters - they are separate boundaries. §6
    template <typename SimT, typename Buffer, typename Sink,
              typename RelaySink = NullRemoteInputRelaySink>
        requires RemoteInputDeliverySink<Sink, InputFor<SimT>>
              && RemoteInputRelaySink<RelaySink, InputFor<SimT>>
    void receiveInputBundle(unsigned int id, const Address& addr, uint8_t playerSlot,
                            const Buffer& wire, Sink&& deliver,
                            RelaySink&& relay = RelaySink{})
    {
        inputRedundancyBundle::forEachSlot<InputFor<SimT>>(
            wire,
            [&](std::uint32_t captureTick, const InputFor<SimT>& input)
            {
                SIMLOG(m_logger, "[ServerReceive] id=%u tick=%u",
                    id, static_cast<unsigned int>(captureTick));

                const ReceiveRemoteInputResult result = receiveRemoteInput<SimT>(
                    id, addr, playerSlot, static_cast<int32_t>(captureTick), input, relay);

                // ⛔ TWO FALSE-PARKED MEANINGS, AND ONLY ONE DELIVERS. §5
                // ⛔ `rejectedOutOfDomain` IS DROPPED - delivering defeats the gate. §5
                // ⛔ The malformed slot IS delivered undelayed, or that input is lost. §2
                if (!result.parked && !result.rejectedOutOfDomain)
                {
                    deliver.deliverRemoteInput(id, captureTick, input);
                }
            });
    }

    // -----------------------------------------------------------------------
    // (3) releaseDelayedInputs - THE DRAIN. Releases every claimed slot's due
    // input for the `numSteps` sim ticks the upcoming physics step will simulate
    // and hands each to the per-id `deliver` callback. §2
    //
    // ⛔ THE ADAPTER SUPPLIES THE TICK; the core never reads a physics clock. §2
    // ⛔ `deliver` RETURNING FALSE MEANS THE OWNER IS GONE; the claim is dropped. §3
    // ⛔ DELIVER THE STORED captureTick, NEVER `simTick - delay`. §2
    // ⛔ AN OVERDUE ENTRY WOULD OTHERWISE NAME A FUTURE TICK and collide with
    // `RemoteMoveQueue`'s capture-tick dedup. §2
    // ⚠ Lateness is reported separately, as `late=N`. §8
    template <typename SimT, typename DeliverFn>
    void releaseDelayedInputs(int32_t firstUpcomingSimTick, int32_t numSteps, DeliverFn&& deliver)
    {
        if (m_delayedInputTargets.empty())
        {
            return;
        }

        // ⛔ ONE `staleBefore` FOR THE GATE AND THE PURGE - one drop point. §2
        // ⚠ Non-positive early in a session: gates nothing, purge skipped. §2
        const int32_t staleBefore = firstUpcomingSimTick - m_config.rollbackWindowHardCap;

        for (auto it = m_delayedInputTargets.begin(); it != m_delayedInputTargets.end();)
        {
            const SlotKey&     key = it->first;
            const unsigned int id  = it->second;

            // ⛔ READ THE DELAY ONCE PER SLOT PER DRAIN, through the Address half. §4
            // ⚠ Used only for [DelayShift] and `late=N`. §8
            const int32_t delay = m_inputDelayQueue.effectiveDelay(key);

            // [DelayShift] is keyed on the WIRE, so it fires at most once per wire
            // per drain rather than once per slot. §8
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

                // Lateness: 0 in the in-time steady state. Logged so a run MEASURES
                // the plateau instead of assuming it. §8
                const int32_t late = simTick - (deliveredCaptureTick + delay);

                if (!deliver(id, static_cast<uint32_t>(deliveredCaptureTick), released))
                {
                    ownerAlive = false;     // owner gone — drop the claim below
                    break;
                }

                // [Release] is the timeline companion to [Park]; this point sees
                // every released tick per id, so [InputGap] lives here too. §8
                SIMLOG(m_logger, "[Release] id=%u captureTick=%d releaseTick=%d late=%d",
                    id, deliveredCaptureTick, simTick, late);
                noteDeliveredForGap(id, deliveredCaptureTick);
            }

            it = ownerAlive ? std::next(it) : m_delayedInputTargets.erase(it);
        }

        // Reclaim input parked but never released - the release gate left it for
        // this, the ONE drop point. §2
        if (staleBefore > 0)
        {

            // ⛔ THE WINDOW DROP COUNTER IS NOT BUMPED HERE - [InputGap] is the one
            // cause-agnostic source, so bumping both would double-count. §8
            // ⚠ [InputDrop] is the precise attributable per-line record instead. §8
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
    // (4) reapConnections - evict dropped WIRES, and carry the once-per-frame
    // tick reference the receipt gate needs. §5
    //
    // ⚠ CADENCE CHANGE, BENIGN: the predecessor never reaped on an idle server. §5
    // ⛔ ONCE PER PHYSICS FRAME, NOT PER TICK: a frame simulates `numSteps` ticks,
    // so the `serverTick % dwell` gate below can step straight over a boundary. §5
    // ⚠ The reap waits for the next boundary; busy-server frequency is unchanged. §5
    // ⚠ THE PRODUCTION-RELEVANT BRANCH IS THE DEAD ONE. §9
    // ⚠ An IN-PLACE liveness flip is unstageable as a unit case - a handle's
    // `aliveBit` is part of its identity, so flipping it makes a different key. §9
    void reapConnections(int32_t serverTick)
    {

        // ⛔ RECORDED BEFORE THE DWELL GATE, so it refreshes every frame. §5
        noteServerTick(serverTick);

        // ⛔ [InputStats] IS ALSO EVALUATED BEFORE THE DWELL GATE. §8
        // ⛔ ITS WINDOW IS SERVER-TICK TIME - no wall-clock is reachable here. §8
        maybeEmitInputStats(serverTick);

        const int32_t dwell = m_config.tierMinDwellTicks;
        if (dwell <= 0 || (serverTick % dwell) != 0)
        {
            return;
        }

        const int32_t deadline = dwell * kTierReapDeadlineDwellPeriods;

        m_tierTable.reapDeadHandles(serverTick, deadline);
        m_inputDelayQueue.reapDeadHandles(serverTick, deadline);

        // ⛔ CLAIM LIVENESS IS READ OFF THE Address HALF ONLY - one wire, all slots. §3
        for (auto it = m_delayedInputTargets.begin(); it != m_delayedInputTargets.end();)
        {
            it = (!it->first.address.isAlive())
                ? m_delayedInputTargets.erase(it)
                : std::next(it);
        }

        // The [DelayShift] memo is Address-keyed, so it is pruned here off the same
        // wire liveness rather than growing forever. §8
        for (auto it = m_lastDrainDelay.begin(); it != m_lastDrainDelay.end();)
        {
            it = (!it->first.isAlive()) ? m_lastDrainDelay.erase(it) : std::next(it);
        }

        // ⛔ `m_loggedOutOfRangeSlots` IS DELIBERATELY NOT PRUNED HERE. §8
        // ⛔ The reap cadence would re-fire the warning every dwell period. §8
    }

    // -----------------------------------------------------------------------
    // Lifecycle: forget an owner id - the unregister-contract half of the
    // id-keyed claim map. §3
    //
    // ⛔ CALLED FROM THE ADAPTER'S UNREGISTER PATH so claims drop promptly; the
    // core cannot observe GC making an engine handle stale. §3
    // ⚠ Parked input is reclaimed by the purge and the reap, not here. §2
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
    // Introspection - read-only, for the adapter and the Catch2 suite.
    const TierTable&  tierTable() const  { return m_tierTable; }
    const DelayQueue& delayQueue() const { return m_inputDelayQueue; }

    int32_t lookupTierIndex(const Address& addr) const { return m_tierTable.lookupTierIndex(addr); }

    std::size_t claimCount() const { return m_delayedInputTargets.size(); }

    // Capture ticks the receipt gate has refused since construction. §5
    // ⚠ NEVER RESET - the per-window counter behind [InputDomain] is separate. §8
    std::size_t outOfDomainRejectCount() const { return m_outOfDomainRejectTotal; }

    // Capture ticks the server APPLIED but the monotonic relay stream skipped as
    // out-of-order-older. Never reset. §6
    // ⛔ THIS IS THE EVIDENCE THE depth>1 DECISION MUST READ, not argument. §6
    std::size_t relayOooSkipCount() const { return m_relayOooSkipTotal; }

    bool hasClaim(const SlotKey& key) const
    {
        return m_delayedInputTargets.find(key) != m_delayedInputTargets.end();
    }

private:

    // -----------------------------------------------------------------------
    // The schedule stamp, narrowed to its wire type. §6
    // ⛔ SATURATE, NEVER WRAP - a wrap would make peers schedule wildly early. §6
    // ⚠ The clamp cannot fire in a shipped configuration; it exists for the
    // misconfigured one. §6
    static uint8_t stampFromDelay(int32_t delay)
    {
        if (delay <= 0)
        {
            return 0;
        }
        return (delay >= 255) ? uint8_t{ 255 } : static_cast<uint8_t>(delay);
    }

    // [RelaySkip] - one line per out-of-order-older tick the relay stream drops,
    // plus the counters the per-window summary reads. §8
    // ⚠ Verbose per event; the per-window total is a Warning from [InputStats]. §8
    void noteRelayOooSkip(unsigned int id, int32_t captureTick)
    {
        ++m_relayOooSkipTotal;
        ++m_windowRelayOooSkips;

        const auto it = m_captureTickWatermark.find(id);
        const int32_t watermark = (it == m_captureTickWatermark.end()) ? captureTick : it->second;
        SIMLOG(m_logger,
            "[Verbose][RelaySkip] id=%u captureTick=%d watermark=%d out-of-order-older; "
            "applied by the authority, not relayed",
            id, captureTick, watermark);
    }

    // First-seen watermark per owner id: `captureTick > seen` => newly accepted,
    // missing id => first sample. §6
    // ⛔ A MONOTONIC MAX, AND NOT A GATE - which is why the receipt gate precedes. §5
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

    // -----------------------------------------------------------------------
    // The receipt gate's decision and accounting. Returns TRUE when `captureTick`
    // must be REFUSED; the bounds are argued at `receiveRemoteInput`. §5
    //
    // ⛔ BOTH BOUNDS COME FROM TimeConfig - there is deliberately no literal here. §5
    // ⛔ CAST THE UPPER BOUND: `hardResyncThresholdTicks` is `uint32_t`. §5
    // ⛔ An unsigned promotion would wrap for an early or negative `serverTick`. §5
    // ⛔ THE COUNTING IS SPLIT IN TWO: a lifetime total, and a window burst counter
    // draining into ONE [InputDomain] line, so a client cannot log once per tick. §8
    bool rejectOutOfDomainCaptureTick(unsigned int id, int32_t captureTick)
    {
        if (!m_serverTickKnown)
        {
            return false;       // no tick reference yet — fail open, see noteServerTick
        }

        const int32_t lower = m_serverTick - m_config.rollbackWindowHardCap;
        const int32_t upper = m_serverTick + static_cast<int32_t>(m_config.hardResyncThresholdTicks);

        if (captureTick >= lower && captureTick <= upper)
        {
            return false;       // in domain — the accepted path is untouched
        }

        ++m_outOfDomainRejectTotal;
        ++m_windowOutOfDomainRejects;
        m_lastRejectedId          = id;
        m_lastRejectedCaptureTick = captureTick;
        m_lastRejectedServerTick  = m_serverTick;
        return true;
    }

    // [InputGap] - the primary, cause-agnostic drop signal, called at the drain
    // deliver point with the ORIGINAL capture tick. A jump of more than 1 means a
    // hole opened in that id's stream from SOME cause. §8
    // ⚠ It also feeds the [InputStats] window, and is reset in `forgetOwner`. §8
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

        // ⛔ A MONOTONIC MAX: a reordered older tick neither logs nor rewinds. §8
        if (deliveredCaptureTick > last)
        {
            it->second = deliveredCaptureTick;
        }
    }

    // [DelayShift] - correlate drops with tier moves, keyed on the wire: the first
    // observation records silently, a later differing one logs the shift. §8
    // ⚠ `Address` is opaque to the core, so the line names the wire by its hash. §8
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

    // [InputStats] - the periodic drop-rate summary, at most one line per ~2 s
    // window of SERVER-TICK time. §8
    //
    // ⛔ THE WINDOW IS DERIVED FROM `TimeConfig::tickFrequency` - no wall-clock. §8
    // ⚠ It emits only when the window carried input, so an idle server is quiet. §8
    // ⛔ [InputDomain] AND [RelaySkip] RIDE THIS SAME WINDOW - one timer, not three. §8
    // ⚠ Same start tick, same reset point, same emit-only-if-carried rule. §8
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

        // [InputDomain] names the most recent offender and the window it was judged
        // against, so a garbage capture tick is diagnosable from the log alone. §8
        if (m_windowOutOfDomainRejects > 0)
        {
            const int32_t lower = m_lastRejectedServerTick - m_config.rollbackWindowHardCap;
            const int32_t upper = m_lastRejectedServerTick
                                + static_cast<int32_t>(m_config.hardResyncThresholdTicks);
            SIMLOG(m_logger,
                "[Warning][InputDomain] rejected %d out-of-domain capture ticks; "
                "last id=%u captureTick=%d serverTick=%d window=[%d,%d]",
                m_windowOutOfDomainRejects, m_lastRejectedId, m_lastRejectedCaptureTick,
                m_lastRejectedServerTick, lower, upper);
        }

        // ⛔ [RelaySkip] HAS ITS OWN TAG, so the [InputStats] string is unchanged. §8
        if (m_windowRelayOooSkips > 0)
        {
            SIMLOG(m_logger,
                "[Warning][RelaySkip] %d out-of-order-older capture ticks applied but not "
                "relayed this window; lifetime %zu",
                m_windowRelayOooSkips, m_relayOooSkipTotal);
        }

        m_windowDelivered          = 0;
        m_windowDropped            = 0;
        m_windowOutOfDomainRejects = 0;
        m_windowRelayOooSkips      = 0;
        m_statsWindowStartTick     = serverTick;
    }

    // ⛔ ONE-SHOT PER (id, slot), keyed on a packed `(id<<8 | slot)`. §8
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

    // ⛔ DECLARATION ORDER IS LOAD-BEARING: table before queue, teardown reverse. §1
    TierTable  m_tierTable;
    DelayQueue m_inputDelayQueue;

    // Delivery routing table, and the set the drain iterates. id-keyed. §3
    std::unordered_map<SlotKey, unsigned int> m_delayedInputTargets;

    // Dedup watermark per owner id. Bounded by live ids, reaped in `forgetOwner`.
    std::unordered_map<unsigned int, int32_t> m_captureTickWatermark;

    // Last tier PUBLISHED to each owner id. §4
    // ⛔ KEYED ON ownerId, NOT Address, so couch-coop siblings each converge. §4
    // ⚠ A missing entry means never published, whose baseline is tier 0. §4
    std::unordered_map<unsigned int, int32_t> m_lastPublishedTier;

    // One-shot malformed-slot memo, packed (id<<8 | slot). Never pruned. §8
    std::unordered_set<uint64_t> m_loggedOutOfRangeSlots;

    // --- Input-path diagnostics --------------------------------------------
    // [InputGap] per-owner last-delivered capture-tick watermark, reset in
    // `forgetOwner`. §8
    std::unordered_map<unsigned int, int32_t> m_lastDeliveredCaptureTick;

    // [DelayShift] per-wire last-seen effective delay, pruned for dead wires in
    // `reapConnections`. §8
    std::unordered_map<Address, int32_t> m_lastDrainDelay;

    // [InputStats] window accounting, in server ticks - no wall-clock. §8
    bool    m_statsWindowStarted   = false;
    int32_t m_statsWindowStartTick = 0;
    int32_t m_windowDelivered      = 0;
    int32_t m_windowDropped        = 0;

    // --- Out-of-domain receipt gate ----------------------------------------
    // The game-thread server-tick reference, fed ONLY by `reapConnections`. §5
    // ⛔ `m_serverTickKnown` KEEPS "never armed" (fail open) DISTINCT FROM "armed
    // at tick 0" - see `noteServerTick`. §5
    bool    m_serverTickKnown = false;
    int32_t m_serverTick      = 0;

    // Lifetime total plus the per-window burst counter that `maybeEmitInputStats`
    // drains into ONE [InputDomain] line, with the last offender kept for it. §8
    std::size_t m_outOfDomainRejectTotal   = 0;
    int32_t     m_windowOutOfDomainRejects = 0;
    unsigned int m_lastRejectedId          = 0;
    int32_t     m_lastRejectedCaptureTick  = 0;
    int32_t     m_lastRejectedServerTick   = 0;

    // --- Relay tap ----------------------------------------------------------
    // Lifetime total plus the per-window burst counter for out-of-order-older
    // capture ticks the server applied but did not relay. §6
    // ⛔ NOT RESET IN `forgetOwner` - session-scoped, not per-owner state. §6
    std::size_t m_relayOooSkipTotal  = 0;
    int32_t     m_windowRelayOooSkips = 0;

    std::function<void(const char*)> m_logger;
};