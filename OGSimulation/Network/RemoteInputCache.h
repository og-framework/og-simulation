#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/CorrectionStateBufferCodec.h"   // kNoInputCaptureTick
#include "OGSimulation/RelayedInputRingCodec.h"        // the wire fence + forEachEntry

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// RemoteInputCache<InputT> — the CLIENT-side read cache of RELAYED inputs.
// (og-netcode-v2-input-relay T5; RelayDelaySpectrumDesign.md §4, §5.2, §8.6/§8.7.)
//
// WHAT IT IS. One store per REMOTE character (a character this client does not
// control). It accumulates the `(captureTick, dA, input)` entries the server
// relays for that character — `dA` being the SCHEDULE STAMP, the effective input
// delay the server held for that wire at receipt — so that:
//
//   * T7's proxy prediction can read `find(N - dLatest)` and integrate the
//     character's REAL input at (approximately) the tick the authority applies
//     it, instead of extrapolating the last correction; and
//   * T6's resim can resolve `find(ref)` for the applied-capture-tick reference
//     the correction state carries, i.e. replay exactly what the authority did.
//
// ---------------------------------------------------------------------------
// THE THREE NAMES, AND WHY THIS IS NOT A LocalInputCache
// (architect ruling 2026-08-04; vocabulary lockdown in the design doc's §4 table).
//
//   LocalInputCache<InputT>       the LOCAL character's own captures, read at
//                                 `T - d`. Sender-side delay mechanism.
//   FRelayedInputRing (+ codec)   the WIRE payload, server -> peers. Bytes.
//   RemoteInputCache<InputT>      THIS: the client's typed read-side cache.
//
// The earlier plan was to reuse LocalInputCache. Three accumulated facts
// flipped that trade, and the third is decisive:
//
//   1. THE PAYLOAD DIFFERS. A slot here is `(dA, input)`, not `InputT` — reuse
//      already meant a widened slot plus sidecar fields.
//   2. THE MISS SEMANTICS DIFFER. `at()` on the delay line INVENTS the neutral on
//      a miss; `find()` here is pure hit/miss and never invents anything, because
//      T7's ladder must SEE the miss in order to fall back to last-known. The
//      store has no neutral on the lookup path at all.
//   3. THE WIPE DIVERGES — and this is the one that makes reuse unsafe rather
//      than merely awkward. `LocalInputCache::clear()` is CONTRACTUALLY swept
//      by SimulationNetSync::wipeAllForResync, because a local capture is keyed
//      to the pre-resync PREDICTION clock and stops meaning anything when that
//      clock jumps. A relayed entry is keyed to the SENDER's capture tick — a
//      server-domain identity that a LOCAL hard resync does not invalidate. A
//      reused type would be wiped by whoever next mirrors the existing per-id map
//      loops in that function, silently, and the only symptom would be a proxy
//      that goes blind for a window after every resync. See the deliberate
//      NON-wipe comment at the wipeAllForResync site.
//
// COMBINED SLOT, NEVER PARALLEL RINGS (architect ruling). `dA` and `input` live
// in ONE slot keyed by ONE capture tick. Two parallel rings would silently defeat
// T7's verify step, which checks a candidate tick against ITS OWN stamp — with
// two rings it would be checking ring-A's tick against ring-B's stamp.
//
// ---------------------------------------------------------------------------
// "LATEST" IS DERIVED, NOT STORED (architect ruling 2026-08-04, user-directed).
//
// `findLatest()` — a scan of the occupied slots for the highest capture tick — IS
// the truth. The design vocabulary's `dLatest` and `lastKnown` are VIEWS on that
// derivation (`findLatest().dA` and `findLatest().input`, the latter surfaced as
// `fallback()`); they are not fields and there is no second source of truth.
//
// WHY THIS IS A RULING AND NOT A PREFERENCE. A stored "latest" scalar needs a
// `>=` update rule, not a `>` one: `push` is last-wins, so a re-stamp of the
// ALREADY-latest capture tick with a FRESHER `dA` must overwrite the scalar. Get
// that wrong and `dLatest` goes stale precisely during a delay transition — the
// one moment it matters. Deriving deletes the invariant outright: `push` rewrites
// the slot in place and the derivation reads the slot's current `dA`.
//
// COST is <= `capacity()` comparisons. The only site that could ever matter is
// T6's frontier ticks inside a deep resim (<= ~20 replayed ticks x N characters).
// IF A PROFILE EVER JUSTIFIES A MEMO it must be `private`, `mutable`, co-located
// immediately with `findLatest()`, refreshed ONLY inside the mutators, never
// writable from outside, carry the header comment "this is a memo; if it and
// findLatest() disagree, findLatest() is right" — and it may NOT land without the
// randomized-equivalence test named in T5's acceptance criteria.
//
// `fallback()` shares the SAME private scan as `findLatest()` (see
// `latestSlotIndex`) precisely so that "one derivation" stays literally true
// rather than being two functions that happen to agree today.
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
//   * Unbounded hold is ALREADY today's behaviour — the proxy branch reads the
//     correction cache's last input, which persists indefinitely if corrections
//     stop. Deferring therefore preserves the increment's headline
//     no-observable-regression gate rather than risking it.
//
// THE DEFERRED RULE, so it is not lost (also RelayDelaySpectrumDesign.md §8.7 and
// the Backlog's "Out of scope"): hold last-known at most K ticks, then fall to the
// game zero. It matters because relay silence does NOT mean "the player is idle" —
// the input provider is polled every tick, so a motionless player still emits a
// full-rate stream of neutral-CONTENT captures. Silence means the WIRE is quiet
// (starvation, loss burst, pre-registration, disconnect-in-progress), and the
// corrections that normally bound extrapolation share that same starving
// connection. Holding a stale "moving forward + attacking" input indefinitely is
// the phantom-hit class. Eviction does not bound it by itself: slots are reclaimed
// by OVERWRITE, never by time, so a silent relay evicts nothing. It becomes
// LOAD-BEARING at sparse state (a longer heal interval is a longer hold window) —
// a named prerequisite of that increment, not optional polish. T9's probe records
// the max consecutive `fallback()` run so K is set from data.
//
// THE NEUTRAL IS INJECTED, NEVER `InputT{}` — the same load-bearing distinction
// LocalInputCache documents: `simulatableBrawler::getZeroPlayerInput()`
// builds forward vectors of (0,0,1), while a value-initialised PlayerInput would
// carry a (0,0,0) forward vector into normalisation. The default `InputT{}` here
// exists purely so an engine-free unit test can construct a store without a game
// type. SimulationNetSync::setNeutralInput injects the real one into every store.
//
// ---------------------------------------------------------------------------
// SENTINEL + INITIAL-STATE CONTRACTS.
//
//   * `kNoInputCaptureTick` is NEVER a store key. `push` rejects it outright and
//     `find` refuses to look it up. This is defensive — the relay path only ever
//     carries real capture ticks — but the store must not be poisonable by a
//     future feeder, because the semantic layer above (T6) resolves a sentinel ref
//     to the game zero and must never see it become a successful lookup.
//     Reserving the value costs nothing: 2^32 ticks at 60 Hz is ~828 days.
//   * `!findLatest().valid` (nothing has ever arrived) MUST mean NO SCHEDULED
//     PROBE AT ALL, not a probe with a default `dA` of 0. Probing `find(N)` can
//     genuinely HIT for a LAN peer; T7's verify would then reject it on the stamp,
//     so it is not a correctness hole today — but "accidentally safe because a
//     later check catches it" is not a contract. T7 states and honours the skip.
//
// ---------------------------------------------------------------------------
// THREADING — GAME-THREAD WRITE / PHYSICS-THREAD READ. ACCEPTED DEBT.
// (Ruling 2026-08-04, option (a); the claims below were source-verified by the
//  2026-08-04 pre-dispatch review and are stated here as FACT, not as hope.)
//
// WRITER: `USimmableUpdateComponent::OnRep_RelayedInputRing` fires on the GAME
// thread and calls straight through to `populateRemoteInputCache` below.
// READER: `SimulationNetSync::collectInputAll` (T7) and
// `SimulationNetSync::collectResimInputAll` (T6, which relocated it off
// SimulationReconciliation) read on the PHYSICS
// thread under `bTickPhysicsAsync`.
//
// DO NOT COPY LocalInputCache's "NOT thread-safe; single-threaded by
// construction... both on the PHYSICS thread" claim along with the ring mechanics.
// That sentence is TRUE for the delay line (its push and its read are two
// statements inside one `collectInputAll` call) and FALSE for this type. A false
// threading contract is worse than none, and a reviewer should reject a copied one
// outright.
//
// WHY THIS IS ACCEPTED RATHER THAN FIXED HERE:
//   * IT IS INHERITED, NOT INTRODUCED. `OnRep_CorrectionState` is likewise
//     game-thread and calls `injectCorrectionState` DIRECTLY — no queue, no seam —
//     while the same two physics-thread readers read the correction cache. T2
//     recorded the same inheritance for `m_lastUsedInputs` and, after T8 retired
//     that map, for its surviving twin `m_lastUsedCaptureTicks`. This store widens
//     nothing.
//   * THE FAILURE MODE IS A TORN SLOT, NOT CONTAINER UB. Storage is a fixed-size
//     slot array (as `CorrectionCache` and `RemoteMoveQueue` both are): there is no
//     rehash to race, so this is emphatically NOT the situation that forced T10's
//     R2 restructuring. The worst case is one bad input, on one proxy, for one
//     tick.
//   * ITS WORST CONCRETE SHAPE, stated plainly rather than left abstract: a torn
//     FORWARD VECTOR can transiently read near-zero, producing one tick of
//     degenerate (or NaN) state on that proxy.
//   * AND IT IS CORRECTION-HEALED, INCLUDING ON A FRONTIER TICK. The heal is the
//     STATE anchor, not corrected-input replay: a torn read on a frontier tick
//     mispredicts, but at every-frame correction cadence that tick is itself
//     corrected shortly after — and the resim then re-reads the store, by which
//     time the write has long since completed (tears are transient). Hit detection
//     is server-side, so a degenerate proxy tick cannot adjudicate anything. The
//     ONLY escape is corrections STOPPING, which is the already-named
//     starving-wire / stale-hold class and already a recorded sparse-state
//     prerequisite.
//
// DEFERRED CLEANUP — RECORDED HERE, NOT DONE HERE. The proper fix is this
// codebase's own seam pattern: have the OnRep decode into an SPSC staging ring
// (game-thread producer) drained at the top of `collectInputAll` (physics-thread
// consumer), exactly as `RemoteMoveQueue` does the server-side GT->PT crossing and
// `PendingInputQueue` the client-side PT->GT one. That would make this store
// genuinely physics-thread-only (and would make the cribbed comment true). It is
// deliberately out of scope because fixing only the relay store while the
// correction cache retains the identical pattern buys little — the whole receive
// path should move together. Logged in three places so it cannot be lost: here,
// the Backlog's "Out of scope" section, and RelayDelaySpectrumDesign.md §8.6.
//
// ---------------------------------------------------------------------------
// ENGINE-AGNOSTIC. STL + other OGSimulation core headers only; no UE types, no
// engine headers. The ring it is fed from arrives as a template parameter
// satisfying the codec's BUFFER CONCEPT, which is what lets the pure-C++
// Low-Level-Tests drive the REAL ingest path against a std::vector-backed buffer.
//
// NAMESPACE NOTE: global namespace, matching the rest of the OGSim core (same note
// as LocalInputCache / ConnectionTierTable / SimulatableList). The design
// corpus writes `ogsim::` but no such namespace exists in this tree.
// ---------------------------------------------------------------------------

// Slot count of a default-constructed RemoteInputCache.
//
// Declared as a FREE constant — exactly like `kLocalInputCacheCapacityTicks`,
// and for the same reason: config-layer code must be able to derive bounds from it
// without naming an arbitrary `InputT` to reach a static member of a class
// template.
//
// THIS CONSTANT IS LOAD-BEARING FOR THE RELAY DELAY FLOOR. It is one of the two
// inputs to `relayDelayFloorHardCapTicks` (Network/ConnectionTierTable.h) — GO
// READ THE DERIVATION THERE rather than trusting a formula mirrored here. In one
// sentence: an entry must stay resident until the frontier passes
// `captureTick + dA + rollbackWindowHardCap`, and is evicted at about
// `captureTick + capacity + wire`, so `dA <= capacity - rollbackWindowHardCap`.
// Lower this number and the floor's hard cap must follow it down, or a configured
// floor silently schedules reads against entries that have already been evicted.
inline constexpr std::size_t kRemoteInputCacheCapacityTicks = 64u;

// ---------------------------------------------------------------------------
// Formerly RelayedInputStore (renamed 2026-08-16, RN-14).
//
// WIPE ASYMMETRY — THE SIGNAL THE OLD, ASYMMETRIC NAMES WERE CARRYING.
// This cache is SENDER-KEYED and is deliberately NOT wiped by
// SimulationNetSync::wipeAllForResync — see "THE WIPE DIVERGES" above and the
// non-wipe comment at the wipeAllForResync call site. Its counterpart,
// LocalInputCache (Network/LocalInputCache.h), is CLOCK-KEYED and IS wiped:
// its keys are the local prediction clock's tick numbers, which a hard resync
// invalidates. `LocalInputCache` / `RemoteInputCache` are now symmetric
// names; wiping this cache too would leave a proxy blind for a window after
// every resync (the 2026-08-04 failure) — restated here because the symmetric
// names no longer carry that warning on their own.
// ---------------------------------------------------------------------------

template <typename InputT>
class RemoteInputCache
{
public:
    // The result of the `findLatest()` DERIVATION. Returned by value because it is
    // an answer computed from the slots, not a handle into them — there is
    // deliberately no way to obtain a reference to "the latest slot" and hold it.
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

    // Replace the injected game zero after construction — the same
    // order-independence LocalInputCache::setNeutralInput provides, so the
    // composition root need not order itself before every registration.
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
    //
    // LAST-WINS on an already-resident capture tick, matching both
    // LocalInputCache::push and the wire codec's writeLatest: the codec
    // explicitly permits REWRITING a resident tick with a fresher `dA` (a re-stamp
    // after a delay change), so the store must accept the same rewrite rather than
    // treating the first arrival as immutable. This is also what makes the ingest
    // idempotent, which is what lets `populateRemoteInputCache` re-consume the
    // whole ring on every arrival instead of diffing.
    //
    // Returns false — changing nothing — only for `kNoInputCaptureTick`, which is
    // never a key here (see the SENTINEL contract above).
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

    // PURE HIT/MISS lookup — the store NEVER invents a neutral on this path.
    //
    // That is the whole difference from LocalInputCache::at(), and it is what
    // keeps a miss VISIBLE to T7's three-step ladder (probe -> verify -> fallback)
    // and to T6's resolution table, instead of being silently swallowed as a
    // neutral that both layers would then be unable to distinguish from a real
    // neutral-content input. On a miss `outDA` and `outInput` are left untouched,
    // mirroring the codec's findEntry.
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

    // THE DERIVATION. The newest relayed entry, computed from the slots on every
    // call. `valid == false` iff nothing has ever arrived for this character.
    //
    // This is what `dLatest` and `lastKnown` MEAN (`.dA` and `.input`); neither is
    // a field. See the ruling in the header block above before adding a memo.
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

    // The terminal value of T7's ladder: the newest arrived input once ANYTHING
    // has arrived, otherwise the injected game zero.
    //
    // ARGUMENT-LESS BY DESIGN — the stale-input hold rule is deferred; see the
    // header block for why deferring is a no-regression choice and what the future
    // rule looks like. Do not add a `frontierTick` parameter here "for later".
    //
    // Shares `latestSlotIndex` with `findLatest()` so the two cannot drift, and
    // returns a reference rather than a copy because it is on the per-tick proxy
    // path.
    const InputT& fallback() const
    {
        bool              found = false;
        const std::size_t index = latestSlotIndex(found);
        return found ? m_slots[index].input : m_neutral;
    }

    // [og-netcode-v2-input-relay T20] THE RESIDENT SPAN — oldest, newest, count.
    //
    // WHAT IT IS FOR, because the distinction it enables is the whole of T20's
    // Probe B. `find(probeTick)` answering false says a scheduled read MISSED; it
    // does not say WHY, and the three whys have three different remedies:
    //
    //   probeTick inside [oldest, newest] but absent  A COVERAGE HOLE. The tick was
    //                                                 produced by the sender and
    //                                                 clobbered before it could be
    //                                                 replicated (the relay ring is
    //                                                 replace-latest at depth 1).
    //                                                 Remedy: raise the depth.
    //   probeTick > newest                            The receiver is asking for a
    //                                                 capture that has not been
    //                                                 produced or has not landed yet.
    //                                                 DEPTH IS IRRELEVANT to this
    //                                                 class — it is the delay deficit.
    //   probeTick < oldest                            Asking older than the store
    //                                                 retains: clock misalignment, or
    //                                                 capacity.
    //
    // THE SPAN IS THE STORE'S, NOT THE RING'S, and conflating the two would
    // invalidate the classification. The ring carries `depth` entries per
    // replication; this store ACCUMULATES up to `capacity()` arrivals. That is
    // exactly why an in-span hole is meaningful even at depth 1 — the store's span
    // grows across arrivals while each individual arrival carried only one entry.
    //
    // ONE SCAN, same shape as `findLatest()` and `residentCount()`, and deliberately
    // not memoized for the reason the header block gives for `findLatest()`. `newest`
    // is by construction identical to `findLatest().captureTick`; both are the max
    // over occupied slots.
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

    // ONE-SHOT gate for the wire-version-mismatch log.
    //
    // Returns true exactly once per store, i.e. exactly once PER CHARACTER (=
    // per replicated component) PER SESSION — which is the cadence the fence
    // requires, because an incompatible peer re-replicates its ring forever and an
    // ungated log would fire on every single replication. The latch lives on the
    // store because the store is the only per-character object the ingest path
    // has; `populateRemoteInputCache` itself is stateless and logger-free (core
    // containers do not log — the caller owns the logger, exactly as
    // RemoteMoveQueue's too-far-future drop is warned by SimulationNetSync).
    bool shouldLogVersionMismatchOnce()
    {
        if (m_versionMismatchLogged)
        {
            return false;
        }
        m_versionMismatchLogged = true;
        return true;
    }

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
    //
    // Plain `>` on the capture tick is correct because the relay stream is
    // MONOTONIC in capture tick by construction (the server relays only on
    // `parked && acceptedNew`, so an out-of-order-older input is never sent —
    // RelayDelaySpectrumDesign.md §5.3a). Ring wraparound of the tick COUNTER
    // itself is out of scope for the same reason the sentinel is safe to reserve:
    // 2^32 ticks at 60 Hz is ~828 days of continuous session.
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

    // See shouldLogVersionMismatchOnce().
    bool m_versionMismatchLogged = false;
};

// ---------------------------------------------------------------------------
// populateRemoteInputCache — THE ingest, in one place, carrying the wire fence.
//
// Lives in core beside the store (the RelayedInputRingCodec precedent) so the
// UE-free Low-Level-Tests exercise the REAL path rather than a mirror of it: the
// UE side does nothing but call this with the freshly-replicated ring.
//
// ---------------------------------------------------------------------------
// THE WIRE-VERSION FENCE. T5 IS THE FIRST READER — NOBODY CHECKS IT TODAY.
//
// T1 shipped `relayedInputRing::kWireFormatVersion` and
// `getWireFormatVersionOnWire()`, but `FRelayedInputRing::NetSerialize` only
// LENGTH-checks. Entry stride is a COMPILE-TIME constant per `InputType`, so a
// layout change makes an old peer read arbitrary bytes as `captureTick` / `dA` /
// `input`: plausible-looking garbage capture ticks inserted as STORE KEYS and then
// applied to a proxy. Silent corruption, no crash. Hence the version is checked
// BEFORE ANY STRUCTURAL READ — `entryCount` lives in the same header and is
// equally untrustworthy on a mismatch, and a stride mismatch would send
// `forEachEntry` off the end into the buffer's fatal bounds `checkf`.
//
// Three-way:
//   version 0  -> never written / never replicated. No-op, and deliberately NO
//                 log: the codec writes its header lazily, so an empty ring reads
//                 0 and can never false-positive as a mismatch.
//   version ==
//   kWireFormat -> consume.
//   anything else -> DROP THE WHOLE RING. Consume nothing. The caller logs, once
//                 per component per session (see shouldLogVersionMismatchOnce).
//
// DROP AT CONSUME — DO NOT `Ar.SetError()`, AND DO NOT "FIX" THIS TO MATCH THE
// LENGTH CHECK. The two are asymmetric on purpose. A bad LENGTH prefix means the
// property cannot know how many bytes to consume, so the archive position
// desynchronizes and every subsequent property in that bunch parses garbage —
// failing the bunch is then mandatory, and that is exactly what T1 shipped. A
// VERSION mismatch is discovered AFTER `NetSerialize` completed cleanly: the
// byte count was consumed exactly, the archive is still in sync, and unrelated
// properties in the same bunch deserialize fine. Failing the bunch there would
// destroy them for no integrity gain. And, concretely: at THIS site — the OnRep /
// populate path — `SetError` is not even available, because serialization has
// already completed by the time we get here.
//
// THE UNSTATED INVARIANT THAT ARGUMENT RESTS ON, now stated (AM-4): the u16
// LENGTH-PREFIX FRAMING is VERSION-INVARIANT FOREVER. Only the payload layout
// BEHIND the prefix may change across wire versions; the prefix itself may never.
// Without that, "the byte count is correct on a mismatch" is unfounded and the
// whole drop-not-SetError asymmetry collapses.
//
// ---------------------------------------------------------------------------
// INGEST: RE-CONSUME EVERYTHING, NO DIFFING. Every resident entry is pushed, and
// `push` is idempotent by capture tick, so re-consuming an entry that is already
// resident simply rewrites it (which is also how a re-stamp lands). Cost is
// O(depth) — 1 today, <= kMaxDepth ever. The identical code stays correct
// unchanged when depth rises, which is the point: §8.2 expects exactly that.
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
    // [T34 loss-counter fix] HOW MANY CAPTURE TICKS THIS ARRIVAL ACTUALLY ADDED.
    //
    // `entriesIngested` is the count of entries PUSHED, and under the re-consume
    // ingest above that includes every already-resident entry the ring carried
    // again. This field counts only the entries whose capture tick was NOT
    // resident when the arrival began — i.e. the NEW COVERAGE this arrival
    // delivered — and it is what `RelayArrivalProbe::noteArrival` charges loss
    // against.
    //
    // WHY THE DISTINCTION IS LOAD-BEARING, and it is the whole defect this field
    // exists to close: under the retired replace-latest write path one arrival
    // carried exactly one new capture tick, so "the newest watermark advanced by
    // g" and "g-1 ticks were lost" were the same statement. Under flush-on-poll
    // ONE ARRIVAL CARRIES THE WHOLE BURST, so a 2-entry burst advances the
    // watermark by 2 while losing nothing — and a probe that assumes 1 charges
    // the burst RATE as loss. The measured Run 1 consequence: ~120 per mille
    // reported against a ~11 per mille pass condition, on a flush that was
    // working perfectly.
    //
    // RE-DELIVERY IS NOT COVERAGE. A ring that carries a tick this store already
    // holds (a dA re-stamp, or the flush republishing an entry that had not yet
    // been evicted from the ring) adds nothing, so it is deliberately NOT counted
    // — counting it would let a stalled sender re-delivering the same entry look
    // like a sender delivering new ones.
    //
    // FREE: `has()` is an O(1) slot probe and the ingest already visits every
    // entry, so this costs one comparison per entry and no second walk.
    // -----------------------------------------------------------------------
    std::uint8_t              newCaptureTicksIngested = 0u;

    // [T19] THE NEWEST CAPTURE TICK THIS ARRIVAL CARRIED — the replication-cadence
    // probe's entire data source (Network/RelayReadProbe.h, probe 2).
    //
    // WHY IT LIVES HERE rather than being re-derived at the call site: the ingest
    // already walks every entry, so the maximum is free, whereas a caller would
    // have to either walk the ring a second time or call `store.findLatest()` —
    // and `findLatest()` is the wrong answer. It scans the STORE, which holds up
    // to 64 accumulated capture ticks, so on an arrival that carried nothing new
    // it would keep reporting the previous arrival's tick and the cadence gap
    // would silently read 0 forever. This field is the newest tick THIS RING
    // carried, which is the quantity "ticks between successful relay-ring OnReps"
    // is actually about.
    //
    // `valid` is false on NeverWritten and VersionMismatch (nothing was read), and
    // also on the degenerate case of a version-matching ring whose every entry was
    // rejected by `push` — which is why it is a separate flag rather than a
    // sentinel value: tick 0 is a perfectly ordinary capture tick.
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
            // [T34 loss-counter fix] Asked BEFORE the push, because the push is
            // what makes it resident. See `newCaptureTicksIngested`.
            const bool wasAlreadyResident = store.has(captureTick);

            if (store.push(captureTick, dA, input))
            {
                ++report.entriesIngested;
                if (!wasAlreadyResident)
                {
                    ++report.newCaptureTicksIngested;
                }

                // [T19] Track the maximum over the entries that were ACCEPTED, so
                // a rejected sentinel can never become the reported newest.
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
