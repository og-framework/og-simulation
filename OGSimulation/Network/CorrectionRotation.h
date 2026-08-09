#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// correctionRotation — WRITE-SITE STATE CADENCE.
// (og-netcode-v2-input-relay / T39, Stage 1 of input-first replication;
//  design_task38_input_first_replication.md §5.4, §6 Candidate A, §13.2.)
//
// WHAT THIS IS. `SimulationNetSync::sendCorrectionAll` used to write EVERY
// authority writer's correction-state buffer on EVERY tick. This kernel turns
// that into a round-robin: K writers per call, the window advancing by K each
// call, wrapping. A character not written is not dirty, and Iris rolls back the
// headers of clean objects (`FReplicationWriter::WriteObjectBatch`), so a
// skipped character costs ZERO bytes rather than a skipped batch.
//
// WHY AT THE WRITE SITE AND NOT IN THE ENGINE (T38 §5.4). Two mechanisms could
// throttle the state: static priority (let Iris skip it under pressure) or
// write-site gating. Gating was chosen because it is DETERMINISTIC — the cadence
// is a decided number that a probe can report and a test can pin — while a
// priority-only scheme yields whatever cadence the packet happened to allow.
// Static priority is still set (ring 4.0 above state 1.0) but it is the BACKSTOP
// for the frames where a written state coincides with a burst, not the cadence
// mechanism. "The state cadence must be a decided, configured number" is the
// point of the change.
//
// ⚠ THE COUPLING THIS DELIBERATELY RELAXES (T38 §2.3). The retirement block at
// `SimulationNetSync::sendCorrectionAll` names sparse state as "THE ONE THING
// THAT WOULD RE-OPEN THIS" for the own-character input-echo drop, because the
// masking argument there is the every-frame-correction argument. That block is
// updated in the same change that introduces this kernel; read it before
// reasoning about repair latency.
//
// -------------------------------------------------------------------------
// THE KERNEL IS SEPARATE FROM ITS CALLER ON PURPOSE.
// -------------------------------------------------------------------------
// `sendCorrectionAll` lives on the `SimulationNetSync` class template, which can
// only be exercised from a suite that binds `SimulatableOwnerTraits` to concrete
// owners (og-brawler-tests). The selection arithmetic — coverage, wrap, the
// K >= N degeneracy, the clamp — is pure and belongs in the core suite
// (og-simulation-tests) where it can be tested exhaustively without a mock
// character. Both halves are tested: the kernel as a unit, and the fact that
// sendCorrectionAll actually calls it as an integration case.
// ---------------------------------------------------------------------------
namespace correctionRotation
{
	// -----------------------------------------------------------------------
	// THE CLAMP BOUNDS. K is a count of characters written per tick, so 0 and
	// negatives are not "off" — they are a silently disabled correction channel,
	// which is a desync, not a saving. They clamp UP to 1. The ceiling is
	// deliberately generous (16 > any supported character count, T38's target is
	// 6): the knob's purpose is to REDUCE cadence, and "K >= N" is the legitimate
	// every-frame setting, so the ceiling only has to stop a typo from becoming a
	// nonsense number. This mirrors the shape `relayedInputRing::clampDepth` and
	// `clampRelayDelayFloorTicks` established for the sibling knobs.
	// -----------------------------------------------------------------------
	inline constexpr std::int32_t kMinK = 1;
	inline constexpr std::int32_t kMaxK = 16;

	// THE ONE shared, idempotent clamp. Called at the ini intake in the
	// composition root, at `SimulationManager::setCorrectionRotationK`, and once
	// more inside the selection predicate below — the same belt-and-braces the
	// depth knob uses. Idempotence is what makes the repetition safe.
	constexpr std::int32_t clampK(std::int32_t requestedK)
	{
		if (requestedK < kMinK) return kMinK;
		if (requestedK > kMaxK) return kMaxK;
		return requestedK;
	}

	// -----------------------------------------------------------------------
	// THE SELECTION PREDICATE.
	//
	// `roundBase` is a MONOTONIC counter that the caller advances by K once per
	// send (see advanceRound); `index` is the writer's 0-based position in the
	// enumeration the caller walks; `writerCount` is N for that enumeration.
	// The window is the K consecutive positions starting at `roundBase % N`,
	// wrapping.
	//
	// WHY A MONOTONIC BASE RATHER THAN A PRE-WRAPPED CURSOR: one counter then
	// serves every simulatable type in the tuple, each of which has its own N.
	// Wrapping is applied per type at the point of use, so a two-type session
	// cannot have one type's registration count corrupt the other's schedule.
	//
	// COVERAGE, which is the property the caller depends on: with the base
	// advancing by K each round, the windows cover positions
	// [0, ceil(N/K)*K) mod N over ceil(N/K) rounds — at least N consecutive
	// positions — so EVERY writer is written at least once within ceil(N/K)
	// consecutive sends, for every N and every K. That bound is what makes
	// "state cadence = 60 * K / N Hz" an honest statement rather than an average.
	// -----------------------------------------------------------------------
	constexpr bool isInRound(std::size_t index,
	                         std::size_t roundBase,
	                         std::size_t writerCount,
	                         std::int32_t k)
	{
		if (writerCount == 0u)
			return false;

		const std::size_t clamped = static_cast<std::size_t>(clampK(k));

		// K >= N degenerates to every-frame — bit-identical to the pre-rotation
		// behaviour, which is what keeps the 2-character archived baselines
		// comparable at the shipped K of 2.
		if (clamped >= writerCount)
			return true;

		const std::size_t start  = roundBase % writerCount;
		const std::size_t offset = (index + writerCount - start) % writerCount;
		return offset < clamped;
	}

	// The base for the NEXT send. Deliberately NOT wrapped here — see the
	// per-type wrapping note above. std::size_t is 64-bit on every supported
	// platform, so at 60 sends/second with K <= 16 this does not wrap in any
	// realistic session lifetime; and even if it did, the modulo at the use site
	// would simply re-phase the schedule, not break coverage.
	constexpr std::size_t advanceRound(std::size_t roundBase, std::int32_t k)
	{
		return roundBase + static_cast<std::size_t>(clampK(k));
	}
} // namespace correctionRotation
