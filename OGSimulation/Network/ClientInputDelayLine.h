#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// ClientInputDelayLine<InputT> — the CLIENT-side counterpart of
// ServerInputDelayQueue. (Stage 5 / D5.2, T9 parts 3+4.)
//
// WHAT IT IS. A small ring of the client's OWN input CAPTURES, keyed by the tick
// they were captured at. The client integrates `at(currentTick - effectiveDelay)`
// so that the input it predicts with is the same input the server will apply at
// that tick — the server having parked the same capture in its
// ServerInputDelayQueue for the same number of ticks. Both ends delay by an
// identical count derived from an identical tier (Option A: the server derives
// the tier, the client consumes it via ReplicatedTierConsumer), which is what
// makes the two ends agree by construction rather than by coincidence.
//
// ---------------------------------------------------------------------------
// WHY THIS IS A SEPARATE STRUCTURE AND NOT THE CORRECTION CACHE.
// (Lead ruling, 2026-07-20 — this class exists BECAUSE of that ruling.)
//
// T9 was originally specified to use the StateCorrectionCache input buffer as
// the delay line: store the capture at slot T, read the slot `effectiveDelay`
// back. That is a silent desync, for two independent reasons:
//
//   1. THE SERVER WRITES THAT SAME BUFFER WITH A DIFFERENT MEANING.
//      SimulationNetSync::sendCorrectionAll replicates the input the server
//      APPLIED, stamped with the tick it applied it at; the client lands it via
//      receiveCorrectionInput -> StateCorrectionCache::insertCorrectionInput.
//      So slot T means "the input APPLIED at tick T" — which, under the server's
//      delay, is the client's capture from tick T-delay. Storing the capture at
//      slot T instead would put two different values in one slot and make every
//      correction comparison in the delay window wrong.
//
//   2. RESIMULATION READS THOSE SLOTS WITH NO OFFSET.
//      SimulationReconciliation::collectResimInputAll(simTick) reads
//      `cache.getInput(cache.getCacheIndex(simTick))` and integrates it
//      directly. Captures in slot T would make every resim replay tick T with
//      the wrong input, and a tier change inside a rollback window would replay
//      with a MIXTURE of offsets.
//
// Both invariants hold iff slot T keeps meaning "input applied at tick T". So
// the cache continues to store the ALREADY-DELAYED input (that is what
// pushPredictionInput receives), and the raw captures live here instead.
//
// PendingInputQueue cannot serve as the delay line either: it retains only
// `redundancyDepthTicks` (3) entries while the worst tier delay is 4, and
// wipeAllForResync clears it outright.
//
// ---------------------------------------------------------------------------
// WHAT THE OUTBOUND RPC CARRIES — DO NOT "SIMPLIFY" THIS.
//
// The capture pushed into PendingInputQueue (and therefore onto the wire) is the
// ORIGINAL, UNDELAYED capture stamped at the CURRENT tick. The server applies
// its own delay to what it receives. If the client sent an already-delayed
// input, the server would delay it a second time and the two ends would diverge
// by exactly `delay` ticks. Only the integrator and the correction cache see
// the delayed value.
//
// ---------------------------------------------------------------------------
// THE NEUTRAL INPUT (T9 part 4 — the [0, effectiveDelay) window).
//
// For the first `effectiveDelay` ticks of a session — and for the equivalent
// window after a hard resync wipes the line — the read lands on a tick that was
// never captured. `at()` answers with the NEUTRAL input for those.
//
// The neutral is INJECTED, not value-initialised, and that distinction is
// load-bearing: the game's zero input is NOT `InputT{}`.
// `simulatableBrawler::getZeroPlayerInput()` (SimulatableBrawlerTypes.h:94)
// builds forward vectors of (0,0,1); a value-initialised PlayerInput would carry
// a (0,0,0) forward vector into normalisation. The composition root injects the
// game's real zero input (see ASimulationManagerUImpl::BeginPlay). The default
// is `InputT{}` purely so an engine-free unit test can construct the line
// without a game type.
//
// ---------------------------------------------------------------------------
// THREADING. NOT thread-safe; single-threaded by construction. Both the push
// and the read happen inside SimulationNetSync::collectInputAll, i.e. both on
// the PHYSICS thread, one after the other in the same call. Nothing else touches
// this container. The one value that genuinely crosses the game/physics boundary
// is the effective delay itself, which SimulationNetSync publishes as a lone
// std::atomic<int32_t> — see the note on setClientEffectiveInputDelayTicks.
//
// ENGINE-AGNOSTIC. STL only; no UE types, no engine headers, no other
// OGSimulation headers (this container needs no config).
//
// NAMESPACE NOTE: global namespace, matching the rest of the OGSim core (same
// note as ConnectionTierTable / ServerInputDelayQueue / SimulatableList). The
// design corpus writes `ogsim::` but no such namespace exists in this tree.
// ---------------------------------------------------------------------------

template <typename InputT>
class ClientInputDelayLine
{
public:
    // Capacity must comfortably exceed the largest configurable tier delay
    // (`TimeConfig::rttTierInputDelays`, worst case 4 today). 64 is chosen to
    // also exceed the rollback window, so a capture is still resident for any
    // tick a resim could plausibly revisit. Capacity is a plain slot count, NOT
    // a tick modulus the caller has to reason about — `at()` validates the
    // stored tick, so an evicted tick reads as absent rather than as a stale
    // neighbour.
    static constexpr std::size_t kDefaultCapacityTicks = 64u;

    explicit ClientInputDelayLine(InputT neutralInput = InputT{},
                                  std::size_t capacity = kDefaultCapacityTicks)
        : m_neutral(std::move(neutralInput))
        , m_slots(capacity == 0u ? kDefaultCapacityTicks : capacity)
    {
    }

    // Replace the neutral input after construction. Exists so the composition
    // root can inject the game's zero input without having to order itself
    // before every registration — a delay line built with the default neutral
    // can be corrected at any point before the first read.
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

    // Record the capture taken at `captureTick`.
    //
    // LAST-WINS, unlike ServerInputDelayQueue::enqueue's first-wins. The two
    // rules are not inconsistent — they guard different things. The server's
    // first-wins protects against a redundancy bundle RE-SENDING an input the
    // simulation may already have consumed. Here there is no retransmission: the
    // only writer is the local input provider, called exactly once per tick, and
    // the only way the same tick is written twice is a Stall/Skip re-entry where
    // the freshest capture is the one that should win.
    void push(std::int32_t captureTick, const InputT& input)
    {
        if (captureTick < 0)
        {
            return;     // pre-session ticks are represented by absence, not by a slot
        }

        Slot& slot = m_slots[slotIndexFor(captureTick)];
        slot.tick     = captureTick;
        slot.occupied = true;
        slot.input    = input;
    }

    // True iff `tick` is still resident. Negative ticks are never resident —
    // that is the [0, effectiveDelay) window, and it is a legitimate state, not
    // an error.
    bool has(std::int32_t tick) const
    {
        if (tick < 0)
        {
            return false;
        }
        const Slot& slot = m_slots[slotIndexFor(tick)];
        return slot.occupied && slot.tick == tick;
    }

    // The capture taken at `tick`, or the NEUTRAL input if that tick was never
    // captured (session start) or has been evicted (older than `capacity()`).
    //
    // Returning the neutral rather than asserting is deliberate: the pre-session
    // window is reached on every single session start, and after every hard
    // resync, by design.
    const InputT& at(std::int32_t tick) const
    {
        if (!has(tick))
        {
            return m_neutral;
        }
        return m_slots[slotIndexFor(tick)].input;
    }

    // Forget every capture, keeping the neutral. Called from
    // SimulationNetSync::wipeAllForResync: after a hard resync the prediction
    // tick jumps, so captures keyed to the pre-resync clock describe ticks that
    // no longer mean what they meant. Dropping them re-enters the neutral-filled
    // window for `effectiveDelay` ticks, which is the same well-defined state as
    // session start.
    void clear()
    {
        for (Slot& slot : m_slots)
        {
            slot.occupied = false;
            slot.tick     = -1;
        }
    }

    // Resident capture count. Tests / telemetry only.
    std::size_t residentCount() const
    {
        std::size_t count = 0u;
        for (const Slot& slot : m_slots)
        {
            count += slot.occupied ? 1u : 0u;
        }
        return count;
    }

private:
    struct Slot
    {
        std::int32_t tick     = -1;
        bool         occupied = false;
        InputT       input{};
    };

    std::size_t slotIndexFor(std::int32_t tick) const
    {
        return static_cast<std::size_t>(tick) % m_slots.size();
    }

    InputT            m_neutral;
    std::vector<Slot> m_slots;
};

// ---------------------------------------------------------------------------
// resolveDelayedInput — THE offset-read rule, in one place.
//
// Production (SimulationNetSync::collectInputAll, provider branch) and the
// Catch2 client-delay integration suite both call THIS function rather than each
// re-deriving `tick - delay`. That is the point: a test that merely mirrored the
// production expression could drift away from it silently, which is exactly the
// failure mode the server-side integration suite documents.
//
// `effectiveDelay <= 0` returns the live capture UNTOUCHED rather than reading
// `at(currentTick)`. Not an optimisation — a correctness requirement. A Stall
// tick does not push into the line, so a zero-delay read of `at(currentTick)`
// could answer with the neutral on exactly the ticks that must keep the live
// input. Zero delay must be indistinguishable from the pre-T9 behaviour.
// ---------------------------------------------------------------------------
template <typename InputT>
const InputT& resolveDelayedInput(const ClientInputDelayLine<InputT>& line,
                                  std::int32_t currentTick,
                                  std::int32_t effectiveDelay,
                                  const InputT& liveCapture)
{
    if (effectiveDelay <= 0)
    {
        return liveCapture;
    }
    return line.at(currentTick - effectiveDelay);
}
