#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "OGSimulation/Network/ConnectionSlotKey.h"
#include "OGSimulation/Network/ConnectionTierTable.h"
#include "OGSimulation/PCTimeManagement/TimeConfig.h"

// ---------------------------------------------------------------------------
// ServerInputDelayQueue<Address, SimulatableTs...> — server-side per-connection
// Layer-1 input delay buffer.
// (Stage 3 / D3.5; proposal_ogbrawler_netcode.md §1.2 + §3.3 + Correction 4.)
//
// The server does not consume a client's input on the tick it arrives. It parks
// the input here and hands it to the simulation on the tick
// `captureTick + effectiveDelay(addr)`. That deliberate constant delay is what
// lets a short network hiccup be absorbed without a visible re-simulation pop:
// the input for tick N is already in hand before tick N is simulated.
//
// ---------------------------------------------------------------------------
// TWO KEYS, NOT ONE — and the split is load-bearing. (Corrected T17, 2026-07-20.)
//
//   INPUT storage is keyed on `ConnectionSlotKey` = (Address, playerSlot).
//   TIER / DELAY / LIVENESS are keyed on `Address` alone.
//
// Inputs are produced per CHARACTER; latency is a property of the WIRE. This
// game ships multiple player-controlled characters on one client (shared-camera
// couch co-op — a designed topology, see ConnectionSlotKey.h), so those two
// granularities genuinely differ and conflating them is not a theoretical
// concern: keying the input storage on Address alone made every character on a
// machine share one deque, where the capture-tick dedup below silently ate all
// but the first character's input for any tick they both captured on.
//
// So `effectiveDelay`, `m_lastActivityTick` and `reapDeadHandles` still take and
// hold a bare `Address` — every slot on a wire shares that wire's tier, becomes
// active together, and is reaped together — while `enqueue`/`tryDequeueForTick`/
// `hasReadyForTick`/`pendingCount` take the full slot key. Do not "simplify" the
// two down to one; each is correct for exactly the quantity it carries.
// ---------------------------------------------------------------------------
//
// SINGLE CONTAINER, VARIADIC OVER THE SIM PACK — proposal Correction 4. This is
// ONE object holding a `std::tuple<PerSimQueue<SimulatableTs>...>`, NOT N
// separate per-simulatable queue instances. The distinction is load-bearing
// rather than cosmetic: the per-connection policy (effective delay, dedup rule,
// reaping, liveness) is decided ONCE per connection and applies uniformly across
// every simulatable. With N instances that policy would be duplicated N times
// and could drift — a connection could be reaped from one sim's queue and
// survive in another's, and a tier change could land on some sims and not
// others mid-tick. Here `reapDeadHandles` walks the whole tuple in one pass and
// `effectiveDelay` is computed from one shared tier table, so all simulatables
// see the same connection state by construction.
//
// ENGINE-AGNOSTIC. Sim-core header: includes ONLY other `OGSimulation/` headers
// and the STL — no UE types, no engine headers. Wire identity arrives as the
// opaque `Address` parameter, bound in production to `FUEConnectionHandle` and
// in the Catch2 suite to `FStandaloneTestHandle`.
//
// ---------------------------------------------------------------------------
// THREADING CONTRACT — THIS CLASS IS **NOT** THREAD-SAFE. (Added T10, 2026-07-20.)
//
// It is intended for SINGLE-THREADED use, and in the UE binding that thread is
// the GAME THREAD. There is no synchronization anywhere in here: the storage is
// a plain `std::deque` per address inside a plain `std::unordered_map`, with no
// mutex and no atomics. Calling `enqueue` from one thread while another calls
// `tryDequeueForTick` / `purgeOlderThan` / `reapDeadHandles` is a genuine data
// race — and specifically NOT a benign one. A concurrent `unordered_map` rehash
// during a `find` is undefined behaviour, not a torn read.
//
// This note exists because its absence caused a real integration failure. T10
// was originally specified to enqueue in the input RPC handler (GAME thread) and
// dequeue at the top of the authority tick (`onGameSimulationAuthority`, which
// runs on the PHYSICS thread under `bTickPhysicsAsync=True`). That pairing is
// unimplementable, and the header said nothing to warn the caller off it.
//
// THE GAME-THREAD -> PHYSICS-THREAD TRANSITION IS NOT THIS CLASS'S JOB. It
// belongs to `RemoteMoveQueue` (SimulationQueues.h), which is the seam the
// server input path already crosses. The correct integration shape, and the one
// T10 ships, is:
//
//   game thread:     RPC arrives -> enqueue() here
//   game thread:     immediately before the physics step -> tryDequeueForTick()
//                    here, then hand the released input to RemoteMoveQueue
//   physics thread:  the authority tick consumes it from RemoteMoveQueue
//
// Both ends of THIS queue therefore sit on the game thread, and nothing about
// this container is ever touched from the physics thread. A future caller that
// wants to dequeue from the physics thread must NOT simply add a lock here: a
// lock would fix this container's internal race and still leave the caller doing
// UObject traversal (`TWeakObjectPtr::Get`, `AActor::GetNetConnection`) from the
// physics thread, which this codebase forbids.
// ---------------------------------------------------------------------------
//
// CONSUMER: wired in Phase B (T10) — the UE server binding parks inbound RPC
// input here and releases it on the game thread immediately before the authority
// physics step. The Catch2 suite additionally drives it standalone.
//
// NAMESPACE NOTE: declared in the GLOBAL namespace, matching the rest of the
// OGSim core (see the same note on ConnectionTierTable.h, NetConfig in
// SimulationManagerConcept.h, and SimulatableList.h). The design corpus writes
// `ogsim::` but no such namespace exists in this tree.
// ---------------------------------------------------------------------------

// --- Input-type resolution -------------------------------------------------
//
// The queue needs one thing from a simulatable: the type of the input it
// consumes. Two spellings exist in this tree and BOTH are supported:
//
//   * `InputType` — what every production simulatable actually uses today
//     (SimulatableBrawler.h:23, DAttackGuardSimulation.h:184, and four more).
//   * `Input`     — the spelling the D3.5 task text and its mock simulatables
//     use.
//
// Resolving both is deliberate. Constraining on `Input` alone would compile
// green against the mocks and then fail to instantiate the moment Phase B binds
// the real `SimulatableBrawler`; constraining on `InputType` alone would
// contradict the task's own mock contract. `Input` wins when a type somehow
// declares both, since that is the more specific opt-in.
template <typename T>
struct SimulatableInputOf
{
    using type = typename T::InputType;
};

template <typename T>
    requires requires { typename T::Input; }
struct SimulatableInputOf<T>
{
    using type = typename T::Input;
};

template <typename T>
using SimulatableInputOf_t = typename SimulatableInputOf<T>::type;

// Minimal contract, intentionally. This container stores and returns inputs; it
// never inspects simulation STATE, so requiring the far heavier `SimulatableState`
// / `SimulatableIntegration` contracts here would constrain a parameter on
// properties this class does not use, and would block the trivial mock
// simulatables the D3.5 test suite is specified to run against.
template <typename T>
concept SimulatableWithInput =
    requires { typename SimulatableInputOf_t<T>; } &&
    std::copy_constructible<SimulatableInputOf_t<T>> &&
    std::is_copy_assignable_v<SimulatableInputOf_t<T>>;

template <ConnectionAddress Address, SimulatableWithInput... SimulatableTs>
class ServerInputDelayQueue
{
public:
    using TierTable = ConnectionTierTable<Address>;

    // The INPUT-storage key: (wire, which local player on it). See the TWO KEYS
    // note above for why this is not the same as the tier/liveness key.
    using SlotKey = ConnectionSlotKey<Address>;

    template <typename SimT>
    using InputFor = SimulatableInputOf_t<SimT>;

    // Number of simulatable types this queue serves. Derived from the pack, so
    // it cannot drift from the tuple it describes.
    static constexpr std::size_t kSimTypeCount = sizeof...(SimulatableTs);

    // A single entry discarded by `purgeOlderThan`, surfaced to the caller.
    // (og-netcode-v2-arch-latency / T25.) The purge used to erase stranded input
    // silently; it now REPORTS what it reclaimed so the reception coordinator can
    // name each one on a `[Warning][InputDrop]` line. The queue itself stays
    // logger-free (see the "the queue stays logger-free" contract in
    // ServerReceptionCoordinator.h): it hands back the facts, the coordinator —
    // which owns the logger — describes them. The FULL slot key is returned (not
    // just the playerSlot) so the caller can re-derive the per-wire effective
    // delay for the log line.
    struct PurgedEntry
    {
        SlotKey slotKey;
        int32_t captureTick;
    };

    // Tier-table-less form — Phase-A back-compat and any deployment that has not
    // wired tier escalation. Every connection then gets the flat baseline
    // `TimeConfig::forcedInputLatencyTicks`.
    explicit ServerInputDelayQueue(const TimeConfig& cfg)
        : m_config(cfg)
        , m_tierTable(nullptr)
    {
    }

    // Both references are BORROWED, not owned — one TimeConfig and one tier
    // table are shared by the whole server-side stack, and copies here could
    // silently diverge from the instances the clocks and the escalation policy
    // read. Caller must outlive the queue.
    ServerInputDelayQueue(const TimeConfig& cfg, const TierTable& tierTable)
        : m_config(cfg)
        , m_tierTable(&tierTable)
    {
    }

    // --- Effective delay ---------------------------------------------------
    //
    // LOCKED 2026-07-19 (backlog C2). effectiveDelay(addr) =
    // `tierTable.lookupInputDelayTicks(addr)`, i.e. the per-tier value
    // `rttTierInputDelays[tier]` IS the effective delay and REPLACES the
    // baseline. It is NOT added to `forcedInputLatencyTicks`.
    //
    // `forcedInputLatencyTicks` is the fallback in exactly two situations, both
    // meaning "no per-connection tier is available":
    //   1. No tier table wired (single-argument constructor), and
    //   2. The tier table has never sampled this Address.
    //
    // Case 2 is why this asks `hasEntry` first rather than calling
    // `lookupInputDelayTicks` unconditionally: the tier table answers tier 0 for
    // an unknown Address (an optimistic default that is correct for ITS purpose),
    // so a bare lookup would silently return the tier-0 delay and the baseline
    // would become unreachable dead config.
    //
    // See TimeConfig.h:180-209, which states the same replacement semantics on
    // the fields themselves.
    int32_t effectiveDelay(const Address& addr) const
    {
        if (m_tierTable != nullptr && m_tierTable->hasEntry(addr))
        {
            return m_tierTable->lookupInputDelayTicks(addr);
        }
        return m_config.forcedInputLatencyTicks;
    }

    // Slot-key overload — convenience for the call sites that hold a full key
    // (the drain reads the delay once per key per tick). Forwards to the
    // Address form: every slot on a wire shares that wire's tier BY DESIGN, and
    // routing through the same function is what keeps that guaranteed rather
    // than merely intended.
    int32_t effectiveDelay(const SlotKey& key) const
    {
        return effectiveDelay(key.address);
    }

    bool hasTierTable() const
    {
        return m_tierTable != nullptr;
    }

    // Park `input` until tick `captureTick + effectiveDelay(key.address)`.
    //
    // DEDUP BY captureTick — the T3 append-only invariant, enforced at this
    // producer too. A redundancy bundle re-sends recent inputs by design, so the
    // same (key, SimT, captureTick) legitimately arrives several times; the
    // FIRST value wins and later ones are dropped. Note the dedup scope is now
    // the SLOT, not the wire: two characters on one machine that captured on the
    // same tick are two distinct keys and both are parked, which is precisely
    // the conflation this key widening removes. First-wins (not last-wins) is
    // the invariant the wire layer already established: `tryAppendSlot` in
    // InputRedundancyBundleCodec.h drops the duplicate and leaves the buffer
    // untouched, and RemoteMoveQueue::queueMove is first-writer-wins. A last-wins
    // rule here would let a late retransmission mutate an input the simulation
    // may already have consumed.
    template <typename SimT>
    void enqueue(const SlotKey& key, int32_t captureTick, const InputFor<SimT>& input)
    {
        static_assert(is_type_in_pack<SimT>(),
            "ServerInputDelayQueue::enqueue<SimT>: SimT is not in this queue's simulatable pack");

        std::deque<Entry<SimT>>& slot = queueFor<SimT>().bySlot[key];

        for (const Entry<SimT>& existing : slot)
        {
            if (existing.first == captureTick)
            {
                return;     // duplicate capture tick — first value wins
            }
        }

        slot.emplace_back(captureTick, input);

        // Liveness/activity stamp for the deadline branch of reapDeadHandles.
        // Stamped per ADDRESS, not per slot, and shared across the whole pack: a
        // connection is "active" if ANY of its simulatables on ANY of its slots
        // produced input. Both breadths are deliberate — liveness is a property
        // of the wire, so a couch co-op player who puts the controller down must
        // not have their parked input reaped out from under them while their
        // partner keeps the shared connection busy.
        int32_t& lastSeen = m_lastActivityTick[key.address];
        lastSeen = (lastSeen > captureTick) ? lastSeen : captureTick;
    }

    // Pop the input DUE at or before `currentServerTick`, if any.
    //
    // DUE-OR-OVERDUE release (og-netcode-v2-arch-latency / T26 — the input-drop
    // fix). An entry is releasable when
    //
    //     captureTick + effectiveDelay(addr) <= currentServerTick    (due, F1/F3)
    //          AND   captureTick >= staleBefore                      (in window, F3)
    //
    // This REPLACES the old exact `== currentServerTick` match, which stranded any
    // input whose exact release tick was skipped (a server sim-tick hitch, or a
    // tier change that shifted the whole deque's due tick past the tick being
    // drained) — the parked entry could then never match and was silently purged.
    // Releasing due-or-overdue means such an input is delivered a tick (or a few)
    // LATE instead of dropped; the server is authoritative + correction-repairing,
    // so a late input costs one client-side correction, strictly better than a
    // lost action. The in-time steady state is UNCHANGED: when the only due entry
    // is `captureTick == currentServerTick - delay`, `<=` selects exactly what
    // `==` did and the delivered tick is identical.
    //
    // `staleBefore` GATES the release to the rollback window (F3). Production
    // passes `firstUpcomingSimTick - rollbackWindowHardCap` — the SAME threshold
    // `purgeOlderThan` uses in the same drain — so an entry older than the window
    // is NOT released here; it falls through to the purge, which stays the SINGLE
    // drop point. Without this bound, lateness would plateau (production is 1
    // input/tick, release is 1/tick, so once late by k the queue stays k late) all
    // the way to the hard cap — permanent added latency invisible to the drop
    // counters. The default sentinel (`INT32_MIN`) means "no window gate" and is
    // for unit tests that exercise the release predicate in isolation; every
    // production caller passes the real bound.
    //
    // `outCaptureTick` (F1 — capture-tick propagation). When non-null, receives the
    // entry's STORED captureTick. The caller MUST deliver THAT downstream, NOT a
    // reconstructed `currentServerTick - delay`: under `<=` an overdue release
    // reconstructs a FUTURE input's tick, which collides with RemoteMoveQueue's
    // captureTick-keyed pending-window dedup and silently drops the later real
    // input. The queue stores the true tick (`Entry::first`); this surfaces it.
    //
    // MIN-captureTick SCAN, not deque-front (F2). `enqueue` is a bare push_back, so
    // under UDP reorder of redundancy bundles an older tick can sit AFTER a newer
    // one. Releasing the front could deliver out of order into the FIFO
    // RemoteMoveQueue and invert the player's action order; selecting the smallest
    // due captureTick keeps releases in capture order across the whole run.
    template <typename SimT>
    [[nodiscard]] bool tryDequeueForTick(const SlotKey& key,
                                         int32_t currentServerTick,
                                         InputFor<SimT>& out,
                                         int32_t staleBefore = std::numeric_limits<int32_t>::min(),
                                         int32_t* outCaptureTick = nullptr)
    {
        static_assert(is_type_in_pack<SimT>(),
            "ServerInputDelayQueue::tryDequeueForTick<SimT>: SimT is not in this queue's simulatable pack");

        PerSimQueue<SimT>& queue = queueFor<SimT>();
        const auto mapIt = queue.bySlot.find(key);
        if (mapIt == queue.bySlot.end())
        {
            return false;       // unknown slot — nothing was ever parked
        }

        std::deque<Entry<SimT>>& slot = mapIt->second;
        const int32_t delay = effectiveDelay(key.address);

        // Select the DUE, in-window entry with the smallest captureTick (F2).
        auto best = slot.end();
        for (auto it = slot.begin(); it != slot.end(); ++it)
        {
            const int32_t captureTick = it->first;
            if (captureTick < staleBefore)
            {
                continue;       // beyond the rollback window — leave for the purge
            }
            if (captureTick + delay > currentServerTick)
            {
                continue;       // not yet due
            }
            if (best == slot.end() || captureTick < best->first)
            {
                best = it;
            }
        }

        if (best == slot.end())
        {
            return false;
        }

        if (outCaptureTick != nullptr)
        {
            *outCaptureTick = best->first;      // F1: the TRUE stored capture tick
        }
        out = best->second;
        slot.erase(best);
        return true;
    }

    // Non-consuming readiness probe with the IDENTICAL predicate to
    // `tryDequeueForTick` (F4): due-or-overdue (`captureTick + delay <=
    // currentServerTick`) AND within the rollback window (`captureTick >=
    // staleBefore`). Returns true when ANY parked entry is releasable — it does
    // not need the min-scan, which only chooses AMONG the releasable set. Exists
    // for tests and for a caller that wants to know whether a tick is servable
    // before committing to consume it. `staleBefore` defaults to the same "no
    // window gate" sentinel as `tryDequeueForTick`.
    template <typename SimT>
    bool hasReadyForTick(const SlotKey& key,
                         int32_t currentServerTick,
                         int32_t staleBefore = std::numeric_limits<int32_t>::min()) const
    {
        static_assert(is_type_in_pack<SimT>(),
            "ServerInputDelayQueue::hasReadyForTick<SimT>: SimT is not in this queue's simulatable pack");

        const PerSimQueue<SimT>& queue = queueFor<SimT>();
        const auto mapIt = queue.bySlot.find(key);
        if (mapIt == queue.bySlot.end())
        {
            return false;
        }

        const int32_t delay = effectiveDelay(key.address);
        for (const Entry<SimT>& entry : mapIt->second)
        {
            const int32_t captureTick = entry.first;
            if (captureTick >= staleBefore && captureTick + delay <= currentServerTick)
            {
                return true;
            }
        }

        return false;
    }

    // Drop every parked entry whose captureTick is STRICTLY OLDER than
    // `staleCaptureTick`; an entry captured exactly at the threshold survives.
    // Without this, an input that was never dequeued (its due tick was skipped,
    // or the tier moved under it) would sit in the deque for the lifetime of the
    // connection.
    //
    // RETURNS the discarded entries (slot + captureTick). (T25.) A stranded input
    // reclaimed here is a real dropped remote input, so the purge no longer erases
    // silently — it hands each reclaimed (slotKey, captureTick) back to the caller,
    // which logs it as `[Warning][InputDrop]`. NOT `[[nodiscard]]`: the existing
    // Catch2 call sites purge for effect and ignore the return, and the value is a
    // diagnostic by-product, not a result the caller is obligated to consume.
    //
    // Empty per-slot deques are erased rather than left behind, so a quiet
    // player does not keep an empty bucket alive in the map.
    template <typename SimT>
    std::vector<PurgedEntry> purgeOlderThan(int32_t staleCaptureTick)
    {
        static_assert(is_type_in_pack<SimT>(),
            "ServerInputDelayQueue::purgeOlderThan<SimT>: SimT is not in this queue's simulatable pack");

        std::vector<PurgedEntry> discarded;

        PerSimQueue<SimT>& queue = queueFor<SimT>();
        for (auto mapIt = queue.bySlot.begin(); mapIt != queue.bySlot.end();)
        {
            std::deque<Entry<SimT>>& slot = mapIt->second;
            for (auto it = slot.begin(); it != slot.end();)
            {
                if (it->first < staleCaptureTick)
                {
                    discarded.push_back(PurgedEntry{ mapIt->first, it->first });
                    it = slot.erase(it);
                }
                else
                {
                    it = std::next(it);
                }
            }
            mapIt = slot.empty() ? queue.bySlot.erase(mapIt) : std::next(mapIt);
        }

        return discarded;
    }

    // Evict every trace of connections that have died OR gone silent longer than
    // `deadlineTicks`. Mirrors ConnectionTierTable::reapDeadHandles — same two
    // conditions, for the same two different failures: `isAlive()` is the prompt
    // signal for a clean disconnect, the deadline catches a connection that
    // stopped producing input without its handle reporting dead.
    //
    // Walks the ENTIRE tuple in one pass. This is the single-container payoff:
    // an Address is removed from every simulatable's queue and from the activity
    // map atomically, so no simulatable can retain input for a connection another
    // simulatable has already forgotten.
    //
    // ACROSS EVERY SLOT, TOO (T17). The reap key is the Address, and it clears
    // EVERY (Address, slot) bucket that wire owns — a disconnect takes the whole
    // machine down, so leaving one couch player's parked input behind would be a
    // leak that no later reap could reach, since nothing would ever stamp that
    // address active again.
    void reapDeadHandles(int32_t currentTick, int32_t deadlineTicks)
    {
        for (auto it = m_lastActivityTick.begin(); it != m_lastActivityTick.end();)
        {
            const bool dead = !it->first.isAlive();
            const bool stale = (it->second + deadlineTicks) < currentTick;

            if (dead || stale)
            {
                eraseAddressFromAllQueues(it->first);
                it = m_lastActivityTick.erase(it);
            }
            else
            {
                it = std::next(it);
            }
        }
    }

    // --- Introspection (tests / future Stage-4 telemetry) ------------------

    template <typename SimT>
    std::size_t pendingCount(const SlotKey& key) const
    {
        const PerSimQueue<SimT>& queue = queueFor<SimT>();
        const auto mapIt = queue.bySlot.find(key);
        return mapIt == queue.bySlot.end() ? 0u : mapIt->second.size();
    }

    // How many distinct player slots this wire currently has parked input for,
    // for the given simulatable. Exists so a test (and Stage-4 telemetry) can
    // assert the couch co-op invariant directly: N characters on one Address
    // must occupy N buckets, not one.
    template <typename SimT>
    std::size_t slotCountFor(const Address& addr) const
    {
        const PerSimQueue<SimT>& queue = queueFor<SimT>();
        std::size_t count = 0u;
        for (const auto& entry : queue.bySlot)
        {
            if (entry.first.address == addr)
            {
                ++count;
            }
        }
        return count;
    }

    bool hasConnection(const Address& addr) const
    {
        return m_lastActivityTick.find(addr) != m_lastActivityTick.end();
    }

    std::size_t connectionCount() const
    {
        return m_lastActivityTick.size();
    }

private:
    template <typename SimT>
    using Entry = std::pair<int32_t /*captureTick*/, InputFor<SimT>>;

    // One map per simulatable type. Private nested so the storage shape stays an
    // implementation detail — callers reach it only through the templated API.
    template <typename SimT>
    struct PerSimQueue
    {
        // Keyed on the FULL slot key (T17). See the TWO KEYS note at the top of
        // this file: this is the per-CHARACTER half of the split, and it is the
        // only member here that is not keyed per-wire.
        std::unordered_map<SlotKey, std::deque<Entry<SimT>>> bySlot;
    };

    template <typename SimT>
    static constexpr bool is_type_in_pack()
    {
        return (std::is_same_v<SimT, SimulatableTs> || ...);
    }

    // Compile-time dispatch by TYPE, not by index — `std::get<T>` is ill-formed
    // if the pack contains T more than once, which makes a duplicated simulatable
    // in the pack a compile error rather than a silently-shadowed queue.
    template <typename SimT>
    PerSimQueue<SimT>& queueFor()
    {
        return std::get<PerSimQueue<SimT>>(m_queues);
    }

    template <typename SimT>
    const PerSimQueue<SimT>& queueFor() const
    {
        return std::get<PerSimQueue<SimT>>(m_queues);
    }

    // Erase every SLOT belonging to `addr`, in every simulatable's queue. Cannot
    // be a plain `erase(key)` any more: the map is keyed on (Address, slot) and
    // this caller only knows the Address half, so each bucket is scanned. The
    // cost is bounded by parked-input volume and this runs on the reap cadence
    // (once per tierMinDwellTicks), not per tick.
    void eraseAddressFromAllQueues(const Address& addr)
    {
        std::apply(
            [&addr](auto&... queues)
            {
                auto eraseFrom = [&addr](auto& queue)
                {
                    for (auto it = queue.bySlot.begin(); it != queue.bySlot.end();)
                    {
                        it = (it->first.address == addr) ? queue.bySlot.erase(it) : std::next(it);
                    }
                };
                (eraseFrom(queues), ...);
            },
            m_queues);
    }

    const TimeConfig& m_config;

    // Pointer, not reference, so "no tier table wired" is representable — that
    // is the state `forcedInputLatencyTicks` exists to serve. Never owned.
    const TierTable* m_tierTable;

    // THE single container, variadic over the pack (proposal Correction 4).
    std::tuple<PerSimQueue<SimulatableTs>...> m_queues;

    // Per-connection activity stamp, shared across the whole pack. Lives outside
    // the tuple deliberately: connection liveness is a per-CONNECTION fact, not a
    // per-simulatable one, and keeping one copy is what makes the reap uniform.
    std::unordered_map<Address, int32_t> m_lastActivityTick;
};
