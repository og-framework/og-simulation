#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/CorrectionStateBufferCodec.h"   // kNoInputCaptureTick
#include "OGSimulation/RelayedInputRingCodec.h"        // the wire fence + forEachEntry

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// RemoteInputCache<InputT> -- the CLIENT-side read cache of RELAYED inputs.
//
// ORIENTATION
//
// ONE STORE PER REMOTE CHARACTER -- a character this client does not control. It
// accumulates the `(captureTick, dA, input)` triples the server relays for that
// character. `dA` is the SCHEDULE STAMP: the effective input delay the server held
// for that wire when it received the capture.
//
// TWO READERS, ONE STORE:
//   proxy prediction  find(N - dLatest)  integrate the sender's REAL input at
//                                        (about) the tick the authority applies it
//   resim             find(ref)          replay exactly what the authority did;
//                                        `ref` is the applied-capture-tick reference
//                                        the correction state carries
//
// THE THREE NAMES. Two of them are now symmetric and their contracts are not:
//   LocalInputCache<InputT>   this machine's OWN captures, read at `T - d`.
//                             Sender-side delay mechanism. CLOCK-KEYED.
//   the relayed input ring    the WIRE payload, server -> peers. Bytes, plus its
//                             codec in core. One adapter's binding:
//                             `FRelayedInputRing`.
//   RemoteInputCache<InputT>  THIS: the client's typed read-side cache. SENDER-KEYED.
//
// THE CONTRACTS, one line each. Every one is fenced at its own declaration:
//   find()               PURE hit/miss. Never invents; the miss stays visible.
//   findLatest()         A DERIVATION over the slots. `dLatest` / `lastKnown` are
//                        views on it; neither is a field.
//   fallback()           ARGUMENT-LESS. Newest arrived input, else the INJECTED zero.
//   push()               LAST-WINS on a resident tick, so ingest is idempotent.
//   kNoInputCaptureTick  NEVER a store key, on the write path or the read path.
//   capacity             64 slots -- and the number is load-bearing for the relay
//                        delay floor's hard cap.
//
// THREADING: GAME-thread write, PHYSICS-thread read, NO SEAM. Accepted, priced,
// correction-healed debt -- `docs/ThreadingCrossings.md` row 1, analysis of record in
// `docs/RemoteInputCache-rationale.md` §6. The PROHIBITIONS stay here.
//
// WHAT PINS WHAT, so a reader knows which test breaks on a change:
//   no wipe surface exists       `RemoteInputCacheTest.cpp` -- the `HasClearMethod`
//                                static_assert pair, and its own TEST_CASE
//   the wipe asymmetry, live     `SimulationInputResolutionTest.cpp` -- three cases,
//                                named at the WIPE ASYMMETRY fence
//   ingest + the version fence   `RemoteInputCacheTest.cpp`
//   the one-shot mismatch gate   `NetSyncTelemetryTest.cpp` (it left this class)
//
// ENGINE-AGNOSTIC. STL plus other OGSimulation core headers only. The ring arrives as
// a template parameter satisfying the codec's BUFFER CONCEPT, which is what lets the
// pure-C++ Low-Level-Tests drive the REAL ingest path against a std::vector buffer.
// GLOBAL NAMESPACE, like the rest of the OGSim core (same note as LocalInputCache /
// ConnectionTierTable / SimulatableList). The design corpus writes `ogsim::`; no such
// namespace exists in this tree.
//
// Derivations, history and the deferred work: `docs/RemoteInputCache-rationale.md`.
// The end-to-end narrative:                   `docs/Perspective-RemoteInputFlow.md`.
//
// ---------------------------------------------------------------------------
// WHY THIS IS NOT A LocalInputCache -- three facts, and the third is decisive. §1
//
// * THE PAYLOAD DIFFERS: a slot here is `(dA, input)`, not `InputT`.
// * THE MISS SEMANTICS DIFFER: the delay line's `at()` INVENTS the neutral on a miss;
//   `find()` here never invents anything.
// * THE WIPE DIVERGES -- see the WIPE ASYMMETRY fence at the class declaration.
//
// ---------------------------------------------------------------------------
// "LATEST" IS DERIVED, NOT STORED (architect ruling 2026-08-04, user-directed).
//
// ⛔ DO NOT ADD A STORED `dLatest`: `push` is last-wins, so it needs `>=`, not `>`. §3
// ⛔ A MEMO NEEDS private + mutable + mutator-only refresh + an equivalence test. §3
// ⛔ COMBINED SLOT, NEVER PARALLEL RINGS -- two rings verify tick A against stamp B. §1
//
// ---------------------------------------------------------------------------
// FALLBACK, AND THE DELIBERATELY ABSENT HOLD RULE (scope ruling 2026-08-04).
//
// `fallback()` is ARGUMENT-LESS: newest arrived input if anything ever arrived,
// otherwise the INJECTED game zero. It takes NO `frontierTick`, and there is no
// `relayStaleInputHoldTicks`. That is deliberate, not an omission:
//
//   * Without a hold window the store needs no knowledge of the caller's clock,
//     which keeps it clear of the receiver-frontier-vs-sender-capture-tick
//     geometry entirely (two different leads over the server, ~5-15 ticks of
//     noise). The future rule adds the parameter; pre-adding it unused would
//     invite someone to feed it a tick from the wrong domain.
//   * Unbounded hold is ALREADY today's behaviour -- the proxy branch reads the
//     correction cache's last input, which persists indefinitely if corrections
//     stop. Deferring therefore preserves the increment's headline
//     no-observable-regression gate rather than risking it.
//
// THE DEFERRED RULE, so it is not lost (also `docs/RemoteInputCache-rationale.md`
// §5): hold last-known at most K ticks, then fall to the
// game zero. It matters because relay silence does NOT mean "the player is idle" --
// the input provider is polled every tick, so a motionless player still emits a
// full-rate stream of neutral-CONTENT captures. Silence means the WIRE is quiet
// (starvation, loss burst, pre-registration, disconnect-in-progress), and the
// corrections that normally bound extrapolation share that same starving
// connection. Holding a stale "moving forward + attacking" input indefinitely is
// the phantom-hit class. Eviction does not bound it by itself: slots are reclaimed
// by OVERWRITE, never by time, so a silent relay evicts nothing. It becomes
// LOAD-BEARING at sparse state (a longer heal interval is a longer hold window) --
// a named prerequisite of that increment, not optional polish.
// `RelayReadProbe.h`'s `maxConsecutiveFallbackRun` records the longest observed run,
// so K is set from data.
//
// ---------------------------------------------------------------------------
// ⛔ THE NEUTRAL IS INJECTED, NEVER `InputT{}` -- value-init aims (0,0,0), not (0,0,1). §4
// ⛔ `InputT{}` exists ONLY so an engine-free unit test can build a store. §4
// ⛔ `kNoInputCaptureTick` IS NEVER A STORE KEY: `push` rejects it, `find` refuses it. §4
// ⛔ `!findLatest().valid` MEANS NO SCHEDULED PROBE AT ALL, never a probe at `dA` 0. §4
//
// ---------------------------------------------------------------------------
// THREADING -- GAME-THREAD WRITE / PHYSICS-THREAD READ. ACCEPTED DEBT.
// (Ruling 2026-08-04, option (a); source-verified at the time, stated as fact.)
//
// WRITER: the adapter's relayed-ring arrival callback, on the GAME thread, routed into
// `populateRemoteInputCache`. READER: `SimulationInputResolution::collectInputAll` and
// `SimulationInputResolution::collectResimInputAll`, on the PHYSICS thread, i.e. only
// where the host ticks physics asynchronously. ONE ADAPTER'S BINDING FOR BOTH ENDS:
// the async-physics switch is `bTickPhysicsAsync`; the arrival callback is
// `ASimulationInputRelay::OnRep_RelayedInputRing`, which hands the ring to the owning
// per-character component and thence to the core callback `registerPredictionOwner`
// installed. `docs/ThreadingCrossings.md` row 1 cites this block as authoritative.
//
// ⛔⛔ DO NOT COPY LocalInputCache's "not thread-safe; single-threaded by construction"
//   claim with the ring mechanics: TRUE of the delay line (its push and read are two
//   statements in one `collectInputAll` call), FALSE here. Reject a copied one. §6
// ⛔ ACCEPTED, NOT OVERLOOKED -- inherited, torn-slot-bounded, correction-healed. §6
// ⛔ THE DEFERRED FIX IS AN SPSC RING at `collectInputAll`, as `RemoteMoveQueue` does. §6
// ---------------------------------------------------------------------------

// Slot count of a default-constructed RemoteInputCache.
// ⛔ A FREE CONSTANT, so config code derives bounds without naming an `InputT`. §2
// ⛔⛔ LOWER IT AND `relayDelayFloorHardCapTicks` (Network/ConnectionTierTable.h) MUST
//   FOLLOW IT DOWN, or a configured floor schedules reads against evicted entries. §2
inline constexpr std::size_t kRemoteInputCacheCapacityTicks = 64u;

// ---------------------------------------------------------------------------
// Formerly RelayedInputStore, renamed 2026-08-16 to pair with LocalInputCache.
//
// ⛔⛔ NEVER WIPE THIS CACHE ON A RESYNC, AND NEVER GIVE IT A `clear()`. SENDER-KEYED --
//   the keys are the SENDER's capture ticks, which a LOCAL clock jump cannot invalidate
//   -- so mirroring `wipeAllForResync`'s per-id `LocalInputCache` loop for a third map
//   blinds every remote proxy for a window after every resync. OBSERVED 2026-08-04. §7
// ⛔ THE SYMMETRIC NAMES CANNOT CARRY THAT; this fence and `LocalInputCache.h`'s do. §7
// ⛔ ENFORCED, NOT ADVISORY -- a wipe fails the suite: `HasClearMethod` + "no wipe
//   surface" (`RemoteInputCacheTest.cpp`), and `ResyncWipeClearsTheLocalInputCache` /
//   `RemoteInputCacheSURVIVESAHardResyncWipe` /
//   `ResimRemoteStillResolvesAfterAHardResyncWipe` (`SimulationInputResolutionTest.cpp`). §7
// ⛔ Both keying domains: `docs/Perspective-RemoteInputFlow.md` §7, not re-derived here.
// ---------------------------------------------------------------------------

template <typename InputT>
class RemoteInputCache
{
public:
    // The result of the `findLatest()` DERIVATION.
    // ⛔ BY VALUE -- an answer computed from the slots, never a handle into them. §3
    struct LatestEntry
    {
        bool          valid       = false;
        std::uint32_t captureTick = 0;
        std::uint8_t  dA          = 0;
        InputT        input{};
    };

    static constexpr std::size_t kDefaultCapacityTicks = kRemoteInputCacheCapacityTicks;

    explicit RemoteInputCache(InputT neutralInput = InputT{},
                              std::size_t capacity = kDefaultCapacityTicks)
        : m_neutral(std::move(neutralInput))
        , m_slots(capacity == 0u ? kDefaultCapacityTicks : capacity)
    {
    }

    // Replace the injected game zero after construction.
    // ⛔ ORDER-INDEPENDENT, exactly as `LocalInputCache::setNeutralInput` is. §4
    void setNeutralInput(const InputT& neutralInput)
    {
        m_neutral = neutralInput;
    }

    const InputT& getNeutralInput() const
    {
        return m_neutral;
    }

    std::size_t capacity() const
    {
        return m_slots.size();
    }

    // Record a relayed entry.
    // ⛔ LAST-WINS on a resident capture tick -- what makes the ingest idempotent. §5
    // ⛔ Returns false -- changing nothing -- ONLY for `kNoInputCaptureTick`. §4
    bool push(std::uint32_t captureTick, std::uint8_t dA, const InputT& input)
    {
        if (captureTick == kNoInputCaptureTick)
        {
            return false;
        }

        Slot& slot       = m_slots[slotIndexFor(captureTick)];
        slot.captureTick = captureTick;
        slot.occupied    = true;
        slot.dA          = dA;
        slot.input       = input;
        return true;
    }

    // True iff `captureTick` is currently resident.
    bool has(std::uint32_t captureTick) const
    {
        if (captureTick == kNoInputCaptureTick)
        {
            return false;
        }
        const Slot& slot = m_slots[slotIndexFor(captureTick)];
        return slot.occupied && slot.captureTick == captureTick;
    }

    // PURE HIT/MISS lookup.
    // ⛔ NEVER INVENTS A NEUTRAL -- the whole difference from `LocalInputCache::at()`,
    //   and what keeps a miss VISIBLE to the ladder and to the resim table. §1
    // ⛔ ON A MISS `outDA` AND `outInput` ARE LEFT UNTOUCHED, mirroring `findEntry`. §5
    bool find(std::uint32_t captureTick, std::uint8_t& outDA, InputT& outInput) const
    {
        if (captureTick == kNoInputCaptureTick)
        {
            return false;
        }

        const Slot& slot = m_slots[slotIndexFor(captureTick)];
        if (!slot.occupied || slot.captureTick != captureTick)
        {
            return false;
        }

        outDA     = slot.dA;
        outInput  = slot.input;
        return true;
    }

    // THE DERIVATION -- the newest relayed entry, computed from the slots on every call.
    // `valid == false` iff nothing has ever arrived for this character.
    // ⛔ THIS IS WHAT `dLatest` AND `lastKnown` MEAN. Neither is a field. §3
    LatestEntry findLatest() const
    {
        bool              found = false;
        const std::size_t index = latestSlotIndex(found);

        LatestEntry latest;
        if (!found)
        {
            return latest;      // valid == false; every other member is meaningless
        }

        const Slot& slot   = m_slots[index];
        latest.valid       = true;
        latest.captureTick = slot.captureTick;
        latest.dA          = slot.dA;
        latest.input       = slot.input;
        return latest;
    }

    // The terminal value of the scheduled-read ladder: the newest arrived input once
    // ANYTHING has arrived, otherwise the injected game zero.
    // ⛔ ARGUMENT-LESS. DO NOT ADD A `frontierTick` "FOR LATER" -- see the absence fence. §5
    // ⛔ Shares `latestSlotIndex` with `findLatest()`; returns a reference, per-tick path. §3
    const InputT& fallback() const
    {
        bool              found = false;
        const std::size_t index = latestSlotIndex(found);
        return found ? m_slots[index].input : m_neutral;
    }

    // THE RESIDENT SPAN -- oldest, newest, count.
    //
    // WHAT IT IS FOR. `find(probeTick)` answering false says a scheduled read MISSED; it
    // does not say WHY, and the three whys have three different remedies:
    //
    //   inside [oldest, newest], absent   A COVERAGE HOLE -- produced by the sender and
    //                                     clobbered before replication. Raise the depth.
    //   > newest                          Not produced yet, or not landed yet. DEPTH IS
    //                                     IRRELEVANT: this is the delay deficit.
    //   < oldest                          Older than the store retains: clock
    //                                     misalignment, or capacity.
    //
    // ⛔ THE SPAN IS THE STORE'S, NOT THE RING'S -- the store ACCUMULATES arrivals, so
    //   an in-span hole is meaningful even at depth 1. §8
    // ⛔ ONE SCAN, not memoized, for the reason `findLatest()` gives. §3
    struct ResidentSpan
    {
        bool          valid  = false;   // false iff nothing has ever arrived
        std::uint32_t oldest = 0;
        std::uint32_t newest = 0;
        std::size_t   count  = 0;
    };

    ResidentSpan residentSpan() const
    {
        ResidentSpan span;
        for (const Slot& slot : m_slots)
        {
            if (!slot.occupied)
            {
                continue;
            }

            if (!span.valid)
            {
                span.valid  = true;
                span.oldest = slot.captureTick;
                span.newest = slot.captureTick;
            }
            else if (slot.captureTick < span.oldest)
            {
                span.oldest = slot.captureTick;
            }
            else if (slot.captureTick > span.newest)
            {
                span.newest = slot.captureTick;
            }

            ++span.count;
        }
        return span;
    }

    // Resident entry count. Tests / telemetry only.
    std::size_t residentCount() const
    {
        std::size_t count = 0u;
        for (const Slot& slot : m_slots)
        {
            count += slot.occupied ? 1u : 0u;
        }
        return count;
    }

    // ⛔ `shouldLogVersionMismatchOnce()` IS GONE FROM THIS CLASS -- `NetSyncTelemetry.h`. §9
    // ⛔ DO NOT RE-ADD IT HERE: log-suppression state belongs with the logger, not the
    //   container. Its four assertions live in `NetSyncTelemetryTest.cpp`. §9

private:
    struct Slot
    {
        std::uint32_t captureTick = 0;
        bool          occupied    = false;
        std::uint8_t  dA          = 0;
        InputT        input{};
    };

    std::size_t slotIndexFor(std::uint32_t captureTick) const
    {
        return static_cast<std::size_t>(captureTick) % m_slots.size();
    }

    // THE single scan behind both `findLatest()` and `fallback()`.
    // ⛔ PLAIN `>` IS CORRECT: the relay stream is MONOTONIC in capture tick. §3
    // ⛔ Tick-counter wraparound is out of scope: 2^32 ticks at 60 Hz is ~828 days. §4
    std::size_t latestSlotIndex(bool& outFound) const
    {
        std::size_t   bestIndex = 0u;
        std::uint32_t bestTick  = 0u;
        bool          found     = false;

        for (std::size_t i = 0u; i < m_slots.size(); ++i)
        {
            const Slot& slot = m_slots[i];
            if (!slot.occupied)
            {
                continue;
            }
            if (!found || slot.captureTick > bestTick)
            {
                found     = true;
                bestTick  = slot.captureTick;
                bestIndex = i;
            }
        }

        outFound = found;
        return bestIndex;
    }

    InputT            m_neutral;
    std::vector<Slot> m_slots;
};

// ---------------------------------------------------------------------------
// populateRemoteInputCache -- THE ingest, in one place, carrying the wire fence.
//
// It lives in core beside the store (the RelayedInputRingCodec precedent) so the
// engine-free Low-Level-Tests exercise the REAL path: the adapter side does nothing
// but call this with the freshly-replicated ring.
//
// ONE ADAPTER'S BINDING FOR THE WIRE NAMES IN THIS BLOCK: the replicated wrapper is
// `FRelayedInputRing`, its property serializer is `NetSerialize`, and the archive's
// failure switch is `SetError`. Another adapter substitutes its own; every claim here
// is about those ROLES, not about one engine.
//
// ⛔⛔ THE VERSION IS CHECKED BEFORE ANY STRUCTURAL READ. The serializer only
//   LENGTH-checks, so a layout change makes an old peer read arbitrary bytes as
//   `captureTick` / `dA` / `input` and insert them as STORE KEYS. Silent corruption. §9
//
// Three-way, and `RelayedInputIngestOutcome` names all three:
//   version 0        never written / never replicated. No-op, and deliberately NO log:
//                    the codec writes its header lazily, so an empty ring reads 0 and
//                    can never false-positive as a mismatch.
//   version matches  consume.
//   anything else    DROP THE WHOLE RING. Consume nothing. The caller logs once per
//                    component per session
//                    (`NetSyncTelemetry::shouldLogVersionMismatchOnce`).
//
// ⛔⛔ DROP AT CONSUME -- DO NOT FAIL THE ARCHIVE, AND DO NOT "FIX" THIS TO MATCH THE
//   LENGTH CHECK. A bad LENGTH prefix desynchronizes the archive, so failing the bunch
//   is mandatory; a VERSION mismatch is found AFTER a clean, exact read, so failing
//   there destroys unrelated properties in the same bunch for no integrity gain. §9
// ⛔ IT RESTS ON ONE INVARIANT: THE u16 LENGTH-PREFIX FRAMING IS VERSION-INVARIANT. §9
// ⛔ INGEST RE-CONSUMES EVERYTHING, NO DIFFING -- `push` is idempotent by capture tick. §5
// ---------------------------------------------------------------------------

enum class RelayedInputIngestOutcome : std::uint8_t
{
    // Version byte 0: the ring has never been written. Not an error, not logged.
    NeverWritten,
    // Version matched; `entriesIngested` entries were pushed.
    Consumed,
    // Version mismatch: the WHOLE ring was dropped and nothing was pushed.
    VersionMismatch,
};

struct RelayedInputIngestReport
{
    RelayedInputIngestOutcome outcome         = RelayedInputIngestOutcome::NeverWritten;
    std::uint8_t              versionOnWire   = 0u;
    std::uint8_t              entriesIngested = 0u;

    // -----------------------------------------------------------------------
    // HOW MANY CAPTURE TICKS THIS ARRIVAL ACTUALLY ADDED.
    //
    // `entriesIngested` counts entries PUSHED, which under the re-consume ingest
    // includes every already-resident entry the ring carried again. This counts only
    // entries whose capture tick was NOT resident when the arrival began -- the NEW
    // COVERAGE -- and it is what `RelayArrivalProbe::noteArrival` charges loss against.
    //
    // ⛔⛔ DO NOT SUBSTITUTE `entriesIngested`, AND DO NOT ASSUME 1. One flush-on-poll
    //   arrival carries the whole BURST, so assuming 1 charges the burst RATE as loss:
    //   ~120 per mille against a ~11 pass condition, on a flush that lost nothing. §8
    // ⛔ RE-DELIVERY IS NOT COVERAGE: a `dA` re-stamp or a republished unevicted entry
    //   adds nothing, and counting it would flatter a stalled sender. §8
    // ⛔ FREE: `has()` is an O(1) slot probe and the ingest already visits every entry. §8
    // -----------------------------------------------------------------------
    std::uint8_t              newCaptureTicksIngested = 0u;

    // THE NEWEST CAPTURE TICK THIS ARRIVAL CARRIED -- the replication-cadence probe's
    // entire data source (`Network/RelayReadProbe.h`, probe 2).
    //
    // ⛔ `store.findLatest()` IS THE WRONG ANSWER HERE: it scans the STORE, so an arrival
    //   carrying nothing new would report the previous tick and the gap would read 0. §8
    // ⛔ `valid` IS A FLAG, NOT A SENTINEL VALUE -- tick 0 is an ordinary capture tick. §8
    bool          newestCaptureTickValid = false;
    std::uint32_t newestCaptureTick      = 0u;
};

template <typename InputType, typename RingT>
RelayedInputIngestReport populateRemoteInputCache(RemoteInputCache<InputType>& store,
                                                  const RingT&                  ring)
{
    RelayedInputIngestReport report;
    report.versionOnWire = relayedInputRing::getWireFormatVersion(ring);

    if (report.versionOnWire == 0u)
    {
        report.outcome = RelayedInputIngestOutcome::NeverWritten;
        return report;
    }

    if (report.versionOnWire != relayedInputRing::kWireFormatVersion)
    {
        report.outcome = RelayedInputIngestOutcome::VersionMismatch;
        return report;      // nothing structural is read on a mismatch — see above
    }

    relayedInputRing::forEachEntry<InputType>(
        ring,
        [&store, &report](std::uint32_t captureTick, std::uint8_t dA, const InputType& input)
        {
            // ⛔ Asked BEFORE the push, because the push is what makes it resident. §8
            const bool wasAlreadyResident = store.has(captureTick);

            if (store.push(captureTick, dA, input))
            {
                ++report.entriesIngested;
                if (!wasAlreadyResident)
                {
                    ++report.newCaptureTicksIngested;
                }

                // ⛔ Maximum over ACCEPTED entries, so a rejected sentinel cannot win. §8
                if (!report.newestCaptureTickValid || captureTick > report.newestCaptureTick)
                {
                    report.newestCaptureTickValid = true;
                    report.newestCaptureTick      = captureTick;
                }
            }
        });

    report.outcome = RelayedInputIngestOutcome::Consumed;
    return report;
}
