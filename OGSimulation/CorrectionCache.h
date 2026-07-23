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
		, m_inputBuffer()
		, m_tickBuffer()
		, m_logger(std::move(logger))
		, m_integrateFn()
	{
		m_tickBuffer.fill(0);
	}

	// Overload that injects an integrate functor so advance_frame() can
	// drive one externally-triggered sim step. The single-arg constructor stays;
	// caches built with it must not call advance_frame() (it OG_CHECK-fails).
	StateCorrectionCache(std::function<void(const char*)> logger, IntegrateFn integrateFn)
		: m_stateBuffer()
		, m_inputBuffer()
		, m_tickBuffer()
		, m_logger(std::move(logger))
		, m_integrateFn(std::move(integrateFn))
	{
		m_tickBuffer.fill(0);
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

	const InputType& getInput(uint32 cacheIndex) const
	{
		OG_CHECK(cacheIndex < StateBufferSize, "trying to access input with bad cacheIndex");
		OG_CHECK(m_inputBuffer[cacheIndex].has_value(), "trying to access input that has not been set");
		
		return m_inputBuffer[cacheIndex].value();
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
	// Corrections are an unthrottled per-tick stream and reconciliation anchors on
	// the newest LANDED correction rather than a specific tick, so a miss costs at
	// most one tick of delayed reconciliation. Logging these at Warning cried wolf
	// badly enough to cost real diagnostic time during that session, because the
	// shape resembled the v1 T23/T24 hard-lock bug signature.
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

	std::optional<InputType> getLastCorrectionInput() const
	{
		const uint32 predictionTick = getPredictionTick();
		const uint32 predictionIndex = getCacheIndex(predictionTick);

		for (int32 offset = 0; offset < static_cast<int32>(StateBufferSize); ++offset)
		{
			int32 checkIndex = static_cast<int32>(predictionIndex) - offset;
			if (checkIndex < 0)
				checkIndex += StateBufferSize;
			if (m_containsCorrectionInput.test(checkIndex) && m_inputBuffer[checkIndex].has_value())
				return m_inputBuffer[checkIndex].value();
		}
		return std::nullopt;
	}

	// Returns the input at the current prediction-tick slot — the just-applied
	// input for LOCAL characters (via pushPredictionInput in collectInputAll's
	// provider branch) and the last-server-corrected value for REMOTE simulated
	// proxies (via collectInputAll's simulated-proxy branch, also calling
	// pushPredictionInput). Complements getLastCorrectionInput, which walks
	// backward for server-correction-flagged slots (older by ~1 RTT for the
	// local character). Returns nullopt if the slot is empty (transient state
	// at registration).
	std::optional<InputType> getLatestInput() const
	{
		const uint32 predictionIndex = getCacheIndex(getPredictionTick());
		return m_inputBuffer[predictionIndex];
	}

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
		m_containsCorrectionInput[newPredictionIndex] = false;
	}

	void pushPredictionInput(const InputType& input)
	{
		const uint32 predictionIndex = getCacheIndex(getPredictionTick());
		m_inputBuffer[predictionIndex].emplace(input);
	}

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

	void tryInsertingCorrectState(StateType&& state, uint32 tick)
	{
		auto it = std::find(m_tickBuffer.begin(), m_tickBuffer.end(), tick);
		if (it != m_tickBuffer.end())
		{
			uint32 cacheIndex = std::distance(m_tickBuffer.begin(), it);

			const bool predictionWasCorrect = m_stateBuffer[cacheIndex].isSimilarTo(state);
			
			m_containsCorrectTick.set(cacheIndex);
			m_predictionWasCorrect[cacheIndex] = predictionWasCorrect;
			m_isResimulated[cacheIndex] = false;

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

	void insertCorrectionInput(InputType&& input, uint32 tick)
	{
		auto it = std::find(m_tickBuffer.begin(), m_tickBuffer.end(), tick);
		if (it != m_tickBuffer.end())
		{
			const auto cacheIndex = std::distance(m_tickBuffer.begin(), it);
			m_inputBuffer[cacheIndex].emplace(std::move(input));
			m_containsCorrectionInput.set(cacheIndex);
			if (m_logger)
			{
				char buf[128];
				std::snprintf(buf, sizeof(buf),
					"[Verbose] insertCorrectionInput: tick=%u cacheIndex=%zu", tick, cacheIndex);
				m_logger(buf);
			}
		}
		else
		{
			// Severity gated by isAnomalousMiss — see the comment on that method.
			if (m_logger)
			{
				char buf[192];
				std::snprintf(buf, sizeof(buf),
					"%s insertCorrectionInput: tick=%u not in cache (predictionTick=%u), discarding",
					isAnomalousMiss(tick) ? "[Warning]" : "[Verbose]", tick, getPredictionTick());
				m_logger(buf);
			}
		}
	}

	void wipeCache(unsigned int newPredictionTick)
	{
		unsigned int predictionIndex = getCacheIndex(getPredictionTick());
		m_tickBuffer.fill(0);
		m_containsCorrectTick.reset();
		m_predictionWasCorrect.reset();
		m_isResimulated.reset();
		m_containsCorrectionInput.reset();

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
			m_containsCorrectionInput[slot] = false;
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
	// (tick, input, state) into the cache exactly as the manual
	// pushPredictionTick + pushPredictionInput + pushPredictionState sequence would.
	// OG_CHECK-fails if the cache was built without an integrate functor.
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
		pushPredictionInput(input);
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
	std::array<std::optional<InputType>, StateBufferSize> m_inputBuffer;
	std::array<uint32, StateBufferSize> m_tickBuffer;
	std::bitset<StateBufferSize> m_containsCorrectTick;
	std::bitset<StateBufferSize> m_predictionWasCorrect;
	std::bitset<StateBufferSize> m_isResimulated;
	std::bitset<StateBufferSize> m_containsCorrectionInput;
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


template <typename SyncedBuffer, typename StateType, typename InputType>
void receiveCorrectionInput(StateCorrectionCache<StateType, InputType>& cache, SyncedBuffer& buffer, std::function<InputType(const SyncedBuffer&, uint32)> readBufferFunction)
{
	uint32 internalByteIterator = 0;

	const uint32 tick = (buffer.template readFromBuffer<uint32>(internalByteIterator));
	internalByteIterator += sizeof(uint32);

	cache.insertCorrectionInput(readBufferFunction(buffer, internalByteIterator), tick);
}

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



