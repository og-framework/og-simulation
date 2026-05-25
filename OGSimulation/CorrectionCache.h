#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include "OGAssert.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <vector>
#include <optional>
#include <limits>
#include <bitset>
#include "glm/vec3.hpp"
#include <glm/gtc/quaternion.hpp>
#include "OGSimulation/SimulationTimeContext.h"

#pragma optimize( "", off )

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template <typename StateType, typename InputType>
class StateCorrectionCache
{
public:
	static constexpr size_t StateBufferSize = 60;
	static constexpr size_t InvalidCacheIndex = 1337;

	StateCorrectionCache(std::function<void(const char*)> logger)
		: m_stateBuffer()
		, m_inputBuffer()
		, m_tickBuffer()
		, m_logger(std::move(logger))
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
			if (m_logger)
			{
				char buf[128];
				std::snprintf(buf, sizeof(buf), "[Warning] tryInsertingCorrectState: tick=%u not in cache window, discarding", tick);
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
			if (m_logger)
			{
				char buf[192];
				std::snprintf(buf, sizeof(buf),
					"[Warning] insertCorrectionInput: tick=%u not in cache (predictionTick=%u), discarding",
					tick, getPredictionTick());
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

private:
	std::array<StateType, StateBufferSize> m_stateBuffer;
	std::array<std::optional<InputType>, StateBufferSize> m_inputBuffer;
	std::array<uint32, StateBufferSize> m_tickBuffer;
	std::bitset<StateBufferSize> m_containsCorrectTick;
	std::bitset<StateBufferSize> m_predictionWasCorrect;
	std::bitset<StateBufferSize> m_isResimulated;
	std::bitset<StateBufferSize> m_containsCorrectionInput;
	std::function<void(const char*)> m_logger;
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



