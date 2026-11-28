#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/RelayRingStager-rationale.md · docs/RelayRingStager-guards.md

#include "OGSimulation/Network/SimPayloadBuffers.h"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/RelayedInputRingCodec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

struct RelayRingStagerCounters
{
	std::uint32_t flushedRounds      = 0u;
	std::uint32_t entries            = 0u;
	std::uint32_t suppressed         = 0u;
	std::uint32_t stageOverflowDrops = 0u;
	std::uint32_t resent             = 0u;

	constexpr bool operator==(const RelayRingStagerCounters&) const = default;
};

class RelayRingStager
{
public:
	using Logger = std::function<void(const char*)>;

	static constexpr std::uint32_t kFlushReportRounds = 120u;
	static constexpr std::size_t   kMaxDebugNameBytes = 128u;
	static constexpr std::size_t   kReportLineBytes   = 384u;

	static constexpr char kReportFormat[] =
		"[Warning][RelayFlush] %s owner=%s flushedRounds=%u entries=%u entriesPerRoundX100=%u "
		"suppressed=%u stageOverflowDrops=%u resent=%u";

	static_assert(kReportLineBytes >= sizeof(kReportFormat) + sizeof("teardown") + kMaxDebugNameBytes + 6u * 10u,
		"kReportLineBytes must hold the longest [RelayFlush] line: the format, the longest reason, a kMaxDebugNameBytes "
		"owner and six 10-digit counters. Was the actor's FString line, which had no length bound");

	explicit RelayRingStager(std::string debugName, Logger logger = {})
		: m_debugName(std::move(debugName))
		, m_logger(std::move(logger))
	{
		OG_CHECK(m_debugName.size() <= kMaxDebugNameBytes,
			"RelayRingStager: debugName is longer than kMaxDebugNameBytes, so its [RelayFlush] line would be truncated");
	}

	template <typename InputT>
	relayedInputRing::StageArrivalOutcome stageArrival(std::uint32_t captureTick, std::uint8_t dA, const InputT& input)
	{
		const relayedInputRing::StageArrivalOutcome outcome = m_stage.stageArrival<InputT>(captureTick, dA, input);
		// ⛔G-01  docs/RelayRingStager-guards.md
		if (outcome.droppedOldest)
			++m_counters.stageOverflowDrops;
		return outcome;
	}

	std::uint8_t flush(std::uint8_t resendEntries)
	{
		const std::uint8_t published = relayedInputRing::flushStagedInto(m_published, m_stage, resendEntries);
		if (published > 0u)
		{
			++m_counters.flushedRounds;
			m_counters.entries += published;
			m_counters.resent += static_cast<std::uint32_t>(relayedInputRing::entryCount(m_published) - published);
		}
		else
		{
			++m_counters.suppressed;
		}

		// ⛔G-02  docs/RelayRingStager-guards.md
		if (++m_flushesSinceReport >= kFlushReportRounds)
		{
			// ⛔G-03  docs/RelayRingStager-guards.md
			m_flushesSinceReport = 0u;
			emitReport("window");
		}
		return published;
	}

	void finalReport() const { emitReport("teardown"); }

	const RelayRingBuffer&         published() const { return m_published; }
	const RelayRingStagerCounters& counters() const { return m_counters; }
	const std::string&             debugName() const { return m_debugName; }

private:
	void emitReport(const char* reason) const
	{
		if (!m_logger)
			return;

		const std::uint32_t entriesPerRoundX100 = (m_counters.flushedRounds == 0u)
			? 0u
			: static_cast<std::uint32_t>(
				(static_cast<std::uint64_t>(m_counters.entries) * 100u) / m_counters.flushedRounds);

		std::array<char, kReportLineBytes> line{};
		std::snprintf(line.data(), line.size(), kReportFormat,
			reason,
			m_debugName.c_str(),
			static_cast<unsigned>(m_counters.flushedRounds),
			static_cast<unsigned>(m_counters.entries),
			static_cast<unsigned>(entriesPerRoundX100),
			static_cast<unsigned>(m_counters.suppressed),
			static_cast<unsigned>(m_counters.stageOverflowDrops),
			static_cast<unsigned>(m_counters.resent));
		m_logger(line.data());
	}

	std::string             m_debugName;
	Logger                  m_logger;
	RelayRingBuffer         m_stage;
	RelayRingBuffer         m_published;
	RelayRingStagerCounters m_counters;
	std::uint32_t           m_flushesSinceReport = 0u;
};

namespace relayRingStager::detail
{
	struct SignatureProbeInput
	{
		std::uint8_t value = 0u;
	};
} // namespace relayRingStager::detail

static_assert(std::is_same_v<decltype(&RelayRingStager::stageArrival<relayRingStager::detail::SignatureProbeInput>),
		relayedInputRing::StageArrivalOutcome (RelayRingStager::*)(
			std::uint32_t, std::uint8_t, const relayRingStager::detail::SignatureProbeInput&)>,
	"RelayRingStager::stageArrival takes NO depth parameter. The stage's capacity is relayedInputRing::kMaxDepth, fixed "
	"inside the codec; a depth passed down here would cap every round at one entry and silently restore the "
	"replace-latest behaviour the staging removed. Was the 'NO DEPTH PARAMETER' fence, the same one "
	"UESimulatableOwnerBinding::stageRelayedInput's signature static_assert carried for the UE stage until "
	"og-input-latency task P5-8 deleted it");
