#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include "OGAssert.h"
#include <algorithm>
#include <array>
#include <atomic>
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
#include "OGSimulation/ResimGatePolicy.h"
#include "OGSimulation/SlotStateProvenance.h"

// Checksum support for the 4-method external API. §7
//
// ⛔ `crc32` is `inline`, NOT `static` — otherwise a TU that never instantiates
// `compute_checksum` emits -Wunused-function on the standalone builds. §7
//
// ⛔ `compute_checksum` CRCs the SERIALIZED bytes, never the raw object — padding-free and
// stable across compilers and architectures. `ChecksumByteBuffer` is the UE-free sink the
// existing `writeToSyncedBuffer` serializers target. §7

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

	// Scalar / trivially-copyable field sink for `writeToSyncedBuffer` descriptors.
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

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// `CorrectionInsertVerdict` — what `tryInsertingCorrectState` DECIDED, handed to a
// caller that knows WHO it decided it about. §10
//
// ⛔ THIS ADDS NO COMPUTATION: `predictionWasCorrect` is the existing `isSimilarTo` verdict,
// already stored in `m_predictionWasCorrect`. Only its ability to leave the function is new. §10
//
// ⛔ AN OUT-POINTER, NOT A RETURN VALUE — the cache is DELIBERATELY ID-AGNOSTIC and cannot
// attribute a verdict; the OnRep-bound callback in `SimulationNetSync::registerPredictionOwner`
// can. DEFAULTED, so every existing caller stays byte-identical by construction — the same
// trick that widened `resolveScheduledRelayedInput`. §10
//
// ⛔ The cache's own log line STAYS untagged and unchanged: it cannot carry an id, and
// retagging it would perturb the log-gating cases that count it. §10
struct CorrectionInsertVerdict
{
    // ⛔ `landed` false ⇒ DISCARDED and `predictionWasCorrect` is meaningless. NEVER count a
    // discard as agreement OR disagreement — it produced neither. §10
    bool   landed               = false;

    // The existing `isSimilarTo` verdict. ⛔ Meaningful only when `landed`. §10
    bool   predictionWasCorrect = false;

    // The correction's tick, echoed back so the caller needs no second wire decode.
    // ⛔ Set on BOTH paths. §10
    uint32 tick                 = 0u;
};

// ---------------------------------------------------------------------------
// ORIENTATION — WHAT ONE SLOT HOLDS, WHO WRITES IT, AND ON WHICH THREAD.
//
// Read this first. Every fence below states one invariant at the line it
// guards; none of them restates this map, and this map states no invariant.
//
//   * ONE SLOT, SIX COLUMNS — `slot i` describes exactly one tick:
//
//       column                       written by                        read by
//       m_tickBuffer                 push / save / wipe                everything: it is the slot
//                                                                      DIRECTORY and the FRONTIER at once
//       m_stateBuffer                prediction, correction, replay    resim, `isSimilarTo`, the checksum
//       m_appliedCaptureTickBuffer   corrections (the wire field)      resim input resolution
//       m_containsCorrectTick        corrections (GT); ring + wipe (PT) the protect-all write rule
//       m_slotLandingSeqNr           corrections (GT)                  the fresh/stale classifier —
//                                                                      COUNTERS ONLY, never a decision
//       m_stateProvenance            all five write sites              NOTHING IN PRODUCTION
//
//     ⛔ There is no input value and no resim bit in a slot. Both are retired, and what
//     replaced each read is listed at the two gravestones. §1
//
//   * TWO THREADS TOUCH THIS CLASS.
//       GAME thread     corrections land — `tryInsertingCorrectState`, reached from the
//                       OnRep_-dispatched lambdas via `SimulationReconciliation::injectCorrectionState`.
//       PHYSICS thread  everything else: the ring push pair, the replay write, both
//                       prepare-time captures, `wipeCache`, and every read.
//
//     ⭐ EXACTLY ONE WORD IS SYNCHRONIZED — `m_pendingResimAnchorTick` — because it is
//     CONTROL state with two legitimate writers and exactly one correct value. Every other
//     crossing below is unsynchronized and rides the pre-existing `m_stateBuffer` GT/PT race:
//     row 2 of `docs/ThreadingCrossings.md`, accepted debt, not re-argued here.
//
//       word / column                crossing              what a race COSTS
//       m_pendingResimAnchorTick     GT set / PT consume   nothing — atomic CAS. §2
//       m_containsCorrectTick        GT set / PT clear     ⛔ a LOST UPDATE of a whole
//                                                          DECISION BIT — not a torn value. §5
//       m_stateBuffer                GT write / PT read    torn or stale state: the
//                                                          pre-existing race itself. §5
//       m_slotLandingSeqNr           GT write / PT read    a mis-LABELLED counter; it can
//                                                          never mis-decide a write. §3
//       m_stateProvenance            GT + PT write         one wrong glyph in one slot-map
//                                                          line. Decides nothing. §4
//       m_frontierSlotAwaitingState  PT only               NO CROSSING — no doc row. §9
//
//   * THE FIVE PROVENANCE WRITE SITES, assembled here and contracted at each site. §4
//       1  pushPredictionTick             ring recycle       -> Predicted
//       2  tryInsertingCorrectState       a landing          -> AuthorityAdopted /
//                                                               AuthorityAgreedKeptPrediction
//       3  tryInsertingResimulatedState   the replay write   -> Replayed /
//                                                               ReplayedOverCorrection (the alarm)
//       4  wipeCache                      hard resync        -> Empty
//       5  save_snapshot                  fresh harness slot -> Predicted
//
//     ⛔ The column has NO PRODUCTION READER, and that is MACHINE-CHECKED rather than
//     asserted, by `CorrectionCache.ResimGate.TheProvenanceColumnCannotReachAnyProductionOutput`.
//     Its one unreachable value is proven live — not merely absent — by
//     `CorrectionCache.ResimGate.ReplayedOverCorrectionIsUnreachableButTheAlarmIsWired`, which
//     forges a precondition the protect-all rule makes unreachable. §4
//
//   * ⛔ WHAT MAY ENTER `compute_checksum`, AND THE RULE THAT DECIDES A NEW COLUMN.
//     THE RULE: a column enters the checksum iff it is SIMULATION STATE — a value two peers
//     replaying identical inputs from identical state MUST agree on. A column recording HOW a
//     peer reached this state, or WHAT IT INTENDS TO DO NEXT, is not, and two perfectly
//     deterministic peers may legitimately differ on it.
//       MAY:        `m_stateBuffer[tick]` — and today it is the only one.
//       MUST NEVER: `m_pendingResimAnchorTick`, `m_stateProvenance`,
//                   `m_frontierSlotAwaitingState`, `m_slotLandingSeqNr`,
//                   `m_containsCorrectTick`, `m_appliedCaptureTickBuffer`.
//     ⛔ Every one of those ALSO carries the prohibition at its own declaration. The duplication
//     is deliberate and must not be deduped to one statement plus pointers: this table is where
//     a reader ADDING a column looks, and the per-column fence is where an editor already
//     inside one lands. §7
//
//   * THE 4-METHOD EXTERNAL API — `save_snapshot` / `load_snapshot` / `advance_frame` /
//     `compute_checksum` — is Layer-1 determinism machinery. ⛔ ITS ONLY CONSUMERS ARE THE
//     CATCH2 DETERMINISM HARNESS AND THE LLTs; it has no production consumer. The comparison
//     and wire layers that would give it one were DEFERRED on 2026-08-16 by user ruling; what
//     was deferred, and what would have to land first, are recorded in §7.
//
//   * THE RESIM CYCLE that drives this class — six phases, all on the physics thread — is
//     stated once in `SimulationReconciliation.h`'s own orientation block; not re-derived here.
//
//   * THE SHIPPED RESIM TRIGGER POLICY, and the code-default-vs-shipped-config split, are
//     stated in `SimulationManager.h`; not re-derived here. This class holds only the pushed
//     copy — `setResimTriggerPolicy`. §2
//
// Relocation history, retired rationale and archived measurement records:
// `docs/CorrectionCache-rationale.md`.
// ---------------------------------------------------------------------------
//
// ⛔ `m_stateProvenance` is NOT the retired `m_isResimulated` bit returning — read
// `SlotStateProvenance.h`'s three fences and this class's gravestone before assuming so. §4
//
// ⛔ `m_slotLandingSeqNr` DECIDES NOTHING; the write rule
// (`resimGate::classifyResimSlotWrite`) reads `m_containsCorrectTick` alone. §3
//
// ⛔ TWO BITS, NOT THREE. `m_isResimulated` is retired; the gate is one
// per-character atomic word, `m_pendingResimAnchorTick`. §2
//
// ⛔ THERE IS NO INPUT VALUE IN A SLOT. `m_inputBuffer`, `getInput`, `getLatestInput`
// and `pushPredictionInput` are all gone — a stored value nothing reads goes stale silently. §1
//
// ⛔ ANY FUTURE CODE LOOKING FOR AN INPUT VALUE HERE IS LOOKING FOR SOMETHING THAT NO LONGER
// EXISTS BY DESIGN. The three live sources, by question asked:
//   "what did the local player capture at tick t?"    -> `LocalInputCache`
//   "what did a REMOTE player send for capture t?"    -> `RemoteInputCache`, via
//      `getLastRelayedInput` / `resolveScheduledRelayedInput`
//   "which capture did the AUTHORITY apply at tick t?" -> `getAppliedCaptureTick`, here
// ⛔ BOTH CACHES AND BOTH READERS BELONG TO `SimulationInputResolution`, NOT `SimulationNetSync`
// — they moved there when input resolution split out of `SimulationNetSync`. §1
// ⛔ Re-adding the column re-creates the application-vs-capture-tick ambiguity. §1
//
// ⛔ The `InputType` parameter DELIBERATELY SURVIVES the column: `IntegrateFn` and
// `advance_frame` still name it. The cache no longer STORES an input; it still integrates. §1
template <typename StateType, typename InputType>
class StateCorrectionCache
{
public:
	static constexpr size_t StateBufferSize = 60;
	static constexpr size_t InvalidCacheIndex = 1337;

	// Integrate functor for `advance_frame`: (tick, prevState, input) -> newState. §7
	using IntegrateFn = std::function<StateType(uint32, const StateType&, const InputType&)>;

	StateCorrectionCache(std::function<void(const char*)> logger)
		: m_stateBuffer()
		, m_tickBuffer()
		, m_logger(std::move(logger))
		, m_integrateFn()
	{
		m_tickBuffer.fill(0);
		m_appliedCaptureTickBuffer.fill(kNoInputCaptureTick);
		m_slotLandingSeqNr.fill(0u);
		// ⛔ `Empty` is the ONLY place "nothing written here yet" lives in data —
		// `m_tickBuffer` cannot carry it, since filling it with 0 makes every slot claim tick 0. §4
		m_stateProvenance.fill(SlotStateProvenance::Empty);
	}

	// ⛔ A cache built with the 1-arg constructor MUST NOT call `advance_frame` — `OG_CHECK`. §7
	StateCorrectionCache(std::function<void(const char*)> logger, IntegrateFn integrateFn)
		: m_stateBuffer()
		, m_tickBuffer()
		, m_logger(std::move(logger))
		, m_integrateFn(std::move(integrateFn))
	{
		m_tickBuffer.fill(0);
		m_appliedCaptureTickBuffer.fill(kNoInputCaptureTick);
		m_slotLandingSeqNr.fill(0u);
		m_stateProvenance.fill(SlotStateProvenance::Empty);
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

	// ⛔ `getInput(cacheIndex)` IS GONE with the input column, and with it the tick-0
	// phantom it carried (slot 0 claiming tick 0, then failing on its empty optional). §1

	// THE PER-TICK JOIN KEY. §10
	//
	// ⛔ `kNoInputCaptureTick` covers THREE situations: no correction landed here; the authority
	// substituted an input (the remote-input-queue underrun sentinel); or the ring recycled it. §10
	//
	// ⛔ IT LIVES IN THE SLOT, parallel to `m_tickBuffer`, not in one scalar per character — resim
	// input resolution runs for EVERY resimulated tick, so slot T's ref describes tick T. §10
	//
	// ⛔ This ref is the ONLY input-related thing a slot carries. §1
	uint32 getAppliedCaptureTick(uint32 cacheIndex) const
	{
		OG_CHECK(cacheIndex < StateBufferSize, "trying to access applied capture tick with bad cacheIndex");
		return m_appliedCaptureTickBuffer[cacheIndex];
	}

	// Has an authoritative correction landed here? ⛔ Set by `tryInsertingCorrectState`,
	// cleared by `pushPredictionTick`, `save_snapshot` and `wipeCache`. §3
	//
	// ⛔ `kNoInputCaptureTick` ALONE IS AMBIGUOUS — "landed, no capture named" versus "nothing
	// landed here". ONLY THIS BIT SEPARATES THEM; conflating them replays game-zero. §10
	bool containsCorrectTick(uint32 cacheIndex) const
	{
		OG_CHECK(cacheIndex < StateBufferSize, "trying to access correction flag with bad cacheIndex");
		return m_containsCorrectTick.test(cacheIndex);
	}

	uint32 getPredictionTick() const
	{
		// find the highest tick in the buffer
		auto maxIt = std::max_element(m_tickBuffer.begin(), m_tickBuffer.end());

		if (maxIt == m_tickBuffer.end())
			return 0;

		return *maxIt;
	}

	// Correction-miss log gating. §6
	// A miss is DISCARDED — routine and self-healing on three characterised paths: a fresh proxy,
	// the connect transient, and a post-Skip hole `backfillSkippedTick` misses. §6
	//
	// Reconciliation anchors on the newest LANDED correction, so a miss costs DELAYED
	// reconciliation, never lost reconciliation. §6
	//
	// ⚠ THE OLD COST MODEL IS FALSE AND MUST NOT BE RE-ADOPTED: corrections are not an
	// unthrottled per-tick stream. `sendCorrectionAll` writes `TimeConfig::correctionRotationK`
	// characters' buffers per tick, round-robin, so each is corrected at `tickFrequency * K / N`
	// and a miss costs up to ceil(N/K) ticks, not "at most one".
	// ⛔ DO NOT RESTATE K's VALUE HERE — `TimeConfig::correctionRotationK` and the ini's own
	// `CorrectionRotationK` block own it, and the last retune falsified the number this
	// comment used to carry. §6
	//
	// ⛔ THE GATE BELOW IS UNAFFECTED, A DERIVED FACT AND NOT AN OVERSIGHT — do not "update" it
	// for the new cadence: it compares DISTANCE FROM THE FRONTIER, which rotation does not change. §6
	//
	// ⛔ GATE: warn only beyond `rollbackWindowHardCap` from the frontier — the deepest the
	// reconciler ever resimulates, and `hardResyncThresholdTicks > rollbackWindowHardCap` means a
	// sustained miss out there is a clock that should have resynced. Closer is Verbose. §6
	//
	// ⚠ `getPredictionTick() != 0` is CONVENTIONAL, not exact — both constructors fill
	// `m_tickBuffer` with 0. The deferred UINT32_MAX sentinel would make it exact. §6
	bool isAnomalousMiss(uint32 missedTick) const
	{
		const uint32 predictionTick = getPredictionTick();

		// ⛔ No frontier yet — a registration transient. Comparing a real server tick against a
		// never-initialised frontier is MEANINGLESS, not anomalous. §6
		if (predictionTick == 0)
			return false;

		const uint32 distance = (missedTick > predictionTick)
			? (missedTick - predictionTick)
			: (predictionTick - missedTick);

		return distance > m_anomalousMissDistanceTicks;
	}

	// ⛔ Sourced from the `TimeConfig` default, NOT a literal, so R-P1 holds. It does NOT track a
	// runtime override. §6
	void setAnomalousMissDistanceTicks(uint32 distanceTicks)
	{
		m_anomalousMissDistanceTicks = distanceTicks;
	}

	// The newest-landed-correction query moved to `getDiagnostics().lastCorrectTick()`. §8

	// THE RESIM GATE — ONE WORD, EDGE-TRIGGERED. §2
	//
	// ⛔ `getLastResimulationTick()` IS GONE, with the `m_isResimulated` bitset it scanned (for
	// `m_isResimulated || m_containsCorrectTick`) and the `pushPredictionTick` inheritance that
	// fed it. It measured CLOCK ALIGNMENT, not divergence. §2
	//
	// ⛔ THE INHERITANCE WAS A LOAD-BEARING GUARD, NOT THE BUG — removing it alone produced an
	// UNTERMINATING 1-tick-resim storm. Design candidates A and B are DEAD; DO NOT RESURRECT. §2
	//
	// ⛔ `m_pendingResimAnchorTick`, 0 == none. FIVE SITES, and the set is the contract:
	//   W1 set   `tryInsertingCorrectState`'s hit path, GAME thread, when
	//            `resimGate::shouldSetPendingAnchor` says so. CAS-max, so landings COALESCE.
	//   W2 clear the resim-completion edge, PHYSICS thread — `consumeResimAnchor`, a literal CAS.
	//   W3 clear `wipeCache`, zeroed with the tick numbering it belongs to.
	//   R1 read  `needsResimulation()`, PHYSICS thread, per frame via `checkDivergenceAll`.
	//   R2 read  the prepare-time capture, PHYSICS thread.
	// ⛔ TERMINATION IS STRUCTURAL: no rescan of slot state can re-trigger a consumed correction.
	// A sixth site that derives the anchor from slot state re-opens the storm. §2
	//
	// ⚠ A TRIGGER, NOT A DATA CHANNEL. ⛔ THE ANCHOR MAKES THE GATE RACE-FREE; IT DOES NOT MAKE
	// THE CACHE RACE-FREE — the corrected STATE still travels through `m_stateBuffer`'s
	// pre-existing GT/PT race — and must never be read as if it did. §5
	//
	// ⛔ IT MUST NEVER ENTER THE DETERMINISM CHECKSUM — transient CONTROL state, which two correct
	// peers may differ on. `compute_checksum` hashes `m_stateBuffer[tick]` only; `save_snapshot` /
	// `load_snapshot` / `advance_frame` are explicit NON-SITES. §2, §7

	// R1 — the pending anchor, or 0. `checkDivergenceAll` folds it to a min across characters and
	// hands that to the physics engine as the tick to restore at. §2
	uint32 getPendingResimAnchorTick() const
	{
		return m_pendingResimAnchorTick.load(std::memory_order_acquire);
	}

	// ⛔ `getLastCorrectionInput` IS GONE, with `insertCorrectionInput` and the
	// `m_containsCorrectionInput` bitset it tested. It could only ever have returned `std::nullopt`
	// — forever, silently, while still looking authoritative at a call site. §1

	// ⛔ `getLatestInput` IS GONE with the column it read — and so is that column's only
	// writer, `pushPredictionInput`. §1
	//
	// ⛔ THE RULE THAT DECIDED IT, AND IT IS STILL THE RULE: REMOVE WHAT A CHANGE MAKES FALSE,
	// LEAVE WHAT IT MAKES MERELY UNUSED. §1
	//
	// ⚠ IT WAS ALSO THE MOST DANGEROUS ACCESSOR HERE FOR A REMOTE CHARACTER: it answered the
	// CLIENT'S OWN PREDICTION while looking server-sourced. The correct remote source is
	// `SimulationInputResolution::getLastRelayedInput`, and that class holds the `LocalInputCache`
	// too — `SimulationNetSync` holds neither. §1

	// ⛔ THE GATE, shape unchanged DELIBERATELY: the second clause makes a frontier landing wait
	// for the frontier to move, which is the legacy timing `FrontierExact` must reproduce. §2
	//
	// `const` because it no longer computes anything — which is what lets `findCorrectionCache`'s const
	// route ask the gate directly. §2
	bool needsResimulation() const
	{
		const uint32 anchorTick = getPendingResimAnchorTick();
		return anchorTick != 0u && anchorTick != getPredictionTick();
	}

	// W2 — CONSUMPTION, on the resim completion edge (beside `applyResimAll`), with the anchor
	// this resim was PREPARED with. §2
	//
	// ⭐ THE CAS IS THE MECHANISM, NOT A FLOURISH: a compare-then-store could be overtaken by W1
	// writing a NEWER anchor and store 0 over it — the original defect in miniature. §2
	//
	// ⛔ `preparedAnchorTick == 0` means this resim consumed nothing; it can never match a live
	// anchor, so it is rejected up front rather than CAS-ed against the "none" sentinel. §2
	bool consumeResimAnchor(uint32 preparedAnchorTick)
	{
		if (preparedAnchorTick == 0u)
			return false;

		uint32 expected = preparedAnchorTick;
		return m_pendingResimAnchorTick.compare_exchange_strong(
			expected, 0u, std::memory_order_acq_rel, std::memory_order_acquire);
	}

	// R2 — the prepare-time capture, and its consume partner. §2
	//
	// ⛔ THE CAPTURE IS PER CACHE, NOT ONE VALUE AT THE MANAGER: a resim is prepared from the MIN
	// anchor (`checkDivergenceAll` folds with `std::min`), so a shared expected value leaves
	// newer-anchored characters pending and re-triggers next frame — a treadmill. §2
	//
	// ⛔ A PLAIN `uint32` BESIDE AN ATOMIC IS NOT AN OVERSIGHT: `m_consumeExpectedAnchorTick` is
	// PHYSICS-THREAD-PRIVATE, written by `prepareResimAll` and read on the completion edge; item
	// 45's one-atomic-word fence governs the two-writer GATE. §2
	//
	// ⛔ IT ALSO CAPTURES `m_preparedLandingSeqNr` IN THE SAME INSTANT — capturing the
	// two arms of `classifyResimSlotWrite` apart lets a landing be counted in NEITHER population. §3
	//
	// ⛔ PT-private in the same way, and it READS a game-thread-written counter — see
	// `m_slotLandingSeqNr`'s declaration. It feeds a COUNTER, never a decision. §3
	void captureResimAnchorForConsume()
	{
		m_consumeExpectedAnchorTick = getPendingResimAnchorTick();
		m_preparedLandingSeqNr        = m_landingSeqNr;
	}

	// ⛔ THE WRITE SIDE STAYS HERE: `captureResimAnchorForConsume` and
	// `setResimTriggerPolicy` did not move with the reads into `getDiagnostics()` /
	// `editDiagnostics()` — performing the act is production, observing it is diagnostic. §8

	bool consumeCapturedResimAnchor() { return consumeResimAnchor(m_consumeExpectedAnchorTick); }

	// ⛔ THE SOURCE OF TRUTH IS `TimeConfig::resimTriggerPolicy`; this is the pushed copy W1
	// consults. Default from the TimeConfig default, not a literal, so R-P1 holds. §2
	//
	// ⛔ GAME-THREAD-ONLY IN EFFECT — written at composition, read at W1 — which is why it is not
	// atomic; a runtime cvar would break that. Shipped policy: `SimulationManager.h`. §2
	//
	// The read-back moved to `getDiagnostics()`; this setter is production-called and stays. §8
	void setResimTriggerPolicy(TimeConfig::ResimTriggerPolicy policy)
	{
		m_resimTriggerPolicy = policy;
	}

	void pushPredictionTick(uint32 tick)
	{
		const uint32 predictionTick = getPredictionTick();

		OG_CHECK(tick >= predictionTick, "Setting bad prediction tick");

		if (tick == predictionTick)
			return; // already at this tick (e.g. clock stall); no new slot needed

		// ⛔ THE FRONTIER-PAIR DETECTOR fires iff the PREVIOUS allocation never received its
		// `pushPredictionState`. Full contract at `m_frontierSlotAwaitingState`'s declaration. §9
		OG_CHECK(!m_frontierSlotAwaitingState,
			"frontier pairing broken: the previous frontier slot was allocated and never received pushPredictionState");
		m_frontierSlotAwaitingState = true;

		const uint32 predictionIndex = getCacheIndex(predictionTick);

		uint32 newPredictionIndex = (predictionIndex + 1) % StateBufferSize;
		m_tickBuffer[newPredictionIndex] = tick;

		m_containsCorrectTick[newPredictionIndex] = false;
		m_predictionWasCorrect[newPredictionIndex] = false;
		// ⛔ THE INHERITANCE LINE IS GONE FROM HERE:
		//     m_isResimulated[newPredictionIndex] = m_isResimulated[predictionIndex];
		// DELETED BECAUSE THE READER IS GONE, NOT BECAUSE THE GUARD WAS WRONG —
		// `getLastResimulationTick` was `m_isResimulated`'s only reader. ⛔ THE PROPERTY TO PROTECT
		// HERE: frontier advance touches NO gate state at all. An edit that makes `pushPredictionTick`
		// write the anchor puts derived-state re-triggering back, and the storm with it. §2
		//
		// ⛔ The recycled slot describes a DIFFERENT tick, so its join key must retire with it, or
		// a resim through the fresh tick resolves remote input from a ~60-tick-old capture and is
		// confidently wrong rather than merely uninformed. §10
		m_appliedCaptureTickBuffer[newPredictionIndex] = kNoInputCaptureTick;
		// ⛔ AND SO MUST THE LANDING STAMP, retired alongside `m_containsCorrectTick`. A
		// stale HIGH stamp cannot MIS-PROTECT, but would report a protection that never happened. §3
		m_slotLandingSeqNr[newPredictionIndex] = 0u;
		// ⛔ WRITE SITE 1 of 5 — RING RECYCLE. New tick, no state yet, so the lineage
		// retires with the rest of the bookkeeping. §4
		//
		// ⛔ THE ALLOCATION HAPPENS HERE, BEFORE THE UPDATE, AND CANNOT SLIDE
		// TO CAPTURE TIME: `m_tickBuffer` is both the slot directory and the frontier
		// (`getPredictionTick()` is max over it), and FOUR concurrent GT reads decide against that word
		// while the physics step runs — the slot lookup, the verdict compare, `landedAtFrontier`
		// feeding `shouldSetPendingAnchor`, and `isAnomalousMiss`'s distance gate. ⛔ ALLOCATION TIMING
		// IS TRIGGER TIMING: allocating later re-times the predicate the `FrontierExact` baselines are
		// defined against and turns mid-update frontier landings into discards. §9
		//
		// ⛔ `Predicted`, NOT `Empty` — `pushPredictionTick` is always immediately followed by
		// `pushPredictionState`, so `Empty` would be a stale lie for the whole ring pass. `Empty`
		// belongs to a slot never written at all: the constructors and `wipeCache`. §4
		//
		// ⛔ AND UNLIKE THE LINE IT SITS BESIDE, IT INHERITS NOTHING. Writing a CONSTANT is what keeps
		// "frontier advance touches no gate state" true. §2, §4
		m_stateProvenance[newPredictionIndex] = SlotStateProvenance::Predicted;
	}

	// ⛔ `pushPredictionInput` IS GONE — the input column's only writer, whose two call sites
	// were both arms of what is now `SimulationInputResolution`. This class's own
	// `pushPredictionTick` now reaches it from `SimulationReconciliation::allocateFrontierSlotsAll`,
	// `backfillSkippedTick` and `advance_frame`. ⛔ The ring's slot allocation is unchanged. §1

	void pushPredictionState(const StateType& state)
	{
		const uint32 predictionIndex = getCacheIndex(getPredictionTick());
		m_stateBuffer[predictionIndex] = state;
		// ⛔ COMPLETES the frontier pair. NO CHECK ON THIS SIDE, DELIBERATELY — bare state
		// pushes are legal, so the one-sided check lives on `pushPredictionTick`. §9
		m_frontierSlotAwaitingState = false;
	}

	// ⛔ TRUE MEANS THE SLOT WAS FOUND — purely observational; every existing call site
	// ignores it and is byte-identical by construction. §3
	//
	// ⛔ IT HAS TO LEAVE THE FUNCTION because `replayOverruns` needs a DENOMINATOR and a log line
	// cannot be counted by its emitter. The cache reports; `postResimulationAll` tallies. §3
	//
	// ⛔ IT NO LONGER WRITES UNCONDITIONALLY — A REPLAY NEVER OVERWRITES A CORRECTED SLOT. §3
	//
	// ⛔ THE RULE AND THE FRESH/STALE CLASSIFIER ARE ONE BLOCK, `resimGate::classifyResimSlotWrite`
	// in `ResimGatePolicy.h`. READ IT BEFORE CHANGING ANYTHING HERE: authority state beats a
	// re-derivation, so `m_containsCorrectTick` is left ALONE — never cleared. §3
	//
	// ⚠ THE INVARIANT'S HONEST BOUND — it holds UP TO AN ACKNOWLEDGED WORD RACE.
	// ⛔ `m_containsCorrectTick` IS A TWO-WRITER WORD — the GT `set()` here and the PT clear in
	// `pushPredictionTick` — so the exposure is a LOST UPDATE OF A DECISION BIT, NOT a torn read,
	// and the milder description must not be re-adopted. The deliberately-redundant provenance
	// check (`ReplayedOverCorrection`, WRITE SITE 3 below) catches it ONLY on the
	// `[ResimProbe.SlotMap]` Verbose surface, never on this decision path. §5
	//
	// ⚠ CALIBRATION: a few-instruction window per event, self-healing at the next landing, hence
	// rare. ⛔ THE ARCHITECTURAL POINT STANDS REGARDLESS — an accepted-tear price list priced torn
	// VALUES; a lost DECISION BIT is a different line item. §5
	//
	// ⛔ THE RETURN VALUE STILL MEANS "THE SLOT EXISTED", NOT "I WROTE": re-pointing
	// `replayOverruns` at protections would silently redefine an ARCHIVED baseline. §3
	//
	// ⛔ `outOutcome` is the same defaulted, observational out-pointer `tryInsertingCorrectState`
	// uses — every call site that predates it stays byte-identical by construction. §3
	bool tryInsertingResimulatedState(StateType&& state, uint32 tick,
	                                  resimGate::ResimSlotWriteOutcome* outOutcome = nullptr)
	{
		if (outOutcome != nullptr)
			*outOutcome = resimGate::ResimSlotWriteOutcome::Discarded;

		auto it = std::find(m_tickBuffer.begin(), m_tickBuffer.end(), tick);
		if (it != m_tickBuffer.end())
		{
			uint32 cacheIndex = std::distance(m_tickBuffer.begin(), it);

			// ⛔ ONE CALL DECIDES BOTH THE ACTION AND ITS LABEL. A separate write predicate was
			// TRIED AND REJECTED — a mutation run overwrote the slot while still reporting
			// `ProtectedFresh`, a counter that disagreed with reality. §3
			const resimGate::ResimSlotWriteOutcome outcome = resimGate::classifyResimSlotWrite(
				m_containsCorrectTick[cacheIndex],
				m_slotLandingSeqNr[cacheIndex],
				m_preparedLandingSeqNr,
				tick,
				m_consumeExpectedAnchorTick);

			if (outOutcome != nullptr)
				*outOutcome = outcome;

			if (outcome != resimGate::ResimSlotWriteOutcome::Written)
			{
				// ⛔ PROTECTED: the replay's state is discarded, the authority state stands. NO LOG LINE — that
				// would re-create the log-volume defect; counted on `[ResimProbe.Apply]` instead. §3
				//
				// ⛔ AND THE PROVENANCE COLUMN IS LEFT ALONE TOO, which is why a protected slot shows
				// `A`/`C` rather than `R`: the state did not change, so neither did its `SlotStateProvenance`. §4
				return true;
			}

			// ⛔ WRITE SITE 3 of 5 — THE REPLAY WRITE, and the ONE site that can stamp the alarm. §4
			//
			// ⭐ THE CHECK IS DELIBERATELY REDUNDANT WITH `classifyResimSlotWrite` ABOVE, AND THE
			// REDUNDANCY IS THE INSTRUMENT: that call decided this write on `m_containsCorrectTick`; this
			// asks a SECOND, INDEPENDENT source — `isAuthorityGradeProvenance` on the provenance column —
			// whether that bit was telling the truth. ⛔ UNDER THE PROTECT-ALL RULE THE TWO CAN NEVER
			// DISAGREE (`ProtectedFresh` / `ProtectedStale` short-circuit above at exactly the population
			// `isAuthorityGradeProvenance` marks), SO THIS BRANCH IS UNREACHABLE AND
			// `ReplayedOverCorrection` READS ZERO FOREVER. §4
			//
			// ⛔ THAT IS NOT A REASON TO DELETE IT, AND COVERAGE TOOLING WILL SAY OTHERWISE. The
			// pre-protect-all clobber was real, shipped and invisible for months for one reason: nothing could
			// represent it. A regression that re-opens it now stamps an `X` in the slot map instead of
			// hiding. Collapsing the two sources into one deletes the alarm and leaves a comment claiming
			// it exists. §4
			//
			// It reads no state it did not already have and changes NO decision — the write below happens
			// either way. §4
			m_stateProvenance[cacheIndex] =
				isAuthorityGradeProvenance(m_stateProvenance[cacheIndex])
					? SlotStateProvenance::ReplayedOverCorrection
					: SlotStateProvenance::Replayed;

			m_stateBuffer[cacheIndex] = std::move(state);
			// ⛔ `m_isResimulated.set(cacheIndex)` IS GONE FROM HERE, and this is the site that
			// makes the storm structurally impossible rather than merely unlikely: REPLAY WRITES STATE AND
			// PROVENANCE, NEVER GATE STATE. Under the level-triggered gate this line re-closed the gate
			// after a resim and, via the frontier inheritance, shadowed every behind-frontier correction.
			// The gate is now closed by an explicit CAS at `consumeResimAnchor`. §2
			if (m_logger)
			{
				char buf[128];
				std::snprintf(buf, sizeof(buf),
					"insertResimulatedState: tick=%u cacheIndex=%u", tick, cacheIndex);
				m_logger(buf);
			}
			return true;
		}
		else
		{
			// Tick not in the cache window — too late or too early; discard.
			if (m_logger)
			{
				char buf[128];
				std::snprintf(buf, sizeof(buf), "[Warning] tryInsertingResimulatedState: tick=%u not in cache window, discarding", tick);
				m_logger(buf);
			}
			return false;
		}
	}

	// ⛔ `appliedCaptureTick` is the per-tick join key. It DEFAULTS to the sentinel so every earlier
	// call site keeps its exact prior meaning — "no ref known for this tick". §10
	//
	// ⛔ Stored on the HIT path REGARDLESS of the verdict: the ref describes what the AUTHORITY
	// did, equally true whether or not the prediction matched. §10
	//
	// ⛔ `outDiagnosticVerdict` is DEFAULTED and PURELY OBSERVATIONAL — nothing about this
	// method's behaviour depends on whether it is supplied. §10
	//
	// ⛔ THE MARKER NAMES THE CHANNEL, NOT THE FACT (`docs/DiagnosticsConventions.md` §4).
	// `predictionWasCorrect` below, computed by `isSimilarTo`, feeds
	// `resimGate::shouldSetPendingAnchor` directly — under the shipped trigger policy that boolean
	// decides whether a resim runs at all. `outDiagnosticVerdict` only carries a COPY of that fact
	// out for reporting; it is not the path the decision takes. DELETING `isSimilarTo` SILENTLY
	// DISABLES THE RESIM GATE.
	void tryInsertingCorrectState(StateType&& state, uint32 tick,
	                              uint32 appliedCaptureTick = kNoInputCaptureTick,
	                              CorrectionInsertVerdict* outDiagnosticVerdict = nullptr)
	{
		if (outDiagnosticVerdict != nullptr)
			*outDiagnosticVerdict = CorrectionInsertVerdict{ false, false, tick };

		auto it = std::find(m_tickBuffer.begin(), m_tickBuffer.end(), tick);
		if (it != m_tickBuffer.end())
		{
			uint32 cacheIndex = std::distance(m_tickBuffer.begin(), it);

			const bool predictionWasCorrect = m_stateBuffer[cacheIndex].isSimilarTo(state);

			m_containsCorrectTick.set(cacheIndex);
			m_predictionWasCorrect[cacheIndex] = predictionWasCorrect;

			// ⛔ THE LANDING STAMP, bumped HERE AND ONLY HERE, and stamped on BOTH verdicts — the
			// protection rule (`resimGate::classifyResimSlotWrite`) makes no verdict distinction. §3
			m_slotLandingSeqNr[cacheIndex] = ++m_landingSeqNr;

			// ⛔ WRITE SITE 2 of 5 — and the one that carries the column's first payload value. §4
			//
			// ⭐ THE BRANCH IS THE VERDICT BRANCH TAKEN TWENTY LINES DOWN, and this is the point of an ENUM
			// rather than a bool: `m_containsCorrectTick` is set for BOTH verdicts, so it cannot say WHICH
			// KIND. An AGREEING landing (`AuthorityAgreedKeptPrediction`) is authority-grade while holding
			// the PREDICTED value; a disagreeing one is `AuthorityAdopted`. §4
			//
			// ⛔ MIRROR THE `if (!predictionWasCorrect)` BELOW IF IT EVER MOVES — one decision expressed
			// twice, asserted together on both arms by
			// `CorrectionCache.ResimGate.ADisagreeingLandingAdoptsAuthorityAndAnAgreeingOneCertifiesThePrediction`. §4
			m_stateProvenance[cacheIndex] = predictionWasCorrect
				? SlotStateProvenance::AuthorityAgreedKeptPrediction
				: SlotStateProvenance::AuthorityAdopted;

			// W1 — THE ONE EVENT THAT OPENS THE RESIM GATE. §2
			//
			// ⛔ This replaces `m_isResimulated[cacheIndex] = false;`, the old un-shadow, which re-opened
			// the gate only when the landing slot HAPPENED to be the frontier. §2
			//
			// ⛔ `landedAtFrontier` is the SAME comparison `classifyCorrectionLanding` makes, which is what
			// makes the probe's ARCHIVED `atFrontier` counts this policy's baseline. §2
			const bool landedAtFrontier = (tick == getPredictionTick());
			if (resimGate::shouldSetPendingAnchor(
					m_resimTriggerPolicy, landedAtFrontier, predictionWasCorrect))
			{
				raisePendingResimAnchorTo(tick);
			}

			m_appliedCaptureTickBuffer[cacheIndex] = appliedCaptureTick;

			// Reported before the state move for locality only; the ordering is not load-bearing.
			if (outDiagnosticVerdict != nullptr)
				*outDiagnosticVerdict = CorrectionInsertVerdict{ true, predictionWasCorrect, tick };

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
			// Discard. ⛔ Severity gated by `isAnomalousMiss`; `predictionTick` is printed so a Warning
			// from here is actionable on its own. §6
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

	// ⛔ `insertCorrectionInput` IS GONE — the SERVER->CLIENT correction-input channel's
	// terminus, with `SimulationReconciliation::injectCorrectionInput` and the OnRep-bound callback
	// `registerPredictionOwner` installed. The severity gate it shared with
	// `tryInsertingCorrectState` is unaffected: that gate is `isAnomalousMiss`, and it is still
	// exercised through the state path. §1
	//
	// ⛔ The input COLUMN `m_inputBuffer` and its writer `pushPredictionInput`, which the
	// correction-input retirement deliberately left standing, ARE NOW GONE TOO. §1

	void wipeCache(unsigned int newPredictionTick)
	{
		unsigned int predictionIndex = getCacheIndex(getPredictionTick());
		m_tickBuffer.fill(0);
		m_containsCorrectTick.reset();
		m_predictionWasCorrect.reset();
		// ⛔ W3 — THE ANCHOR DIES WITH THE TICK NUMBERING: a survivor names a tick that no
		// longer exists, at worst one permanently ABOVE the frontier that the gate can NEVER close.
		// Same retirement discipline as `m_appliedCaptureTickBuffer` below. §2
		//
		// ⚠ THREAD: this runs on the PHYSICS thread — the clock invokes the resync callback inline from
		// `advancePrediction`, reaching `m_reconciliation.wipeAllForResync` — so W3 races only against
		// W1 and the atomic store handles it. §5
		m_pendingResimAnchorTick.store(0u, std::memory_order_release);
		m_consumeExpectedAnchorTick = 0u;
		// ⛔ The stamps die with the numbering too, or the classifier compares an old
		// landing against a new resim's capture and calls an ancient slot FRESH. §3
		m_slotLandingSeqNr.fill(0u);
		m_landingSeqNr         = 0u;
		m_preparedLandingSeqNr = 0u;
		// ⛔ WRITE SITE 4 of 5 — THE WIPE, and the one place `Empty` is written after
		// construction. The frontier slot is renumbered below and stays `Empty` — `wipeCache` sets its
		// TICK, never its state; `pushPredictionState` fills it. §4
		m_stateProvenance.fill(SlotStateProvenance::Empty);
		// ⛔ Every surviving join key describes a numbering the resync invalidated — same reasoning
		// as `SimulationInputResolution::wipeAllForResync`, which owns the input containers. §10
		m_appliedCaptureTickBuffer.fill(kNoInputCaptureTick);

		m_tickBuffer[predictionIndex] = newPredictionTick;

		// ⛔ THE DETECTOR DIES WITH THE NUMBERING TOO — `wipeCache` resets it. This is
		// coverage blind spot (iii): an allocation abandoned before this wipe is SWALLOWED. §9
		m_frontierSlotAwaitingState = false;
	}

	// THE 4-METHOD EXTERNAL API. ⛔ Consumers: the Catch2 determinism harness and the LLTs, nothing
	// in production — see the orientation block. §7

	// Writes `state` into the cache slot for `tick`.
	//
	// ⛔ SLOT COLLISION: a `tick` already present is updated IN PLACE, never duplicated; otherwise
	// the slot is `tick % StateBufferSize`. A fresh slot resets its bookkeeping — mirroring
	// `pushPredictionTick` — so stale correction metadata cannot leak. §7
	//
	// ⛔ AND IT DELIBERATELY DOES NOT TOUCH `m_pendingResimAnchorTick` — an explicit
	// NON-SITE: snapshot ORDER would decide whether a resim fires, and `compute_checksum` would
	// hash identical simulations differently. §2, §7
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
			// ⛔ `m_isResimulated[slot] = false;` is gone with the bitset. Fresh slot — no
			// correction has named a join key for this tick yet, and leaving it `Empty` would claim no
			// state had been written into a slot this path is about to write. §1
			m_appliedCaptureTickBuffer[slot] = kNoInputCaptureTick;
			// ⛔ Same reset as `pushPredictionTick`'s. NOTE the API touches NO gate state and NO
			// landing COUNTER: `m_landingSeqNr` and the anchor are session control state. §7
			m_slotLandingSeqNr[slot] = 0u;
			// ⛔ WRITE SITE 5 of 5 — THE FRESHLY ALLOCATED HARNESS SLOT. §4
			//
			// ⭐ WHY THIS API WRITES PROVENANCE WHILE IT DELIBERATELY DOES NOT WRITE THE ANCHOR — THE TWO
			// LOOK LIKE THE SAME KIND OF EXCEPTION AND ARE NOT, and this is the nearest thing to the rule
			// for a SIXTH column. THE ANCHOR IS A LIVE TRIGGER: writing it here would let snapshot ORDER
			// decide whether a resim fires. PROVENANCE DESCRIBES STATE LINEAGE, and the harness
			// legitimately CREATES state — `save_snapshot` is the externally-driven twin of
			// `pushPredictionTick` + `pushPredictionState`, so the honest answer to "where did this state
			// come from" is the same one that path gives. §4, §7
			//
			// ⛔ Neither write can perturb a simulated value: the anchor is excluded because it WOULD, and
			// provenance is admitted because it CANNOT. §7
			m_stateProvenance[slot] = SlotStateProvenance::Predicted;
		}
		else if (tick == getPredictionTick())
		{
			// ⛔ COMPLETES THE FRONTIER PAIR ON THIS PATH TOO — full contract at
			// `m_frontierSlotAwaitingState`'s declaration. §9
			//
			// ⛔ GUARDED ON `tick == getPredictionTick()`, DELIBERATELY NOT an unconditional clear like
			// `pushPredictionState`'s: this method's `tick` is caller-supplied and `getCacheIndex` scans
			// the WHOLE ring, so clearing unconditionally would swallow a still-open pairing and arm a
			// spurious `OG_CHECK`. §9
			m_frontierSlotAwaitingState = false;
		}

		// ⚠ AN EXISTING SLOT KEEPS ITS OLD PROVENANCE WHILE ITS STATE IS REPLACED — KNOWN,
		// PRE-EXISTING and harness-only, recorded rather than repaired: `m_containsCorrectTick` and
		// the applied-capture ref have always behaved this way. ⛔ It does NOT produce
		// `ReplayedOverCorrection`, which the replay path alone stamps. §4
		m_stateBuffer[slot] = state;
	}

	// Returns true and fills `out_state` if `tick` is in the cache window. §7
	[[nodiscard]] bool load_snapshot(uint32 tick, StateType& out_state) const
	{
		const uint32 cacheIndex = getCacheIndex(tick);
		if (cacheIndex == InvalidCacheIndex)
			return false;

		out_state = m_stateBuffer[cacheIndex];
		return true;
	}

	// Drives one externally-triggered sim step. ⛔ `OG_CHECK`-fails without an integrate functor. §7
	//
	// ⛔ `input` is CONSUMED, not stored: it feeds `m_integrateFn` and nothing else, which is
	// why the `InputType` parameter outlives the column. §1
	void advance_frame(uint32 tick, const InputType& input)
	{
		OG_CHECK(static_cast<bool>(m_integrateFn),
			"advance_frame called on a cache with no integrate functor (use the 2-arg constructor)");

		const uint32 prevIndex = getCacheIndex(getPredictionTick());
		OG_CHECK(prevIndex != InvalidCacheIndex, "advance_frame: previous prediction tick not in cache");

		// ⛔ Integrate into a fresh value BEFORE mutating the ring — `prevState` is a reference into
		// `m_stateBuffer` and must stay valid through the call. §7
		StateType newState = m_integrateFn(tick, m_stateBuffer[prevIndex], input);

		pushPredictionTick(tick);
		pushPredictionState(newState);
	}

	// CRC-32 over the serialized bytes at `tick`; 0 with a logger warning on a miss. §7
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

	// DIAGNOSTIC VIEWS. ⛔ The grouping rule these follow is
	// centralised in `docs/DiagnosticsConventions.md` — DO NOT RE-DERIVE IT HERE. §8
	//
	// ⛔ NINE accessors sharing one property: deleting them and every caller changes no production
	// behaviour and no shipped telemetry — EIGHT read seams on the const view and ONE test-only
	// mutator on the non-const one. §8
	//
	//     cache.getDiagnostics().slotLandingSeqNr(i)
	//     cache.editDiagnostics().scribbleStateProvenanceForFenceTest(seed)
	//
	// ⛔ THE VIEW'S TYPE IS THE MARKER: members inside carry no `get` prefix and no `Diagnostic`
	// infix. §8
	//
	// ⛔ THE WRITE SIDE STAYS ON THE CACHE — NEVER GROUP IT. Three of these have a production write
	// partner a "move everything related" pass would sweep in by mistake:
	//     resimTriggerPolicy (read)      moved | `setResimTriggerPolicy`        STAYS (config path)
	//     capturedResimAnchorTick (read) moved | `captureResimAnchorForConsume` STAYS (PT, production)
	//     capturedLandingSeqNr (read)    moved | (the same capture call)        STAYS
	// ⛔ `isAnomalousMiss` and `getPendingResimAnchorTick` are NOT in the view for the same reason:
	// both are production-read. §8
	//
	// A nested class already reaches the enclosing template's privates, so no friend declaration
	// is needed. §8
	class Diagnostics
	{
	public:
		explicit Diagnostics(const StateCorrectionCache& cache) : m_cache(cache) {}

		// ⛔ READ THE FULL THREADING CONTRACT at `m_slotLandingSeqNr`'s declaration (private
		// section, below this class) BEFORE relying on this for anything but a diagnostic. §3
		uint32 slotLandingSeqNr(uint32 cacheIndex) const
		{
			OG_CHECK(cacheIndex < StateBufferSize, "trying to access landing seq with bad cacheIndex");
			return m_cache.m_slotLandingSeqNr[cacheIndex];
		}

		// The counter itself — the value the NEXT landing will exceed. §3
		uint32 landingSeqNr() const { return m_cache.m_landingSeqNr; }

		// ⛔ Diagnostics and tests only; production reads it once, inside the classifier call
		// in `tryInsertingResimulatedState`. Write side: `captureResimAnchorForConsume`. §3, §8
		uint32 capturedLandingSeqNr() const { return m_cache.m_preparedLandingSeqNr; }

		// R2's diagnostic read. Write side: `captureResimAnchorForConsume`, on the cache. §2, §8
		uint32 capturedResimAnchorTick() const { return m_cache.m_consumeExpectedAnchorTick; }

		// "Which is the newest tick an authoritative correction has LANDED in?" §2
		//
		// ⛔ ITS FORMER PRODUCTION READER IS GONE — `getLastResimulationTick`'s retired scan. It is KEPT
		// because it STORES nothing: it derives from `m_containsCorrectTick` and cannot go stale.
		// ⛔ The resim ANCHOR is a DECIDED TRIGGER, not "the newest correction". §2
		//
		// ⛔ 0 production callers, DELIBERATELY left a test-only seam: the resim-gate
		// probe needed a wiring test because it is a COUNTER FED BY SEPARATE WRITE SITES; this is a
		// pure derivation with nothing upstream to wire. §8
		uint32 lastCorrectTick() const
		{
			const uint32 predictionTick = m_cache.getPredictionTick();
			const uint32 predictionIndex = m_cache.getCacheIndex(predictionTick);

			// iterate backwards from the prediction index to find the last correct tick
			for (int32 offset = 0; offset < StateBufferSize; ++offset)
			{
				int32 checkIndex = static_cast<int32>(predictionIndex) - offset;
				if (checkIndex < 0)
					checkIndex += StateBufferSize;
				if (m_cache.m_containsCorrectTick.test(checkIndex))
				{
					return m_cache.m_tickBuffer[checkIndex];
				}
			}

			return 0;
		}

		// The trigger-policy read-back. Write side: `setResimTriggerPolicy`, on the cache. §2, §8
		TimeConfig::ResimTriggerPolicy resimTriggerPolicy() const { return m_cache.m_resimTriggerPolicy; }

		// THE PER-SLOT STATE PROVENANCE — DIAGNOSTIC-ONLY. §4
		//
		// ⛔ THE FULL CONTRACT, THE THREE FENCES AND THE ⚠ AGAINST MISTAKING THIS FOR THE RETIRED
		// `m_isResimulated` BIT ARE IN `SlotStateProvenance.h`. READ THAT BLOCK before changing any of
		// the five write sites, and READ THE GRAVESTONE at the bottom of this class BEFORE ADDING A
		// SIXTH COLUMN. §4
		//
		// ⛔ Written at FIVE sites, read by NOTHING IN PRODUCTION. Complete reader list: the
		// `[CorrectionCache][ResimGate][Provenance]` LLTs and the Verbose `[ResimProbe.SlotMap]` line
		// (`logSlotProvenanceAll`). §4
		//
		// ⭐ THE INDEPENDENCE IS MACHINE-CHECKED, and the case is NAMED HERE so it cannot be quietly
		// dropped:
		//     CorrectionCache.ResimGate.TheProvenanceColumnCannotReachAnyProductionOutput
		// It garbage-fills this column mid-lifecycle and asserts every production output — gate,
		// anchor, captures, verdicts, replay outcomes, adopted state and the CHECKSUM — byte-identical.
		// ⛔ THAT CASE IS THE FENCE: it makes the original defect fail a test rather than contradict a
		// comment. §4
		//
		// ⚠ THREADING: GT- and PT-written plain bytes on the pre-existing `m_stateBuffer` race. A
		// diagnostic read TOLERATES tearing — nothing decides on it. §5
		//
		// ⛔ IT NEVER ENTERS `compute_checksum` — the anchor's prohibition, verbatim: two peers must
		// agree on the STATE and may disagree on how each got there. §7
		SlotStateProvenance stateProvenance(uint32 cacheIndex) const
		{
			OG_CHECK(cacheIndex < StateBufferSize, "trying to access state provenance with bad cacheIndex");
			return m_cache.m_stateProvenance[cacheIndex];
		}

		// Read seam for the frontier-pair detector — full contract at
		// `m_frontierSlotAwaitingState`'s declaration (private section, BELOW this class). §9
		bool frontierSlotAwaitingState() const { return m_cache.m_frontierSlotAwaitingState; }

	private:
		const StateCorrectionCache& m_cache;
	};

	class MutableDiagnostics
	{
	public:
		explicit MutableDiagnostics(StateCorrectionCache& cache) : m_cache(cache) {}

		// ⛔ THE FENCE'S OWN INSTRUMENT. IT HAS NO PRODUCTION CALLER AND MUST NEVER ACQUIRE ONE — the
		// name says so at every call site on purpose. §4
		//
		// Fills the provenance column with deterministic garbage — every slot's lineage a LIE, every
		// other column untouched. ⛔ TWO CASES USE IT, and both names are the only grep handle:
		//
		//   1 `CorrectionCache.ResimGate.TheProvenanceColumnCannotReachAnyProductionOutput` — fence 2.
		//     If any production output moves, some production path is reading this column.
		//   2 `CorrectionCache.ResimGate.ReplayedOverCorrectionIsUnreachableButTheAlarmIsWired` — FORGES
		//     the guard-failure precondition (authority-grade provenance over a CLEAR
		//     `m_containsCorrectTick`) that protect-all makes unreachable, so the alarm value is proven
		//     LIVE rather than merely asserted absent. §4
		//
		// ⛔ The cycle stays INSIDE the enumeration deliberately — an out-of-range byte would be UB on
		// `slotStateProvenanceChar`'s switch. It walks all `kSlotStateProvenanceCount` values. §4
		void scribbleStateProvenanceForFenceTest(uint32 seed)
		{
			for (size_t i = 0; i < StateBufferSize; ++i)
			{
				m_cache.m_stateProvenance[i] = static_cast<SlotStateProvenance>(
					static_cast<std::uint8_t>((seed + static_cast<uint32>(i)) % kSlotStateProvenanceCount));
			}
		}

	private:
		StateCorrectionCache& m_cache;
	};

	Diagnostics getDiagnostics() const { return Diagnostics(*this); }
	MutableDiagnostics editDiagnostics() { return MutableDiagnostics(*this); }

private:
	// W1's CAS-MAX — the anchor is the NEWEST tick anybody asked to resimulate from. §2
	//
	// ⛔ NEWEST-CORRECTED COALESCING IS DELIBERATE, not a shortcut around a queue: the newest
	// corrected tick SUBSUMES every older one, so several landings cost ONE resim. §2
	//
	// ⛔ A CAS LOOP, not `fetch_max` and not a compare-then-store: a read-then-write could RESURRECT
	// an anchor W2 just consumed. ⛔ `tick == 0` can never become an anchor — it is the "none"
	// sentinel AND the reserved pre-sim tick. §2
	void raisePendingResimAnchorTo(uint32 tick)
	{
		uint32 current = m_pendingResimAnchorTick.load(std::memory_order_acquire);
		while (tick > current)
		{
			if (m_pendingResimAnchorTick.compare_exchange_weak(
					current, tick, std::memory_order_acq_rel, std::memory_order_acquire))
				return;
			// Serializes a state into the checksum byte sink. §7
		}
	}

	// Project serializer where available (determinism); raw-byte fallback for POD test types. §7
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
	// ⛔ `m_inputBuffer` — one optional input composite per slot, per character, per client —
	// IS GONE FROM HERE. §1
	std::array<uint32, StateBufferSize> m_tickBuffer;
	// Per-slot join key — see `getAppliedCaptureTick`. Parallel to `m_tickBuffer`;
	// `kNoInputCaptureTick` wherever unknown. §10
	std::array<uint32, StateBufferSize> m_appliedCaptureTickBuffer;

	// THE PER-SLOT LANDING STAMP. ⛔ 0 means no correction has EVER landed here. §3
	//
	// ⛔ THIS CONTRACT BELONGS TO THE MEMBER, NOT THE RELOCATED ACCESSOR, which is why it did
	// not travel into `Diagnostics::slotLandingSeqNr`. §8
	//
	// ⚠ THREADING — THE ONE NEW CROSS-THREAD SURFACE THE LANDING STAMPS ADD. GT-written (W1), PT-read (the
	// classifier), PLAIN `uint32`s deliberately:
	//   * THEY RIDE THE PRE-EXISTING `m_stateBuffer` RACE they exist to protect — the same race,
	//     not a new class of one. §5
	//   * THEY ARE OBSERVATIONAL: `classifyResimSlotWrite` returns `Written` iff
	//     `!slotContainsCorrectTick`, REGARDLESS of the stamps. ⛔ That one input,
	//     `m_containsCorrectTick`, is TWO WRITERS ON ONE WORD — the GT `set()` in
	//     `tryInsertingCorrectState` and the PT clear in `pushPredictionTick` — so a LOST UPDATE,
	//     not a torn read. A torn stamp mis-labels a COUNTER; it can never mis-decide a write.
	//     (`ResimGatePolicyTest` sweeps that one-bit property as its own section.) §5
	//   * ⛔ THEY ARE NOT GATE STATE. `needsResimulation()` does not read them and must never be
	//     made to — growing the GATE (`m_pendingResimAnchorTick`) to a second shared word is what
	//     the gate's one-atomic-word fence forbids. §2
	//
	// ⛔ WRAPAROUND, RECORDED AS A KNOWN BOUND RATHER THAN DEFENDED AGAINST: `m_landingSeqNr` is
	// monotonic and reset only by `wipeCache`, so at 2^32 landings the `>` comparison inverts for
	// one window and MIS-CLASSIFIES. It can never MIS-PROTECT. §3
	//
	// 60 * 4 B = 240 B per character: the whole memory cost of the fresh/stale split.
	std::array<uint32, StateBufferSize> m_slotLandingSeqNr;
	// Per-slot STATE LINEAGE — contract at `Diagnostics::stateProvenance`, three fences
	// in `SlotStateProvenance.h`. ⛔ DIAGNOSTIC ONLY: no production reader, MACHINE-CHECKED by a
	// named LLT rather than asserted here. §4
	std::array<SlotStateProvenance, StateBufferSize> m_stateProvenance;
	std::bitset<StateBufferSize> m_containsCorrectTick;
	std::bitset<StateBufferSize> m_predictionWasCorrect;
	// ⛔ THE GRAVESTONE. `m_isResimulated` IS GONE, and with it the gate's derived-state
	// machinery: the scan `getLastResimulationTick`, the frontier inheritance in
	// `pushPredictionTick`, and a five-site bit discipline across `tryInsertingCorrectState` /
	// `tryInsertingResimulatedState` / `wipeCache` / `save_snapshot` that had to stay correct or
	// the gate broke SILENTLY — which is what happened, for months. §1, §2
	//
	// ⛔ It had exactly ONE production reader, the gate, verified by EXHAUSTING readers before
	// removal. `tryInsertingResimulatedState`'s observational `bool` return never had anything to do
	// with this bit. §1
	//
	// ⚠ IF YOU WANT PER-SLOT RESIM PROVENANCE FOR A DIAGNOSTIC: it is genuinely gone, and a bitset
	// re-creates the discipline above. `ResimGateProbe` already counts the event stream — prepares,
	// finishes, replayTicks, `replayOverruns` — which is what every such diagnostic wanted. §1
	//
	// ⚠⚠ ANNOTATION, NOT A REVERSAL. ⛔ THE BITSET ABOVE STAYS RETIRED AND EVERY WORD OF
	// THAT BLOCK STILL STANDS; `getLastResimulationTick` is not coming back. §4
	//
	// WHAT CHANGED is that the diagnostic the paragraph above turns away now exists deliberately as
	// `m_stateProvenance` — an ENUM, not a bitset, whose `Replayed` answers everything the old bit
	// could and whose other five values answer things it could not. ⛔ IT IS ADMITTED ONLY BECAUSE
	// THREE FENCES MAKE THE HAZARD UN-SHIPPABLE:
	//   1 it is not a bool and DOES NOT TAKE THE OLD NAME (every archived document binds
	//     `m_isResimulated` to trigger semantics);
	//   2 it has NO production reader, MACHINE-CHECKED by
	//     `…TheProvenanceColumnCannotReachAnyProductionOutput`, which garbage-fills the column
	//     mid-lifecycle and asserts every production output byte-identical — so "the gate derives
	//     from a per-slot column" is a RED TEST rather than a discouraged practice;
	//   3 its readers exist on day one, so the stored-value-nothing-reads rule is satisfied
	//     rather than re-broken. §4
	//
	// ⛔ A READER OF THIS BLOCK MUST NOT CONCLUDE THE OLD MECHANISM IS BACK. The column's stated non-goal
	// forbids wiring provenance into a trigger BY NAME; argue against fence 2, do not weaken it.
	// Full contract: `SlotStateProvenance.h`. §4
	// ⛔ `m_containsCorrectionInput` IS GONE with the channel that set it: its sole setter was
	// `insertCorrectionInput` and its sole reader `getLastCorrectionInput`, so with the setter
	// retired it could only ever have read false. §1
	std::function<void(const char*)> m_logger;
	IntegrateFn m_integrateFn;

	// ⛔ Log gate only — NEVER read by insertion logic. See `isAnomalousMiss`. §6
	uint32 m_anomalousMissDistanceTicks =
		static_cast<uint32>(TimeConfig{}.rollbackWindowHardCap);

	// THE GATE. 0 == no resim pending. ⛔ THE ONLY CROSS-THREAD STATE HERE WITH ANY
	// SYNCHRONIZATION — GT set versus PT consume/wipe. The four write sites and the live read
	// `getPendingResimAnchorTick` are rostered at the gate block. §2
	//
	// ⛔ DO NOT "SIMPLIFY" THIS BACK TO A PLAIN `uint32`, AND DO NOT GROW THE GATE TO A SECOND
	// SHARED WORD: a compare-then-clear can stomp a newer anchor — this defect, in miniature. §2
	//
	// ⚠ THIS DOES NOT CONTRADICT THE "NO ATOMICS" RULE THAT GOVERNS THE PROBES
	// (`ResimGateProbe`). This is CONTROL state with one correct value; splitting it per thread
	// would mean two gates that disagree. §5
	std::atomic<uint32> m_pendingResimAnchorTick{ 0u };

	// R2's capture — PHYSICS-THREAD-PRIVATE, hence plain. Written by
	// `captureResimAnchorForConsume`, read by `consumeCapturedResimAnchor`. §2
	uint32 m_consumeExpectedAnchorTick = 0u;

	// THE MONOTONIC LANDING COUNTER `m_landingSeqNr` (GAME thread) and its PREPARE-TIME
	// CAPTURE `m_preparedLandingSeqNr` (PHYSICS, `captureResimAnchorForConsume`). ⛔ Both plain,
	// both observational — the write rule reads neither. §3
	uint32 m_landingSeqNr         = 0u;
	uint32 m_preparedLandingSeqNr = 0u;

	// The pushed copy of `TimeConfig::resimTriggerPolicy` — see `setResimTriggerPolicy`.
	// ⛔ Sourced from the TimeConfig default rather than a literal, so R-P1 holds. §2
	TimeConfig::ResimTriggerPolicy m_resimTriggerPolicy = TimeConfig{}.resimTriggerPolicy;

	// THE FRONTIER-PAIR DETECTOR — PHYSICS-THREAD-PRIVATE. Catches "tick pushed, state
	// never pushed", one allocation late. §9
	//
	// ⛔ THE SITE SET IS THE CONTRACT. Set by `pushPredictionTick` on its ALLOCATION path, guarded
	// by `OG_CHECK`. Cleared by `pushPredictionState` WITH NO CHECK, by `save_snapshot` only when
	// `tick` IS the frontier, and reset by `wipeCache`. §9
	//
	// ⛔ DECLARED NON-PROPERTIES — load-bearing, read before touching this bit:
	//   * NOT GATE STATE. `needsResimulation()` must never read it; the gate's one-atomic-word
	//     fence governs `m_pendingResimAnchorTick`, and this is a different word entirely. §2
	//   * NEVER ENTERS `compute_checksum` — the anchor's prohibition verbatim: two peers must
	//     agree on STATE and may legitimately disagree on this bit's transient value. §7
	//   * PT-ONLY. `pushPredictionTick` / `pushPredictionState` / `wipeCache` / `advance_frame` are
	//     all physics- or harness-thread, so this adds NO GT/PT crossing and NO
	//     `docs/ThreadingCrossings.md` row. §5
	//   * A SEPARATE WORD FROM THE PROVENANCE COLUMN, DELIBERATELY: an `OG_CHECK` against
	//     `m_stateProvenance` would be a production READ of that column and would fire under
	//     `…TheProvenanceColumnCannotReachAnyProductionOutput`. This bit is dedicated precisely so
	//     that fence stays green. §4
	//
	// ⛔ BUILD REACH — a development-time detector and it cannot be otherwise: standalone `OG_CHECK`
	// is a bare `assert` (gone under `NDEBUG`), UE-side `checkf` (gone in Shipping). §9
	//
	// ⛔ COVERAGE — HONESTLY BOUNDED, NOT "CANNOT FAIL SILENTLY". It catches exactly ONE failure
	// mode, one allocation late, and has FOUR known limits SPLIT BY DIRECTION:
	//   UNDER-DETECTION, still live — a violation exists and is never reported:
	//   (i)   on the FINAL TICK before teardown: no later allocation exists to catch it;
	//   (ii)  the ENTIRE REVERSE DIRECTION: state pushed without a paired allocation is legal here
	//         and is covered ONLY by the shared `stepAllocatesFrontierSlot` predicate;
	//   (iii) a WIPE interposed between an abandoned allocation and the next SWALLOWS it.
	//   OVER-DETECTION, FIXED — kept so nobody "simplifies" the fix back out:
	//   (iv)  `save_snapshot`'s existing-slot path used to never touch this bit, so a
	//         `save_snapshot` completing a `pushPredictionTick`-opened pairing left it falsely
	//         `true` and armed a spurious `OG_CHECK` crash at the NEXT legitimate allocation — ON A
	//         CACHE WHOSE STATE WAS ENTIRELY CORRECT, the more damaging direction. Closed by the
	//         guarded clear in `save_snapshot`; pinned RED-then-GREEN by
	//         `FrontierPairContractTest.cpp`'s `SaveSnapshotOnAnExistingSlotClearsTheAwaitingBit`. §9
	//
	// Read seam for tests: `getDiagnostics().frontierSlotAwaitingState()`. §8
	bool m_frontierSlotAwaitingState = false;

public:
	// NON-COPYABLE AND NON-MOVABLE, stated rather than inherited. §2
	//
	// `std::atomic` deletes both implicitly; spelling them out keeps a future `cache = otherCache`
	// from being a puzzle. ⛔ THE SEMANTIC REASON THEY MUST STAY DELETED: a duplicated cache gives
	// two objects one anchor. `createCacheFor` uses `try_emplace` so the map never needs a move. §2
	StateCorrectionCache(const StateCorrectionCache&)            = delete;
	StateCorrectionCache& operator=(const StateCorrectionCache&) = delete;
	StateCorrectionCache(StateCorrectionCache&&)                 = delete;
	StateCorrectionCache& operator=(StateCorrectionCache&&)      = delete;
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

// ⛔ `receiveCorrectionInput` — the free-function mirror of `receiveCorrectionState` — IS
// GONE. It decoded a (tick, input) payload no sender writes. §1

template <typename SyncedBuffer, typename StateType, typename InputType>
void sendCorrectionState(const SimulationTimeStep& timingInfo, const StateType& state, SyncedBuffer& buffer, std::function<uint32(const StateType&, SyncedBuffer&, uint32)> writeBufferFunction)
{
	uint32 internalByteIterator = 0;

	buffer.template writeToBuffer<uint32>(internalByteIterator, timingInfo.getTick());
	internalByteIterator += sizeof(uint32);

	internalByteIterator += writeBufferFunction(state, buffer, internalByteIterator);
}

OGSIM_OPTIMIZE_ON
// pragma optimize on.
