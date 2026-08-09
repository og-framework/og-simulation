#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include "OGAssert.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <functional>
#include <vector>
#include <optional>
#include <limits>
#include <bitset>
#include "glm/vec3.hpp"
#include <glm/gtc/quaternion.hpp>
#include "OGSimulation/SimulationTimeContext.h"
#include "OGSimulation/SimulationComposite.h"
#include "OGSimulation/CorrectionStateBufferCodec.h"
#include "OGSimulation/PCTimeManagement/TimeConfig.h"

// Checksum support for the StateCorrectionCache 4-method external API.
//
// crc32 is the standard reflected CRC-32 (polynomial 0xEDB88320, init 0xFFFFFFFF,
// final XOR 0xFFFFFFFF) implemented via a 256-entry lookup table built once on
// first use. It is declared `inline` (not `static`) so a translation unit that
// includes this header without ever instantiating compute_checksum does not emit
// an -Wunused-function warning on the standalone GCC/Clang/NDK build paths.
//
// ChecksumByteBuffer is a minimal byte sink exposing the same writeToBuffer<T> /
// readFromBuffer<T> template surface as the UE-side FSimulationStateSyncBuffer, so
// the existing writeToSyncedBuffer / writeCompositeToSyncedBuffer serializers can
// target it without any UE dependency. compute_checksum CRCs the serialized bytes
// (padding-free, deterministic) rather than the raw object, which keeps the hash
// stable across compilers/architectures for the cross-arch determinism harness.

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t length)
{
	static const std::array<std::uint32_t, 256> table = [] {
		std::array<std::uint32_t, 256> t{};
		for (std::uint32_t i = 0; i < 256; ++i)
		{
			std::uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			t[i] = c;
		}
		return t;
	}();

	std::uint32_t crc = 0xFFFFFFFFu;
	for (std::size_t i = 0; i < length; ++i)
		crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

struct ChecksumByteBuffer
{
	std::vector<std::uint8_t> bytes;

	// Scalar/trivially-copyable field sink used by writeToSyncedBuffer descriptors.
	template <typename T>
	void writeToBuffer(std::uint32_t offset, const T& value)
	{
		static_assert(std::is_trivially_copyable_v<T>,
			"ChecksumByteBuffer::writeToBuffer requires a trivially-copyable field type");
		growTo(static_cast<std::size_t>(offset) + sizeof(T));
		std::memcpy(bytes.data() + offset, &value, sizeof(T));
	}

	template <typename T>
	T readFromBuffer(std::uint32_t offset) const
	{
		T value{};
		std::memcpy(&value, bytes.data() + offset, sizeof(T));
		return value;
	}

	void writeRaw(std::uint32_t offset, const std::uint8_t* src, std::size_t length)
	{
		growTo(static_cast<std::size_t>(offset) + length);
		std::memcpy(bytes.data() + offset, src, length);
	}

	const std::uint8_t* data() const { return bytes.data(); }
	std::size_t size() const { return bytes.size(); }

private:
	void growTo(std::size_t requiredSize)
	{
		if (bytes.size() < requiredSize)
			bytes.resize(requiredSize, std::uint8_t(0));
	}
};

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
#pragma optimize( "", off )

// ---------------------------------------------------------------------------
// [og-netcode-v2-input-relay T24] CorrectionInsertVerdict — what
// `tryInsertingCorrectState` DECIDED, handed back to a caller that knows WHO it
// decided it about.
//
// THIS ADDS NO COMPUTATION. `predictionWasCorrect` is the existing
// `m_stateBuffer[cacheIndex].isSimilarTo(state)` verdict, verbatim, already
// computed on every correction and already stored in `m_predictionWasCorrect`.
// The only thing that changes is that it can now leave the function.
//
// WHY IT LEAVES AS AN OUT-POINTER RATHER THAN A RETURN VALUE OR A LOG LINE.
//   * The cache is deliberately ID-AGNOSTIC — it holds one character's ticks and
//     has never known which character it belongs to. The T24 acceptance criterion
//     needs the verdict attributed by `id` AND by class (remote proxy vs locally
//     predicted, decided by provider-presence), and NEITHER of those facts exists
//     in this class or could be handed to it without giving it an identity it has
//     no other use for. So the verdict has to travel up to the site that already
//     knows both, which is the OnRep-bound correction callback in
//     `SimulationNetSync::registerPredictionOwner`.
//   * A DEFAULTED out-pointer leaves every existing caller — the four production
//     paths and every `[CorrectionCache]` / `[AppliedCaptureTickSlot]` case —
//     byte-identical, which is what makes this task behaviour-neutral by
//     construction rather than by inspection. Same trick T19 used to widen
//     `resolveScheduledRelayedInput` without touching a single existing call
//     site, and the same shape `find()` already uses locally.
//
// The cache's own existing log line is UNCHANGED and stays where it is. It is
// untagged, so it still falls to the LogOG fallback and is still suppressed at
// the shipped default — retagging it would have moved a line that cannot carry
// an id anyway, and would have perturbed the log-gating cases that count it.
// ---------------------------------------------------------------------------
struct CorrectionInsertVerdict
{
    // False when the correction's tick had no slot in the cache window and the
    // correction was DISCARDED. No comparison happened, so `predictionWasCorrect`
    // is meaningless — a discarded correction must never be counted as either an
    // agreement or a disagreement, because it produced neither.
    bool   landed               = false;

    // The existing `isSimilarTo` verdict. Meaningful only when `landed`.
    bool   predictionWasCorrect = false;

    // The correction's tick, echoed back so the caller's log line needs no second
    // decode of the wire buffer. Set on BOTH paths.
    uint32 tick                 = 0u;
};

// [og-netcode-v2-input-relay T16] WHAT A CACHE SLOT IS, AFTER THE INPUT COLUMN
// WAS RETIRED.
//
//     slot i  ==  { tick, STATE at that tick, the APPLIED-CAPTURE-TICK REF for
//                   that tick, plus the three bookkeeping bits }
//
// There is NO input value in a slot any more. `m_inputBuffer`, `getInput`,
// `getLatestInput` and `pushPredictionInput` are all gone: T6 re-pointed the
// resim read to identity resolution, T7 re-pointed the remote-proxy viz to the
// relay store, T8 removed the SERVER->CLIENT correction-input channel outright,
// and T15 re-sourced the motion matcher to the client's raw capture history. The
// column then had no consumer for any character, local or remote, and storing a
// value nothing reads is how a stale second copy of the truth gets re-discovered
// and trusted years later.
//
// ANY FUTURE CODE LOOKING FOR AN INPUT VALUE IN THE CORRECTION CACHE IS LOOKING
// FOR SOMETHING THAT NO LONGER EXISTS BY DESIGN. The three live sources, by
// question asked:
//   * "what did the local player capture at tick t?"   -> ClientInputDelayLine
//     (capture-tick keyed; SimulationNetSync owns one per provider-present id).
//   * "what did a REMOTE player send for capture t?"   -> RelayedInputStore, via
//     SimulationNetSync::getLastRelayedInput / resolveScheduledRelayedInput.
//   * "which capture did the AUTHORITY apply at tick t?" -> the ref that lives in
//     THIS slot, read through getAppliedCaptureTick / the classified
//     SimulationReconciliation::getAppliedCaptureTickRef.
// The ref is the join key that replaced the value: 4 bytes naming an input,
// instead of a copy of one. Re-adding the column would re-create exactly the
// application-tick-vs-capture-tick ambiguity T15 was filed to fix.
//
// The InputType template parameter DELIBERATELY SURVIVES the column: it is still
// named by IntegrateFn and by advance_frame, i.e. by the externally-driven
// 4-method API the determinism harness runs on. The cache no longer STORES an
// input; it still INTEGRATES with one.
template <typename StateType, typename InputType>
class StateCorrectionCache
{
public:
	static constexpr size_t StateBufferSize = 60;
	static constexpr size_t InvalidCacheIndex = 1337;

	// Integrate functor for the externally-driven advance_frame() path:
	// (tick, prevState, input) -> newState. The cache itself owns no integration
	// logic; the harness injects it via the 2-arg constructor below.
	using IntegrateFn = std::function<StateType(uint32, const StateType&, const InputType&)>;

	StateCorrectionCache(std::function<void(const char*)> logger)
		: m_stateBuffer()
		, m_tickBuffer()
		, m_logger(std::move(logger))
		, m_integrateFn()
	{
		m_tickBuffer.fill(0);
		m_appliedCaptureTickBuffer.fill(kNoInputCaptureTick);
	}

	// Overload that injects an integrate functor so advance_frame() can
	// drive one externally-triggered sim step. The single-arg constructor stays;
	// caches built with it must not call advance_frame() (it OG_CHECK-fails).
	StateCorrectionCache(std::function<void(const char*)> logger, IntegrateFn integrateFn)
		: m_stateBuffer()
		, m_tickBuffer()
		, m_logger(std::move(logger))
		, m_integrateFn(std::move(integrateFn))
	{
		m_tickBuffer.fill(0);
		m_appliedCaptureTickBuffer.fill(kNoInputCaptureTick);
	}

	uint32 getCacheIndex(uint32 tick) const
	{
		auto it = std::find(m_tickBuffer.begin(), m_tickBuffer.end(), tick);
		if (it != m_tickBuffer.end())
		{
			return std::distance(m_tickBuffer.begin(), it);
		}
		else
		{
			return InvalidCacheIndex;
		}
	}

	const StateType& getState(uint32 cacheIndex) const
	{
		OG_CHECK(cacheIndex < StateBufferSize, "trying to access state with bad cacheIndex");
		return m_stateBuffer[cacheIndex];
	}

	StateType& editState(uint32 cacheIndex)
	{
		OG_CHECK(cacheIndex < StateBufferSize, "trying to access state with bad cacheIndex");
		return m_stateBuffer[cacheIndex];
	}

	// [og-netcode-v2-input-relay T16] `getInput(cacheIndex)` IS GONE with the input
	// column. It also carried the tick-0 phantom: `m_tickBuffer.fill(0)` in both
	// constructors means every UNWRITTEN slot claims tick 0, so
	// `getInput(getCacheIndex(0))` resolved to slot 0 and then OG_CHECK-failed on
	// that slot's empty optional (calling value() on it in an unchecked build).
	// Nothing can trip that any more, because there is no optional to be empty.
	// The replacement for "which input produced tick T" is the ref immediately
	// below — read it, then ask the delay line or the relay store for the value.

	// [og-netcode-v2-input-relay T4 / design D3] THE PER-TICK JOIN KEY.
	//
	// The capture tick of the input the AUTHORITY applied at this slot's tick, as
	// carried by the correction that landed here (correctionStateBufferCodec's
	// second wire field), or kNoInputCaptureTick when no ref is known — which
	// covers three distinct situations that all mean the same thing to a reader:
	// the slot has never received a correction; the correction said the authority
	// substituted an input (RemoteMoveQueue underrun, the D1 sentinel); or the
	// slot was recycled by the prediction ring and its old ref retired with it.
	//
	// WHY IT LIVES IN THE SLOT and not in one scalar per character: T6 resolves
	// remote input for EVERY tick it resimulates, so it needs the ref that
	// belonged to each of those ticks, not the newest one. It is stored parallel
	// to the state it corrects — slot T's ref describes what the authority applied
	// AT tick T, with no offset, which is what lets resim read it straight.
	//
	// [T16] This ref is now the ONLY input-related thing a slot carries; the input
	// COLUMN it used to sit beside is retired. See the class header block.
	uint32 getAppliedCaptureTick(uint32 cacheIndex) const
	{
		OG_CHECK(cacheIndex < StateBufferSize, "trying to access applied capture tick with bad cacheIndex");
		return m_appliedCaptureTickBuffer[cacheIndex];
	}

	// [og-netcode-v2-input-relay T6] Has an authoritative correction landed in this
	// slot? Set by tryInsertingCorrectState, cleared by pushPredictionTick (ring
	// recycle), save_snapshot (fresh slot) and wipeCache (resync) — i.e. exactly
	// alongside m_appliedCaptureTickBuffer's own retirement points.
	//
	// WHY T6 NEEDS IT, given getAppliedCaptureTick already exists. The sentinel is
	// AMBIGUOUS on its own: `kNoInputCaptureTick` in a slot means either "a
	// correction landed and the authority named no capture" (the D1 underrun — T6
	// resolves that to the game zero) or "no correction has landed here at all"
	// (a prediction-frontier tick, or a tick whose correction was never replicated
	// because the net update rate is below the sim rate — T6 must RE-DERIVE those,
	// not zero them). Only this bit separates the two, and getting it wrong would
	// replay game-zero over roughly every uncorrected resim tick.
	bool containsCorrectTick(uint32 cacheIndex) const
	{
		OG_CHECK(cacheIndex < StateBufferSize, "trying to access correction flag with bad cacheIndex");
		return m_containsCorrectTick.test(cacheIndex);
	}

	uint32 getPredictionTick() const
	{
		//find the highest tick in the buffer
		auto maxIt = std::max_element(m_tickBuffer.begin(), m_tickBuffer.end());

		if (maxIt == m_tickBuffer.end())
			return 0;

		return *maxIt;
	}

	// -----------------------------------------------------------------------
	// Correction-miss log gating (Stage 5 / Task 18)
	// -----------------------------------------------------------------------
	// A correction whose tick has no slot in this cache is DISCARDED. That is a
	// routine, expected, self-healing outcome on three known-benign paths, all
	// characterised from the 2026-07-20 dedicated-server PIE session
	// (`impl/research_correction_discards.md`):
	//
	//   1. Freshly registered remote proxy — the per-simulatable cache exists but
	//      pushPredictionTick has never been called, so the tick buffer is still
	//      all-zero and EVERY correction misses for a few ticks until the proxy
	//      joins the resim loop.
	//   2. Connect transient — the client's prediction frontier has not yet
	//      converged on authority; corrections land ahead of or behind it until
	//      the clock settles (or HardResync fires).
	//   3. Post-Skip holes — a graduated Skip advances the frontier by two,
	//      leaving a gap that backfillSkippedTick does not always cover.
	//
	// Reconciliation anchors on the newest LANDED correction rather than on a
	// specific tick, so a miss costs delayed reconciliation rather than lost
	// reconciliation.
	//
	// ⚠ [og-netcode-v2-input-relay T39] THE COST IS NO LONGER "AT MOST ONE TICK",
	// AND CORRECTIONS ARE NO LONGER AN UNTHROTTLED PER-TICK STREAM. Both clauses
	// were true when this block was written and both are now false:
	// `SimulationNetSync::sendCorrectionAll` writes `TimeConfig::correctionRotationK`
	// characters' state buffers per tick, round-robin, so each character is
	// corrected at `tickFrequency * K / N` Hz — 60 Hz at N <= K, and 20 Hz at the
	// shipped K = 2 with six characters. A miss therefore costs up to one ROTATION
	// SLOT of delayed reconciliation: bounded by ceil(N/K) ticks (3 at K=2/N=6),
	// not by 1.
	//
	// ⛔ THE GATE BELOW IS UNAFFECTED, AND THAT IS A DERIVED FACT, NOT AN
	// OVERSIGHT — do not "update" it for the new cadence. The gate compares the
	// missed tick's DISTANCE FROM THE PREDICTION FRONTIER against
	// rollbackWindowHardCap. That distance is a CLOCK-OFFSET quantity (how far the
	// client is predicting ahead of the authority tick the correction describes);
	// it does not depend on how OFTEN corrections are sent. Rotation changes the
	// number of corrections per second, not the distance any one of them lands at.
	// The 2026-07-20 verification numbers below therefore still bind unchanged.
	//
	// Logging these at Warning cried wolf badly enough to cost real diagnostic time
	// during that session, because the shape resembled the v1 T23/T24 hard-lock bug
	// signature.
	//
	// GATE: warn only when the missed tick is FURTHER than rollbackWindowHardCap
	// from the prediction frontier. That bound is the maximum depth the reconciler
	// will ever resimulate, so a miss beyond it could not have become a useful
	// resim anchor even had a slot existed — and, because TimeConfig pins the
	// ordering invariant `hardResyncThresholdTicks > rollbackWindowHardCap`, a
	// sustained miss out there means the clock should have hard-resynced and has
	// not. That is genuinely anomalous. Everything closer is routine and logs at
	// Verbose.
	//
	// Verified against the 2026-07-20 session (both clients, both miss sites,
	// 111 events): 108 events had a live frontier with a max distance of exactly
	// 20 == rollbackWindowHardCap, and 3 were empty-cache registration transients.
	// This gate suppresses all 111 while still firing at distance 21+.
	//
	// NOTE: the frontier check is `getPredictionTick() != 0` because both
	// constructors fill the tick buffer with 0. The deferred UINT32_MAX empty-slot
	// sentinel (researcher item 3, deliberately out of scope here) would make this
	// exact rather than conventional.
	bool isAnomalousMiss(uint32 missedTick) const
	{
		const uint32 predictionTick = getPredictionTick();

		// No prediction frontier yet — registration transient. Comparing a real
		// server tick against a never-initialised frontier is meaningless, not
		// anomalous.
		if (predictionTick == 0)
			return false;

		const uint32 distance = (missedTick > predictionTick)
			? (missedTick - predictionTick)
			: (predictionTick - missedTick);

		return distance > m_anomalousMissDistanceTicks;
	}

	// Seam for wiring the LIVE TimeConfig once a consumer has one to hand. The
	// default is sourced from the TimeConfig default (not a literal), so R-P1
	// holds and the gate tracks any retune of the field's default; it does NOT
	// track a runtime override. Acceptable for a log gate, one line to fix.
	void setAnomalousMissDistanceTicks(uint32 distanceTicks)
	{
		m_anomalousMissDistanceTicks = distanceTicks;
	}

	uint32 getLastCorrectTick() const
	{
		const uint32 predictionTick = getPredictionTick();
		const uint32 predictionIndex = getCacheIndex(predictionTick);

		//iterate backwards, from the prediction index, through the ring buffer to find the last correct tick
		for (int32 offset = 0; offset < StateBufferSize; ++offset)
		{
			int32 checkIndex = static_cast<int32>(predictionIndex) - offset;
			if (checkIndex < 0)
				checkIndex += StateBufferSize;
			if (m_containsCorrectTick.test(checkIndex))
			{
				return m_tickBuffer[checkIndex];
			}
		}

		return 0;
	}

	uint32 getLastResimulationTick() const
	{
		uint32 lastCorrectTick = getLastCorrectTick();
		if (lastCorrectTick == 0)
			return 0;

		const uint32 lastCorrectIndex = getCacheIndex(lastCorrectTick);
		
		if (lastCorrectIndex == InvalidCacheIndex)
			return 0;

		const uint32 predictionTick = getPredictionTick();
		const uint32 predictionIndex = getCacheIndex(predictionTick);

		const uint32 nrOfPredictedTicks = (predictionTick - lastCorrectTick);

		//iterate backwards, from the prediction index, through the ring buffer to find the last resimulated tick
		for (uint32 offset = 0; offset <= nrOfPredictedTicks; ++offset)
		{
			int32 checkIndex = static_cast<int32>(predictionIndex) - offset;
			if (checkIndex < 0)
				checkIndex += StateBufferSize;
			if (m_isResimulated.test(checkIndex) || m_containsCorrectTick.test(checkIndex))
				return m_tickBuffer[checkIndex];
		}

		return 0;
	}

	// [og-netcode-v2-input-relay T8] `getLastCorrectionInput` — the walk-backwards
	// "what did the server last tell us this player did" reader — IS GONE, together
	// with `insertCorrectionInput` and the `m_containsCorrectionInput` bitset it
	// tested. It read the SERVER->CLIENT correction-input channel, and that channel
	// is retired: nothing sets a correction flag on an input slot any more, so the
	// method could only ever have returned `std::nullopt`. It was removed rather
	// than left in place precisely BECAUSE it would still have compiled, still have
	// looked authoritative at a call site, and still have answered "no input has
	// ever been corrected" forever, silently. See the retirement block at
	// SimulationNetSync::sendCorrectionAll for the full channel inventory.

	// [og-netcode-v2-input-relay T16] `getLatestInput` — "the input at the current
	// prediction-tick slot" — IS GONE, together with the column it read.
	//
	// T8 kept it deliberately, under the rule "remove what T8 makes FALSE, leave
	// what T8 makes merely UNUSED": it still returned real, freshly-written data,
	// it just had no caller left. T16 removes the writer, so there is nothing to
	// return and the distinction collapses.
	//
	// It was ALSO the most dangerous accessor on this class for a remote
	// character, and that is worth keeping on the record: the slot was written by
	// `pushPredictionInput` only, so for a proxy it answered the CLIENT'S OWN
	// PREDICTION of that player's input (post-T7, the relay store's scheduled
	// read) while looking exactly like a server-sourced value — refreshed every
	// tick, so with no staleness to notice. The correct remote source is
	// `SimulationNetSync::getLastRelayedInput` / `resolveScheduledRelayedInput`;
	// the correct local source is the client's own `ClientInputDelayLine`.

	bool needsResimulation()
	{
		uint32 lastResimulationTick = getLastResimulationTick();

		if (lastResimulationTick != 0 && lastResimulationTick != getPredictionTick())
			return true;
		else
			return false;
	}

	void pushPredictionTick(uint32 tick)
	{
		const uint32 predictionTick = getPredictionTick();

		OG_CHECK(tick >= predictionTick, "Setting bad prediction tick");

		if (tick == predictionTick)
			return; // already at this tick (e.g. clock stall); no new slot needed

		const uint32 predictionIndex = getCacheIndex(predictionTick);

		uint32 newPredictionIndex = (predictionIndex + 1) % StateBufferSize;
		m_tickBuffer[newPredictionIndex] = tick;

		m_containsCorrectTick[newPredictionIndex] = false;
		m_predictionWasCorrect[newPredictionIndex] = false;
		m_isResimulated[newPredictionIndex] = m_isResimulated[predictionIndex];
		// [T4] The recycled slot now describes a DIFFERENT tick, so the previous
		// occupant's join key must retire with it — otherwise a resim through the
		// fresh tick would resolve remote input from a capture ~StateBufferSize
		// ticks old and be confidently wrong rather than merely uninformed.
		m_appliedCaptureTickBuffer[newPredictionIndex] = kNoInputCaptureTick;
	}

	// [og-netcode-v2-input-relay T16] `pushPredictionInput` — the client-local
	// writer of the input column, and by T8 its ONLY writer — IS GONE. Its two
	// production call sites were both arms of
	// `SimulationNetSync::collectInputAll` (the provider branch, writing the
	// already-delayed applied capture; and the simulated-proxy branch, writing
	// this client's guess at the remote input), plus
	// `SimulationReconciliation::backfillSkippedTick` and `advance_frame` below.
	// The prediction TICK is still pushed at every one of those sites — only the
	// input write went, so the ring's slot allocation is bit-for-bit unchanged.

	void pushPredictionState(const StateType& state)
	{
		const uint32 predictionIndex = getCacheIndex(getPredictionTick());
		m_stateBuffer[predictionIndex] = state;
	}

	void tryInsertingResimulatedState(StateType&& state, uint32 tick)
	{
		auto it = std::find(m_tickBuffer.begin(), m_tickBuffer.end(), tick);
		if (it != m_tickBuffer.end())
		{
			uint32 cacheIndex = std::distance(m_tickBuffer.begin(), it);
			m_stateBuffer[cacheIndex] = std::move(state);
			m_isResimulated.set(cacheIndex);
			if (m_logger)
			{
				char buf[128];
				std::snprintf(buf, sizeof(buf),
					"insertResimulatedState: tick=%u cacheIndex=%u", tick, cacheIndex);
				m_logger(buf);
			}
		}
		else
		{
			// Tick not in cache window — correction arrived too late or too early; discard.
			if (m_logger)
			{
				char buf[128];
				std::snprintf(buf, sizeof(buf), "[Warning] tryInsertingResimulatedState: tick=%u not in cache window, discarding", tick);
				m_logger(buf);
			}
			return;
		}
	}

	// `appliedCaptureTick` is the correction's per-tick join key (T4): the capture
	// tick of the input the authority applied when it produced THIS state. It
	// defaults to the sentinel so every pre-T4 call site (and any caller with no
	// wire ref to hand, e.g. the test harnesses) keeps its exact prior meaning —
	// "no ref known for this tick".
	//
	// It is stored on the HIT path regardless of predictionWasCorrect: the ref
	// describes what the AUTHORITY did, which is equally true whether or not the
	// local prediction happened to match, and T6 needs it in both cases.
	//
	// [T24] `outVerdict` is a DEFAULTED, PURELY OBSERVATIONAL out-parameter: it
	// reports the `isSimilarTo` verdict this method has always computed, so a
	// caller that knows the character id and its class can attribute it. Nothing
	// on this method's behaviour depends on whether it is supplied. See
	// CorrectionInsertVerdict at the top of this file.
	void tryInsertingCorrectState(StateType&& state, uint32 tick,
	                              uint32 appliedCaptureTick = kNoInputCaptureTick,
	                              CorrectionInsertVerdict* outVerdict = nullptr)
	{
		if (outVerdict != nullptr)
			*outVerdict = CorrectionInsertVerdict{ false, false, tick };

		auto it = std::find(m_tickBuffer.begin(), m_tickBuffer.end(), tick);
		if (it != m_tickBuffer.end())
		{
			uint32 cacheIndex = std::distance(m_tickBuffer.begin(), it);

			const bool predictionWasCorrect = m_stateBuffer[cacheIndex].isSimilarTo(state);

			m_containsCorrectTick.set(cacheIndex);
			m_predictionWasCorrect[cacheIndex] = predictionWasCorrect;
			m_isResimulated[cacheIndex] = false;
			m_appliedCaptureTickBuffer[cacheIndex] = appliedCaptureTick;

			// [T24] Reported BEFORE the state move below, for no reason other than
			// that the verdict is about the state as it was compared; the flag is a
			// bool copy and the ordering is not load-bearing.
			if (outVerdict != nullptr)
				*outVerdict = CorrectionInsertVerdict{ true, predictionWasCorrect, tick };

			if (!predictionWasCorrect)
				m_stateBuffer[cacheIndex] = std::move(state);

			if (m_logger)
			{
				char buf[128];
				std::snprintf(buf, sizeof(buf), "inserting correction at tick=%u, correct=%u ", tick, predictionWasCorrect);
				m_logger(buf);
			}
		}
		else
		{
			// Tick not in cache window — correction arrived too late or too early; discard.
			// Severity is gated by isAnomalousMiss: routine self-healing misses log at
			// Verbose, only a miss beyond the reconciler's reach warns. predictionTick is
			// now included so a Warning from this site is actionable on its own.
			if (m_logger)
			{
				char buf[192];
				std::snprintf(buf, sizeof(buf),
					"%s tryInsertingCorrectState: tick=%u not in cache window (predictionTick=%u), discarding",
					isAnomalousMiss(tick) ? "[Warning]" : "[Verbose]", tick, getPredictionTick());
				m_logger(buf);
			}
			return;
		}
	}

	// [og-netcode-v2-input-relay T8] `insertCorrectionInput` — the SERVER->CLIENT
	// correction-input channel's terminus — IS GONE. Its only production caller was
	// `SimulationReconciliation::injectCorrectionInput`, itself reached only from
	// the OnRep-bound callback that `SimulationNetSync::registerPredictionOwner`
	// used to install; all three are retired together with the replicated property
	// on the UE side. The isAnomalousMiss severity gate it shared with
	// `tryInsertingCorrectState` is unaffected — that gate lives on the method
	// below and is still exercised through the state path.
	//
	// [T16] T8's note here said the input COLUMN itself (`m_inputBuffer`) and its
	// client-local writer `pushPredictionInput` were deliberately left standing,
	// as T16's scope. THEY ARE NOW GONE TOO — T8 removed the channel, T16 removed
	// the column. Nothing on this class stores an input value any more.

	void wipeCache(unsigned int newPredictionTick)
	{
		unsigned int predictionIndex = getCacheIndex(getPredictionTick());
		m_tickBuffer.fill(0);
		m_containsCorrectTick.reset();
		m_predictionWasCorrect.reset();
		m_isResimulated.reset();
		// [T4] Every surviving join key describes a tick numbering that the resync
		// just invalidated — same reasoning as the delay-line clear in
		// SimulationNetSync::wipeAllForResync.
		m_appliedCaptureTickBuffer.fill(kNoInputCaptureTick);

		m_tickBuffer[predictionIndex] = newPredictionTick;
	}

	// StateCorrectionCache 4-method external API (proposal §2.2).
	// Additive public wrappers around the existing internal mechanisms; the legacy
	// API surface above is unchanged. These exist so the Catch2 determinism harness
	// (and, from Stage 3, the rollback driver) can save/load/advance/checksum the
	// cache without a live SimulationManager.

	// Writes `state` into the cache slot for `tick`.
	//
	// Slot-collision semantics: if `tick` is already present in the cache (at any
	// index, e.g. inserted earlier via the ring-advancing prediction path), its
	// existing slot is updated in place — we never create a duplicate entry for the
	// same tick. Otherwise the slot is `tick % StateBufferSize`; writing there
	// naturally evicts whatever older tick previously mapped to that ring slot
	// (the cache holds a rolling window of StateBufferSize ticks). A freshly
	// allocated slot has its per-slot bookkeeping bits reset (mirrors
	// pushPredictionTick), so stale correction/resim metadata cannot leak.
	void save_snapshot(uint32 tick, const StateType& state)
	{
		const uint32 existingIndex = getCacheIndex(tick);
		const uint32 slot = (existingIndex != InvalidCacheIndex)
			? existingIndex
			: static_cast<uint32>(tick % StateBufferSize);

		if (existingIndex == InvalidCacheIndex)
		{
			m_tickBuffer[slot] = tick;
			m_containsCorrectTick[slot] = false;
			m_predictionWasCorrect[slot] = false;
			m_isResimulated[slot] = false;
			// [T4] Freshly allocated slot — no correction has named a join key for
			// this tick yet (mirrors pushPredictionTick).
			m_appliedCaptureTickBuffer[slot] = kNoInputCaptureTick;
		}

		m_stateBuffer[slot] = state;
	}

	// Returns true and fills `out_state` if `tick` is in the cache window; false otherwise.
	[[nodiscard]] bool load_snapshot(uint32 tick, StateType& out_state) const
	{
		const uint32 cacheIndex = getCacheIndex(tick);
		if (cacheIndex == InvalidCacheIndex)
			return false;

		out_state = m_stateBuffer[cacheIndex];
		return true;
	}

	// Drives one externally-triggered sim step. Reads the previous prediction
	// state, integrates it via the injected functor, then commits the new
	// (tick, state) into the cache exactly as the manual
	// pushPredictionTick + pushPredictionState sequence would.
	// OG_CHECK-fails if the cache was built without an integrate functor.
	//
	// [T16] `input` is CONSUMED, not stored: it feeds m_integrateFn and nothing
	// else, because the cache no longer has an input column to commit it to. This
	// is why the InputType template parameter outlives that column — see the class
	// header block. The 4-method external API's shape is unchanged for callers.
	void advance_frame(uint32 tick, const InputType& input)
	{
		OG_CHECK(static_cast<bool>(m_integrateFn),
			"advance_frame called on a cache with no integrate functor (use the 2-arg constructor)");

		const uint32 prevIndex = getCacheIndex(getPredictionTick());
		OG_CHECK(prevIndex != InvalidCacheIndex, "advance_frame: previous prediction tick not in cache");

		// Integrate into a fresh value BEFORE mutating the ring (prevState is a
		// reference into m_stateBuffer and must stay valid through the call).
		StateType newState = m_integrateFn(tick, m_stateBuffer[prevIndex], input);

		pushPredictionTick(tick);
		pushPredictionState(newState);
	}

	// CRC-32 over the serialized bytes of the state cached at `tick`. Returns 0
	// (with a logger warning) if `tick` is not in the cache window. The state is
	// serialized via the project serializer when possible (padding-free,
	// deterministic across compilers/architectures); trivially-copyable test
	// types fall back to a raw-byte hash.
	[[nodiscard]] uint32 compute_checksum(uint32 tick) const
	{
		const uint32 cacheIndex = getCacheIndex(tick);
		if (cacheIndex == InvalidCacheIndex)
		{
			if (m_logger)
			{
				char buf[128];
				std::snprintf(buf, sizeof(buf),
					"[Warning] compute_checksum: tick=%u not in cache window, returning 0", tick);
				m_logger(buf);
			}
			return 0;
		}

		ChecksumByteBuffer buffer;
		const std::uint32_t written = serializeStateForChecksum(m_stateBuffer[cacheIndex], buffer);
		return crc32(buffer.data(), written);
	}

private:
	// Serializes a state into the checksum byte sink. Prefers the project's
	// field-wise serializer (Serializable scalars/aggregates and SimulationComposite)
	// for determinism; trivially-copyable POD test types use a raw-byte fallback.
	template <typename S>
	static std::uint32_t serializeStateForChecksum(const S& state, ChecksumByteBuffer& buffer)
	{
		if constexpr (Serializable<S>)
		{
			return writeToSyncedBuffer(state, buffer, 0u);
		}
		else if constexpr (requires { writeCompositeToSyncedBuffer(state, buffer, 0u); })
		{
			return writeCompositeToSyncedBuffer(state, buffer, 0u);
		}
		else
		{
			static_assert(std::is_trivially_copyable_v<S>,
				"compute_checksum: StateType must be Serializable, a SimulationComposite, or trivially copyable");
			buffer.writeRaw(0u, reinterpret_cast<const std::uint8_t*>(&state), sizeof(S));
			return static_cast<std::uint32_t>(sizeof(S));
		}
	}

	std::array<StateType, StateBufferSize> m_stateBuffer;
	// [T16] `m_inputBuffer` — `std::array<std::optional<InputType>, 60>`, the
	// input COLUMN — is gone. One optional input composite per slot, times 60
	// slots, times every predicted character on every client, storing a value that
	// no consumer had left after T6/T7/T8/T15. See the class header block for what
	// replaced each read.
	std::array<uint32, StateBufferSize> m_tickBuffer;
	// [T4 / D3] Per-slot join key — see getAppliedCaptureTick. Parallel to
	// m_tickBuffer: index i answers "which capture tick did the authority apply at
	// m_tickBuffer[i]". kNoInputCaptureTick everywhere it is unknown.
	std::array<uint32, StateBufferSize> m_appliedCaptureTickBuffer;
	std::bitset<StateBufferSize> m_containsCorrectTick;
	std::bitset<StateBufferSize> m_predictionWasCorrect;
	std::bitset<StateBufferSize> m_isResimulated;
	// [T8] `m_containsCorrectionInput` — the per-slot "this input came from the
	// authority" flag — is gone with the channel that set it. Its sole setter was
	// insertCorrectionInput and its sole reader getLastCorrectionInput; with the
	// setter retired the flag could only ever have read false, so keeping it would
	// have kept a discriminator that no longer discriminates.
	std::function<void(const char*)> m_logger;
	IntegrateFn m_integrateFn;

	// Log gate only — never read by insertion logic. See isAnomalousMiss.
	uint32 m_anomalousMissDistanceTicks =
		static_cast<uint32>(TimeConfig{}.rollbackWindowHardCap);
};

template <typename SyncedBuffer, typename StateType, typename InputType>
void receiveCorrectionState(StateCorrectionCache<StateType, InputType>& cache, SyncedBuffer& buffer, std::function<void(StateType&, const SyncedBuffer&, uint32)> readBufferFunction)
{
	uint32 internalByteIterator = 0;

	const uint32 tick = (buffer.template readFromBuffer<uint32>(internalByteIterator));
	internalByteIterator += sizeof(uint32);

	StateType state;
	readBufferFunction(state, buffer, internalByteIterator);
	cache.tryInsertingCorrectState(std::move(state), tick);
}

// [og-netcode-v2-input-relay T8] `receiveCorrectionInput` — the free-function
// mirror of receiveCorrectionState for the input channel — IS GONE. It decoded a
// (tick, input) payload that no sender writes any more. `receiveCorrectionState`
// above is unchanged and remains the live correction path; it is the state, plus
// the T4 applied-capture-tick ref it carries, that the client now needs.

template <typename SyncedBuffer, typename StateType, typename InputType>
void sendCorrectionState(const SimulationTimeStep& timingInfo, const StateType& state, SyncedBuffer& buffer, std::function<uint32(const StateType&, SyncedBuffer&, uint32)> writeBufferFunction)
{
	uint32 internalByteIterator = 0;

	buffer.template writeToBuffer<uint32>(internalByteIterator, timingInfo.getTick());
	internalByteIterator += sizeof(uint32);

	internalByteIterator += writeBufferFunction(state, buffer, internalByteIterator);
}

#pragma optimize( "", on )
// pragma optimize on.



