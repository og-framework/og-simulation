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
//                   that tick, the LANDING STAMP for that tick, the DIAGNOSTIC
//                   STATE-PROVENANCE byte for that tick, plus the two
//                   bookkeeping bits }
//
// [item 48] THE STATE-PROVENANCE BYTE is the newest column and it is the ONLY
// one on this class that NOTHING IN PRODUCTION READS — deliberately, and
// machine-checked. It answers "where did the state in this slot come from?"
// (`SlotStateProvenance`: Empty / Predicted / AuthorityAdopted /
// AuthorityAgreedKeptPrediction / Replayed / ReplayedOverCorrection), 60 bytes
// per cache. ⚠ It is NOT the retired `m_isResimulated` bit coming back — read
// the three fences in SlotStateProvenance.h and the gravestone at the bottom of
// this class BEFORE assuming otherwise. See `getDiagnosticStateProvenance`.
//
// [item 47] THE LANDING STAMP is the one column added since: "the value of this
// cache's monotonic landing counter when a correction was last inserted here",
// 0 for never. It exists to split item 47's replay-write PROTECTIONS into fresh
// and stale populations for the probe — it decides nothing. The write rule reads
// `m_containsCorrectTick` alone. See `getSlotLandingSeq` and
// `resimGate::classifyResimSlotWrite`.
//
// [item 45] TWO BITS, NOT THREE: `m_isResimulated` retired with the
// level-triggered resim gate. The gate's state is no longer per-slot at all — it
// is ONE per-character atomic word, `m_pendingResimAnchorTick`. See the gate block
// above `needsResimulation`.
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
		m_slotLandingSeq.fill(0u);
		// [item 48] Nothing has written state anywhere yet, and `Empty` is the one
		// place that fact is recorded in data — the tick buffer cannot carry it,
		// because filling it with 0 makes every unwritten slot claim tick 0.
		m_stateProvenance.fill(SlotStateProvenance::Empty);
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
		m_slotLandingSeq.fill(0u);
		m_stateProvenance.fill(SlotStateProvenance::Empty);   // [item 48]
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

	// [og-netcode-v2-input-relay item 45] "Which is the newest tick an authoritative
	// correction has LANDED in?" — a pure query over `m_containsCorrectTick`.
	//
	// ITS FORMER PRODUCTION READER IS GONE: `getLastResimulationTick` used this as
	// the lower bound of its newest-first scan, and that scan is retired with the
	// level-triggered gate (see the anchor block below). It is kept rather than
	// retired with it, and the T16 rule is why that is not a contradiction: T16
	// retires STORED values nothing reads, because a second copy of the truth goes
	// stale silently. This stores nothing — it derives an answer from the live
	// bitset on every call and cannot be stale. It remains the honest way to ask
	// the question (the resim ANCHOR is now a decided trigger, not "the newest
	// correction", and the two must not be conflated), and it is what the wipe
	// cases assert against.
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

	// =======================================================================
	// [og-netcode-v2-input-relay item 45] THE RESIM GATE — ONE WORD, EDGE-TRIGGERED.
	// (design_task43_resim_gate_fix.md §1, §3 candidate D, §3.1; the defect it
	//  repairs is impl/finding_task31_resim_rate.md; the semantics are pinned by
	//  og-simulation-tests `[CorrectionCache][ResimGate]`.)
	//
	// ⛔ `getLastResimulationTick()` IS GONE, together with the `m_isResimulated`
	// bitset it scanned and the `pushPredictionTick` inheritance that fed it. What
	// it did: computed `getLastCorrectTick()`, then walked newest->oldest FROM THE
	// FRONTIER SLOT (offset 0) and returned the first slot flagged `m_isResimulated
	// || m_containsCorrectTick`. Since `postResimulationAll` flagged every replayed
	// slot INCLUDING the frontier and `pushPredictionTick` copied that bit into
	// every new frontier slot, after any completed resim the scan terminated at
	// offset 0 with `anchor == predictionTick` and the gate was pinned FALSE. A
	// correction landing BEHIND the frontier set its own slot's bit and was never
	// reached: ~8,759 behind-frontier corrections against 2 triggers per measured
	// run. The gate measured CLOCK ALIGNMENT, not divergence.
	//
	// ⚠ AND THE INHERITANCE WAS A LOAD-BEARING GUARD, NOT THE BUG. A level-triggered
	// gate needs something to hold it closed once the state that opened it has been
	// consumed. Seeding the new frontier `false`, or starting the scan at offset 1,
	// produced an UNTERMINATING 1-tick-resim storm (the just-replayed flagged slot
	// sits one below an unflagged frontier and re-triggers every tick, for the ~60
	// ticks until the ring recycles it). Those are design §3's candidates A and B
	// and they are DEAD — do not resurrect them. The inheritance line was deleted
	// HERE, in this change, ONLY BECAUSE THE READER IS GONE: an edge-triggered gate
	// needs no hold, because nothing recomputes it from derived state.
	//
	// WHAT REPLACES IT — `m_pendingResimAnchorTick`, 0 == none:
	//   W1  set   `tryInsertingCorrectState`'s hit path, GAME thread, when
	//             `resimGate::shouldSetPendingAnchor` says so. CAS-max, so several
	//             landings between two checks COALESCE to the newest.
	//   W2  clear the resim-completion edge, PHYSICS thread, as a LITERAL CAS
	//             against the anchor captured at prepare time (see
	//             `consumeResimAnchor`).
	//   W3  clear `wipeCache` (hard resync), zeroed with the tick numbering it
	//             belongs to.
	//   R1  read  `needsResimulation()` + this accessor, PHYSICS thread, per frame
	//             via `SimulationReconciliation::checkDivergenceAll`.
	//   R2  read  the prepare-time capture, PHYSICS thread.
	// TERMINATION IS STRUCTURAL: events set the anchor, completion consumes it, and
	// no rescan of slot state can re-trigger a consumed correction.
	//
	// ⚠ IT IS A TRIGGER, NOT A DATA-PUBLICATION CHANNEL. The corrected STATE this
	// anchor points at travels through `m_stateBuffer`, whose unsynchronized GT/PT
	// access is the cache's PRE-EXISTING formal race (finding §1 thread note),
	// unchanged by this change and owned by its own future item (design §7.4). The
	// anchor makes the GATE race-free; it does not make the cache race-free, and it
	// must never be read as if it did.
	//
	// ⛔ IT MUST NEVER ENTER THE DETERMINISM CHECKSUM. It is transient CONTROL
	// state, not simulation state: two peers replaying the same inputs from the same
	// state must agree on the STATE, and they legitimately differ on whether a resim
	// is pending. `compute_checksum` hashes `m_stateBuffer[tick]` only, and
	// `save_snapshot` / `load_snapshot` / `advance_frame` deliberately do not touch
	// the anchor — see the non-site list in design §3.1.
	// =======================================================================

	// R1 — the pending anchor, or 0 when no resim is pending. This is the value
	// `checkDivergenceAll` folds to a min across characters and hands to Chaos as
	// the tick to restore at.
	uint32 getPendingResimAnchorTick() const
	{
		return m_pendingResimAnchorTick.load(std::memory_order_acquire);
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

	// R1 — THE GATE. Unchanged in shape from the level-triggered version
	// (`anchor != 0 && anchor != predictionTick`) and deliberately so: the second
	// clause is what makes a correction landing ON the frontier wait for the
	// frontier to move before it triggers, which is the legacy timing the
	// `FrontierExact` policy has to reproduce. Only the SOURCE of `anchor` changed
	// — a stored trigger instead of a derived scan.
	//
	// `const` now, because it no longer computes anything: worth one word of note
	// because the old signature's non-constness was the only thing stopping
	// `findInputCache`'s const route from asking the gate directly.
	bool needsResimulation() const
	{
		const uint32 anchorTick = getPendingResimAnchorTick();
		return anchorTick != 0u && anchorTick != getPredictionTick();
	}

	// W2 — CONSUMPTION, AND IT IS A LITERAL COMPARE-AND-SWAP. Called on the resim
	// completion edge (the `[Resim.Finish]` block in
	// `SimulationManager::onPostGameSimulation`, beside `applyResimAll`) with the
	// anchor this resim was PREPARED with. Returns true when it consumed.
	//
	// ⭐ WHY THE CAS IS THE MECHANISM AND NOT A DEFENSIVE FLOURISH. A correction can
	// land on the GAME thread while the replay is running on the PHYSICS thread. A
	// compare-then-store would read `pending == prepared`, then be overtaken by W1
	// writing a NEWER anchor, then store 0 over it — a silently lost trigger, which
	// is the original defect in miniature. As a CAS the newer anchor makes the
	// exchange FAIL and SURVIVE, so the next frame re-triggers on it. The intended
	// behaviour becomes impossible to violate rather than improbable by timing, and
	// — because the expected value is a parameter — it is exercisable
	// SINGLE-THREADED in a unit test: prime the anchor, consume with a stale
	// expected value, assert survival.
	//
	// A `preparedAnchorTick` of 0 means this resim consumed nothing (no anchor was
	// pending when it was prepared, e.g. an engine-side rewind we did not ask for);
	// it can never match a live anchor, so it is rejected up front rather than
	// CAS-ing against the "none" sentinel.
	bool consumeResimAnchor(uint32 preparedAnchorTick)
	{
		if (preparedAnchorTick == 0u)
			return false;

		uint32 expected = preparedAnchorTick;
		return m_pendingResimAnchorTick.compare_exchange_strong(
			expected, 0u, std::memory_order_acq_rel, std::memory_order_acquire);
	}

	// R2 — the prepare-time capture, and its consume partner.
	//
	// ⚠ WHY THE CAPTURE IS PER CACHE AND NOT ONE VALUE AT THE MANAGER. A resim is
	// prepared from the MIN anchor across characters (`checkDivergenceAll` folds
	// with `std::min`), so character B's anchor is frequently NEWER than the tick
	// the resim restores at. CAS-ing every character against that single min would
	// fail for B, leave B's anchor pending, and re-trigger a resim next frame — a
	// treadmill in any session with more than one character, where the legacy gate
	// closed for everyone because `postResimulationAll` flagged every character's
	// replayed slots. Each cache therefore captures ITS OWN anchor and consumes
	// against that.
	//
	// ⚠ AND THIS ONE IS A PLAIN `uint32`, WHICH IS NOT AN OVERSIGHT NEXT TO THE
	// ATOMIC ABOVE. It is PHYSICS-THREAD-PRIVATE: written by `prepareResimAll` and
	// read by the completion edge, both on the physics thread, never touched by the
	// game thread. The one-atomic-word fence (design §3.2 invariant 4) governs the
	// GATE word, which is two-writer; this is a single-threaded scratch value for
	// the CAS's expected argument and gains nothing from an atomic.
	// [item 47] IT ALSO CAPTURES THE LANDING-SEQUENCE BASELINE, and that is why
	// this one method is still the whole of "prepare" for this cache. The two
	// captured values are the two arms of the freshness classifier
	// (`resimGate::classifyResimSlotWrite`): the anchor answers "which ticks was
	// this resim supposed to act on", the sequence answers "which slots were
	// written after it started". Capturing them anywhere but the same instant
	// would let a landing fall between them and be counted in neither population.
	//
	// The sequence capture is PHYSICS-THREAD-PRIVATE in the same way the anchor
	// capture is, and it READS a game-thread-written counter — see the threading
	// note on `getSlotLandingSeq`. It feeds a COUNTER, never a decision.
	void captureResimAnchorForConsume()
	{
		m_consumeExpectedAnchorTick = getPendingResimAnchorTick();
		m_preparedLandingSeq        = m_landingSeq;
	}

	uint32 getCapturedResimAnchorTick() const { return m_consumeExpectedAnchorTick; }

	// [item 47] The prepare-time landing-sequence capture. Diagnostics and tests
	// only — production reads it exactly once, inside the classifier call in
	// `tryInsertingResimulatedState`.
	uint32 getCapturedLandingSeq() const { return m_preparedLandingSeq; }

	// [item 47] THE PER-SLOT LANDING STAMP — "the value of this cache's monotonic
	// landing counter when a correction was last inserted into this slot", 0 for a
	// slot no correction has ever landed in.
	//
	// ⚠ THREADING, STATED HERE BECAUSE THIS IS THE ONE NEW CROSS-THREAD SURFACE
	// ITEM 47 ADDS. The stamps are written on the GAME thread (`W1`'s hit path)
	// and read on the PHYSICS thread (the classifier). They are PLAIN `uint32`s,
	// deliberately, and this is NOT a second gate word:
	//   * they RIDE THE PRE-EXISTING STATE-BUFFER RACE they exist to protect. The
	//     authority STATE this stamp describes travels through `m_stateBuffer`,
	//     whose unsynchronized GT/PT access is the cache's long-standing formal
	//     race (design §7.4, its own future item). A stamp torn or stale relative
	//     to the state beside it is the same race, not a new class of one.
	//   * they are OBSERVATIONAL. `resimGate::classifyResimSlotWrite` returns
	//     `Written` iff `!slotContainsCorrectTick` — REGARDLESS of the stamps —
	//     and the write site acts on `outcome != Written`, so the decision rests
	//     on exactly one input, itself a pre-existing GT-written / PT-read bit.
	//     The stamps only split the protections into fresh/stale for the probe. A
	//     torn stamp mis-labels a COUNTER; it can never mis-decide a write. (That
	//     one-bit property is swept as its own section in `ResimGatePolicyTest`,
	//     because it is what this whole paragraph rests on.)
	//   * ⛔ THEY ARE NOT GATE STATE. `needsResimulation()` does not read them and
	//     must never be made to. Item 45's one-atomic-word fence governs the GATE
	//     (`m_pendingResimAnchorTick`), which is two-writer control state; growing
	//     the GATE to a second shared word is what that fence forbids, and this is
	//     not that.
	//
	// WRAPAROUND: `m_landingSeq` is monotonic and never reset except by
	// `wipeCache`. At 2^32 landings — >2 years of continuous 60 Hz corrections on
	// one character — the `>` comparison would invert for one window and
	// mis-CLASSIFY (never mis-protect). Not defended against, recorded so it is a
	// known bound rather than a surprise.
	uint32 getSlotLandingSeq(uint32 cacheIndex) const
	{
		OG_CHECK(cacheIndex < StateBufferSize, "trying to access landing seq with bad cacheIndex");
		return m_slotLandingSeq[cacheIndex];
	}

	// The monotonic counter itself — the value the NEXT landing will exceed.
	uint32 getLandingSeq() const { return m_landingSeq; }

	// =======================================================================
	// [og-netcode-v2-input-relay item 48] THE PER-SLOT STATE PROVENANCE —
	// "WHERE DID THE STATE IN THIS SLOT COME FROM?", DIAGNOSTIC-ONLY.
	//
	// ⛔ THE FULL CONTRACT, THE THREE FENCES AND THE ⚠ AGAINST MISTAKING THIS
	// FOR THE RETIRED `m_isResimulated` BIT ARE IN `SlotStateProvenance.h`.
	// Read that block before changing any of the five write sites, and read the
	// gravestone at the bottom of this class before adding a sixth.
	//
	// The one-paragraph version, because a reader who lands HERE needs it:
	// this column is written at five sites and read by NOTHING IN PRODUCTION.
	// The accessor is named `getDiagnosticStateProvenance` rather than anything
	// shorter for exactly that reason — the name is load-bearing. Its readers
	// are the `[CorrectionCache][ResimGate][Provenance]` LLTs and the Verbose
	// `[ResimProbe.SlotMap]` line (`SimulationReconciliation::
	// dumpSlotProvenanceAll`), and that is the complete list.
	//
	// ⭐ THE INDEPENDENCE IS MACHINE-CHECKED, and the case is named so it can be
	// found and so it cannot be quietly dropped:
	//     CorrectionCache.ResimGate.TheProvenanceColumnCannotReachAnyProductionOutput
	// It scribbles arbitrary garbage into this column at three points of a live
	// gate lifecycle and asserts every production output — gate, anchor,
	// captures, verdicts, replay write outcomes, adopted state and the
	// determinism CHECKSUM — is byte-identical to the un-scribbled run. THAT
	// CASE IS THE FENCE. The original resim-gate defect was production logic
	// derived from per-slot bits; the case makes that regression fail a test
	// rather than merely contradict a comment.
	//
	// ⚠ THREADING: GT-written (corrections) and PT-written (prediction, replay,
	// wipe), plain bytes, riding the pre-existing `m_stateBuffer` GT/PT race
	// exactly as item 47's landing stamps do. A diagnostic read TOLERATES a torn
	// or ±1-stale value: one slot of one map line may be wrong, and because
	// nothing decides on it there is no correctness consequence to be had.
	//
	// ⛔ IT NEVER ENTERS `compute_checksum` OR THE DETERMINISM COMPARISON — the
	// anchor's prohibition, verbatim, for the same reason: two peers replaying
	// identical inputs from identical state must agree on the STATE and may
	// legitimately disagree on how each of them got there.
	// =======================================================================
	SlotStateProvenance getDiagnosticStateProvenance(uint32 cacheIndex) const
	{
		OG_CHECK(cacheIndex < StateBufferSize, "trying to access state provenance with bad cacheIndex");
		return m_stateProvenance[cacheIndex];
	}

	// ⛔ THE FENCE'S OWN INSTRUMENT. IT HAS NO PRODUCTION CALLER AND MUST NEVER
	// ACQUIRE ONE — the name says so at every call site on purpose.
	//
	// Fills the whole provenance column with a deterministic garbage cycle
	// derived from `seed`, i.e. makes every slot's recorded lineage a LIE while
	// leaving every other column untouched. Two cases use it:
	//
	//   1. `…TheProvenanceColumnCannotReachAnyProductionOutput` — fence 2. If
	//      any production output moved, some production path is reading this
	//      column and the fence is broken.
	//   2. `…ReplayedOverCorrectionIsUnreachableButTheAlarmIsWired` — forges the
	//      guard-failure precondition (authority-grade provenance over a CLEAR
	//      `m_containsCorrectTick`) that protect-all makes unreachable, so the
	//      alarm value can be proven live rather than merely asserted absent.
	//
	// The cycle walks all `kSlotStateProvenanceCount` enumerators and offsets by
	// slot index, so no two adjacent slots agree and every value appears. It
	// stays INSIDE the enumeration deliberately: a genuinely out-of-range byte
	// would be undefined behaviour on the switch in `slotStateProvenanceChar`,
	// and "every slot lies" is already the whole hypothesis under test.
	void scribbleDiagnosticStateProvenanceForFenceTest(uint32 seed)
	{
		for (size_t i = 0; i < StateBufferSize; ++i)
		{
			m_stateProvenance[i] = static_cast<SlotStateProvenance>(
				static_cast<std::uint8_t>((seed + static_cast<uint32>(i)) % kSlotStateProvenanceCount));
		}
	}

	bool consumeCapturedResimAnchor() { return consumeResimAnchor(m_consumeExpectedAnchorTick); }

	// The trigger-policy seam. The SOURCE OF TRUTH is `TimeConfig::
	// resimTriggerPolicy`; this is the pushed copy the game-thread write site
	// consults, published one way through `SimulationManager::
	// setResimTriggerPolicy` -> `SimulationReconciliation::setResimTriggerPolicy`
	// -> here, which is also what stamps caches created later (a character
	// registering mid-session). The default below is read from the TimeConfig
	// default rather than named as a literal, so R-P1 holds and a retune of the
	// shipped policy tracks automatically.
	//
	// GAME-THREAD-ONLY in effect: it is written at composition (before any
	// correction can land) and read at W1. It is not atomic for that reason, and
	// making it a runtime-tunable cvar would break that argument — see the
	// one-shot-at-composition ruling on the sibling knobs in SimulationManager.h.
	void setResimTriggerPolicy(TimeConfig::ResimTriggerPolicy policy)
	{
		m_resimTriggerPolicy = policy;
	}

	TimeConfig::ResimTriggerPolicy getResimTriggerPolicy() const { return m_resimTriggerPolicy; }

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
		// ⛔ [item 45] THE INHERITANCE LINE IS GONE FROM HERE:
		//     m_isResimulated[newPredictionIndex] = m_isResimulated[predictionIndex];
		// IT IS DELETED BECAUSE THE READER IS GONE, NOT BECAUSE THE GUARD WAS WRONG.
		// It held the level-triggered gate closed after a completed resim, and
		// deleting it on its own reintroduced an unterminating 1-tick-resim storm
		// (design §3 candidate A). With the gate edge-triggered there is nothing left
		// to hold: `getLastResimulationTick`'s scan — `m_isResimulated`'s only reader
		// — is retired, and frontier advance no longer touches gate state AT ALL.
		// That last clause is the property to protect here: a future edit that makes
		// `pushPredictionTick` write the anchor would put derived-state re-triggering
		// back and the storm with it. See the gate block above `needsResimulation`.
		// [T4] The recycled slot now describes a DIFFERENT tick, so the previous
		// occupant's join key must retire with it — otherwise a resim through the
		// fresh tick would resolve remote input from a capture ~StateBufferSize
		// ticks old and be confidently wrong rather than merely uninformed.
		m_appliedCaptureTickBuffer[newPredictionIndex] = kNoInputCaptureTick;
		// [item 47] AND SO MUST THE LANDING STAMP, for a sharper version of the
		// same reason: it is retired alongside `m_containsCorrectTick`, which is
		// what the protection rule reads. Leaving a stale HIGH stamp on a
		// recycled slot would make the NEXT resim classify a fresh landing there
		// as... still fresh (the stamp only rises), so it cannot mis-protect —
		// but it would report a protection for a landing that never happened.
		// The stamp's "0 == no correction has ever landed here" contract is worth
		// more than the two instructions it costs to keep.
		m_slotLandingSeq[newPredictionIndex] = 0u;
		// [item 48] WRITE SITE 1 of 5 — RING RECYCLE. The slot now describes a
		// different tick and holds no state for it yet, so its lineage retires
		// with the rest of its bookkeeping.
		//
		// ⚠ `Predicted`, NOT `Empty`, and the reason is the ordering of the two
		// calls production always makes: `pushPredictionTick` is immediately
		// followed by `pushPredictionState` (postPredictionAll, backfillSkippedTick,
		// advance_frame — every one of them). A slot allocated by this method
		// is a prediction slot; `Empty` would be true for the width of one
		// statement and would then be a stale lie for the whole ring pass. The
		// place `Empty` genuinely belongs is a slot that has never been written
		// at all — the constructors and `wipeCache`.
		//
		// ⛔ AND UNLIKE THE LINE THIS SITS BESIDE, IT INHERITS NOTHING. The old
		// bit's `m_isResimulated[new] = m_isResimulated[prev]` inheritance is
		// the deleted line six comments up, and it was DELETED BECAUSE ITS
		// READER WAS GONE. Writing a CONSTANT here — never a value read out of
		// the previous slot — is what keeps that true: frontier advance still
		// touches no gate state, and nothing here is derived from anything.
		m_stateProvenance[newPredictionIndex] = SlotStateProvenance::Predicted;
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

	// [og-netcode-v2-input-relay item 42] RETURNS TRUE WHEN THE SLOT WAS FOUND —
	// purely observational, and the same defaulted-out-parameter trick
	// `tryInsertingCorrectState` uses one method down, in its cheapest form: a
	// return value nothing was reading. Every existing call site ignores it and is
	// byte-identical, which is what makes this behaviour-neutral by construction
	// rather than by inspection.
	//
	// WHY IT HAS TO LEAVE THE FUNCTION AT ALL. The discard branch already logs at
	// Warning, so the EVENT is visible; what does not exist is a DENOMINATOR — item
	// 42's I6 `replayOverruns` needs "how many discards per window of replay ticks",
	// and a log line cannot be counted by the code that emits it. The cache stays
	// id-agnostic and probe-agnostic: it reports, `postResimulationAll` tallies.
	//
	// ---------------------------------------------------------------------
	// [og-netcode-v2-input-relay item 47] ⛔ IT NO LONGER WRITES UNCONDITIONALLY —
	// A REPLAY NEVER OVERWRITES A CORRECTED SLOT.
	//
	// The rule, its rationale, the provenance invariant it makes hold by
	// construction, and the fresh/stale classifier are all in ONE block:
	// `resimGate::classifyResimSlotWrite` in ResimGatePolicy.h. Read that before
	// changing anything here. In one line: authority state (or a
	// within-tolerance prediction the authority certified) is strictly better
	// than a re-derivation of it, so the replay's own output is dropped for that
	// slot and `m_containsCorrectTick` is left ALONE — never cleared, because
	// nothing authority-marked is ever overwritten.
	//
	// ⚠ THE RETURN VALUE STILL MEANS "THE SLOT EXISTED", NOT "I WROTE". A
	// PROTECTED slot returns TRUE: item 42's `replayOverruns` counts ticks that
	// fell out of the 60-slot window, and re-pointing it at protections would
	// silently redefine an archived baseline (1-2 per RUN) into a different
	// population. The protections get their OWN counters, through `outOutcome`.
	//
	// `outOutcome` is the same defaulted, purely observational out-pointer
	// `tryInsertingCorrectState` uses one method down — every pre-item-47 call
	// site is byte-identical by construction, not by inspection.
	// ---------------------------------------------------------------------
	bool tryInsertingResimulatedState(StateType&& state, uint32 tick,
	                                  resimGate::ResimSlotWriteOutcome* outOutcome = nullptr)
	{
		if (outOutcome != nullptr)
			*outOutcome = resimGate::ResimSlotWriteOutcome::Discarded;

		auto it = std::find(m_tickBuffer.begin(), m_tickBuffer.end(), tick);
		if (it != m_tickBuffer.end())
		{
			uint32 cacheIndex = std::distance(m_tickBuffer.begin(), it);

			// [item 47] ONE CALL DECIDES BOTH THE ACTION AND ITS LABEL. The
			// action is `outcome != Written`; the fresh/stale split only chooses
			// which counter the protection lands in. A separate write predicate
			// beside this call was tried and REJECTED — a mutation run produced a
			// build that overwrote the slot while still reporting
			// `ProtectedFresh`, i.e. a counter that disagreed with reality. See
			// the ACTION note on `resimGate::ResimSlotWriteOutcome`.
			const resimGate::ResimSlotWriteOutcome outcome = resimGate::classifyResimSlotWrite(
				m_containsCorrectTick[cacheIndex],
				m_slotLandingSeq[cacheIndex],
				m_preparedLandingSeq,
				tick,
				m_consumeExpectedAnchorTick);

			if (outOutcome != nullptr)
				*outOutcome = outcome;

			if (outcome != resimGate::ResimSlotWriteOutcome::Written)
			{
				// PROTECTED. The replay's state for this tick is discarded; the
				// authority state and its provenance bit both stand. No log line:
				// this is per-replay-tick and would be exactly the T19 volume
				// defect. The event is counted on `[ResimProbe.Apply]` instead.
				//
				// [item 48] AND THE PROVENANCE COLUMN IS LEFT ALONE TOO, which is
				// the whole reason a completed resim's map shows an unbroken
				// `A`/`C` at every protected slot rather than an `R`: the state
				// did not change, so its lineage did not either.
				return true;
			}

			// [item 48] WRITE SITE 3 of 5 — THE REPLAY WRITE, and the ONE site
			// that can stamp the alarm value.
			//
			// ⭐ THE CHECK IS DELIBERATELY REDUNDANT WITH THE ONE ABOVE, AND THE
			// REDUNDANCY IS THE INSTRUMENT. `classifyResimSlotWrite` decided this
			// write on `m_containsCorrectTick`; this asks a SECOND, INDEPENDENT
			// source — the provenance column — whether that bit was telling the
			// truth. Under item 47's protect-all the two can never disagree
			// (`ProtectedFresh`/`ProtectedStale` short-circuits above at exactly
			// the population `isAuthorityGradeProvenance` marks), so this branch
			// is UNREACHABLE and `ReplayedOverCorrection` reads zero forever.
			//
			// ⛔ THAT IS NOT A REASON TO DELETE IT. The pre-item-47 state — bit
			// set, state replayed — was real, shipped and invisible for months
			// for exactly one reason: nothing could represent it. A regression
			// that re-opens the clobber now stamps an `X` in the slot map instead
			// of hiding. Collapsing the two sources into one would delete the
			// alarm and leave a comment claiming it exists.
			//
			// It reads no state it did not already have and changes NO decision:
			// the write below happens either way. See `SlotStateProvenance`.
			m_stateProvenance[cacheIndex] =
				isAuthorityGradeProvenance(m_stateProvenance[cacheIndex])
					? SlotStateProvenance::ReplayedOverCorrection
					: SlotStateProvenance::Replayed;

			m_stateBuffer[cacheIndex] = std::move(state);
			// ⛔ [item 45] `m_isResimulated.set(cacheIndex)` IS GONE FROM HERE, and
			// this is the site that makes the storm structurally impossible rather
			// than merely unlikely: REPLAY WRITES STATE AND PROVENANCE, NEVER GATE
			// STATE. Under the level-triggered gate this line was what re-closed the
			// gate after a resim (and, one tick later via the frontier inheritance,
			// what shadowed every behind-frontier correction). The gate is now closed
			// by an explicit CAS on the completion edge — see
			// `StateCorrectionCache::consumeResimAnchor` — so a replay tick has no
			// business touching it.
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
			// Tick not in cache window — correction arrived too late or too early; discard.
			if (m_logger)
			{
				char buf[128];
				std::snprintf(buf, sizeof(buf), "[Warning] tryInsertingResimulatedState: tick=%u not in cache window, discarding", tick);
				m_logger(buf);
			}
			return false;
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

			// [item 47] THE LANDING STAMP. One monotonic counter per cache,
			// bumped here and only here, so "this slot was corrected after that
			// resim was prepared" is answerable by comparing two integers rather
			// than by reasoning about wall time. It is stamped on BOTH verdicts
			// — an agreeing landing is authority information too (it certifies
			// the prediction), and the protection rule makes no verdict
			// distinction. See `resimGate::classifyResimSlotWrite`.
			m_slotLandingSeq[cacheIndex] = ++m_landingSeq;

			// [item 48] WRITE SITE 2 of 5 — AND THE ONE THAT CARRIES THE COLUMN'S
			// FIRST PAYLOAD VALUE.
			//
			// ⭐ THE BRANCH IS THE VERDICT BRANCH TAKEN TWENTY LINES DOWN, and
			// this is the point of an ENUM rather than a bool. `m_containsCorrectTick`
			// is set for BOTH verdicts, so the bit alone says "authority-grade" and
			// stops; it cannot say WHICH KIND. The state copy below runs only
			// `if (!predictionWasCorrect)` — so on an AGREEING landing the slot is
			// authority-grade while physically holding the PREDICTED value. Item 47
			// treats the two identically ON PURPOSE (a rule that protected only
			// adopted state would need a second bit to tell them apart); its rule
			// does not need the distinction, and a human reading a slot map does.
			//
			// ⚠ MIRROR THE `if (!predictionWasCorrect)` BELOW IF IT EVER MOVES.
			// The two are one decision expressed twice, and the case
			// `…ADisagreeingLandingAdoptsAuthorityAndAnAgreeingOneCertifiesThePrediction`
			// asserts state and provenance TOGETHER on both arms precisely so they
			// cannot drift apart silently.
			m_stateProvenance[cacheIndex] = predictionWasCorrect
				? SlotStateProvenance::AuthorityAgreedKeptPrediction
				: SlotStateProvenance::AuthorityAdopted;

			// ---------------------------------------------------------------
			// [item 45] W1 — THE ONE EVENT THAT OPENS THE RESIM GATE.
			//
			// This replaces `m_isResimulated[cacheIndex] = false;` — the old
			// un-shadow, which re-opened the gate only when the slot it landed in
			// HAPPENED to be the frontier. The condition is now explicit and
			// configured instead of emergent (`resimGate::shouldSetPendingAnchor`),
			// and it is evaluated HERE because this is the single chokepoint every
			// landed correction already passes through: a future correction source
			// or cadence feeds the gate with no new wiring.
			//
			// `landedAtFrontier` is the same comparison `classifyCorrectionLanding`
			// makes for item 42's landing probe, so the probe's `atFrontier` bucket
			// and the legacy policy's trigger condition are the same predicate on
			// the same value — which is what makes the probe's archived
			// `atFrontier` counts the baseline this policy has to reproduce.
			// `getPredictionTick()` is read here rather than after the state move
			// only for locality; the insert never touches `m_tickBuffer`.
			// ---------------------------------------------------------------
			const bool landedAtFrontier = (tick == getPredictionTick());
			if (resimGate::shouldSetPendingAnchor(
					m_resimTriggerPolicy, landedAtFrontier, predictionWasCorrect))
			{
				raisePendingResimAnchorTo(tick);
			}

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
		// [item 45] W3 — THE ANCHOR DIES WITH THE TICK NUMBERING. A resync renumbers
		// the prediction clock, so a surviving anchor would name a tick that no
		// longer exists — at best a resim to nowhere, at worst (if the new numbering
		// is lower) an anchor permanently ABOVE the frontier that the gate can never
		// close. Same retirement discipline as `m_appliedCaptureTickBuffer` two lines
		// down, and it also re-arms the CAS-max coalescing from a clean 0.
		//
		// ⚠ THREAD: this runs on whichever thread drives the resync callback, which
		// is the PHYSICS thread — `ClientPredictionClock::advancePrediction` is
		// called from `SimulationManager::onGameSimulationPrediction`, i.e. from
		// `FSimulationManagerAsyncCallback::OnPreSimulate_Internal`, and the clock
		// invokes the registered resync callback inline from there
		// (`SimulationManager`'s ctor -> `m_reconciliation.wipeAllForResync`). So W3
		// shares its thread with W2 and races only against W1 on the game thread —
		// which the atomic store handles, and which is why the anchor is atomic
		// rather than this site needing a lock.
		m_pendingResimAnchorTick.store(0u, std::memory_order_release);
		m_consumeExpectedAnchorTick = 0u;
		// [item 47] The landing stamps die with the tick numbering too, and for
		// the strongest form of the reason: after a resync a surviving stamp
		// would describe a slot whose TICK has been renumbered, so the classifier
		// would compare an old landing's sequence against a new resim's capture
		// and call an ancient slot fresh. The counter is re-armed from 0 with the
		// slots, which keeps "stamp 0 == never landed" true after a wipe as well.
		m_slotLandingSeq.fill(0u);
		m_landingSeq         = 0u;
		m_preparedLandingSeq = 0u;
		// [item 48] WRITE SITE 4 of 5 — THE WIPE, and the one place `Empty` is
		// written after construction. A resync renumbers every tick, so no slot
		// describes state for the tick it now claims: "predicted" and "corrected"
		// are both false of every slot here, and the ONLY honest lineage is "no
		// state has been written for this tick". Note the frontier slot is
		// renumbered two lines down and stays `Empty` too — `wipeCache` sets its
		// TICK, never its state; the next `pushPredictionState` is what fills it.
		m_stateProvenance.fill(SlotStateProvenance::Empty);
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
	// pushPredictionTick), so stale correction metadata cannot leak.
	//
	// [item 45] AND IT DELIBERATELY DOES NOT TOUCH THE RESIM ANCHOR — one of the
	// explicit non-sites in design §3.1. The 4-method API is the externally-driven
	// determinism surface: it saves and replays SIMULATION state, and whether a
	// resim is pending is transient CONTROL state that two peers may legitimately
	// disagree on while remaining perfectly deterministic. Writing the anchor here
	// would make a harness run's gate state depend on snapshot order, and reading it
	// into a checksum would make identical simulations hash differently.
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
			// [item 45] `m_isResimulated[slot] = false;` is gone with the bitset.
			// [T4] Freshly allocated slot — no correction has named a join key for
			// this tick yet (mirrors pushPredictionTick).
			m_appliedCaptureTickBuffer[slot] = kNoInputCaptureTick;
			// [item 47] Same reset as pushPredictionTick's, for the same reason:
			// a freshly allocated slot has had no landing. NOTE the 4-method API
			// still touches NO gate state and NO landing COUNTER — the stamp is
			// per-slot bookkeeping that mirrors the bits beside it, whereas
			// `m_landingSeq` / the anchor are session control state a determinism
			// harness must not be able to perturb (design §3.1's non-site list).
			m_slotLandingSeq[slot] = 0u;
			// [item 48] WRITE SITE 5 of 5 — THE FRESHLY ALLOCATED HARNESS SLOT.
			//
			// ⭐ WHY THE 4-METHOD API WRITES PROVENANCE WHILE IT DELIBERATELY DOES
			// NOT WRITE THE ANCHOR — the two look like the same kind of exception
			// and are not. THE ANCHOR IS A LIVE TRIGGER: writing it here would let
			// a determinism harness's snapshot ORDER decide whether a resim fires,
			// which is why design §3.1 lists this method as an explicit non-site.
			// PROVENANCE DESCRIBES STATE LINEAGE, and the harness legitimately
			// CREATES state — `save_snapshot` is the externally-driven twin of
			// `pushPredictionTick` + `pushPredictionState`, so the honest answer to
			// "where did this state come from" is the same one that path gives.
			// Leaving it `Empty` would claim no state had been written into a slot
			// this line is about to write state into.
			//
			// Neither write can perturb a simulated value: the anchor is excluded
			// because it WOULD, and provenance is admitted because it CANNOT (fence
			// 2, and the checksum prohibition on both).
			m_stateProvenance[slot] = SlotStateProvenance::Predicted;
		}

		// ⚠ [item 48] AN **EXISTING** SLOT KEEPS ITS OLD PROVENANCE WHILE ITS
		// STATE IS REPLACED, so a harness snapshotting over a slot a correction
		// already landed in leaves an `A`/`C` describing state the harness wrote.
		// That is a KNOWN, PRE-EXISTING RESIDUAL rather than a new one, and it is
		// recorded rather than repaired for three reasons: `m_containsCorrectTick`
		// and the applied-capture ref have behaved exactly this way here since T4,
		// so provenance is CONSISTENT with its neighbours instead of uniquely
		// wrong; the path is reachable only from the 4-method determinism-harness
		// API, never from the live client; and item 47's review §6 already lists
		// it (with the GT/PT `m_stateBuffer` frontier race) as the residual pair
		// belonging to design §7.4. ⛔ It does NOT produce `ReplayedOverCorrection`
		// — that value is stamped by the replay path alone.
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
	// [item 45] W1's CAS-MAX — "the anchor is the NEWEST tick anybody asked to
	// resimulate from".
	//
	// NEWEST-CORRECTED COALESCING IS DELIBERATE, not a shortcut around a queue
	// (design §3, candidate C analysis): corrections arrive in tick order, and the
	// authority state at the newest corrected tick SUBSUMES every older correction
	// on the same trajectory. Restoring at the newest and replaying forward consumes
	// the whole backlog at depth = `frontier - anchor` ~= correction transit latency,
	// so several landings between two divergence checks cost ONE resim rather than
	// one each. "Older corrections are dead weight" dissolves the moment resims fire
	// against recent ones: the older ones are SUBSUMED, which is not the same as
	// ignored.
	//
	// A CAS loop rather than `fetch_max` (C++26) or a plain compare-then-store: W1
	// is on the game thread while W2/W3 are on the physics thread, so a
	// read-then-write could resurrect an anchor W2 has just consumed. Losing the CAS
	// means someone else moved the word; re-reading and re-testing is what makes the
	// max honest. `tick == 0` cannot become an anchor — 0 is the "none" sentinel AND
	// the reserved pre-sim tick, so the loop's guard rejects it for both reasons at
	// once.
	void raisePendingResimAnchorTo(uint32 tick)
	{
		uint32 current = m_pendingResimAnchorTick.load(std::memory_order_acquire);
		while (tick > current)
		{
			if (m_pendingResimAnchorTick.compare_exchange_weak(
					current, tick, std::memory_order_acq_rel, std::memory_order_acquire))
				return;
			// `current` has been reloaded with the value that beat us — re-test.
		}
	}

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
	// [item 47] Per-slot landing stamp — see getSlotLandingSeq for the contract,
	// the threading argument and the wraparound bound. 60 * 4 B = 240 B per
	// character, which is the whole memory cost of the fresh/stale split.
	std::array<uint32, StateBufferSize> m_slotLandingSeq;
	// [item 48] Per-slot STATE LINEAGE — see `getDiagnosticStateProvenance` for
	// the contract and `SlotStateProvenance.h` for the three fences. 60 * 1 B =
	// 60 B per character. ⛔ DIAGNOSTIC ONLY: no production reader, and the
	// independence is machine-checked by a named LLT rather than asserted here.
	std::array<SlotStateProvenance, StateBufferSize> m_stateProvenance;
	std::bitset<StateBufferSize> m_containsCorrectTick;
	std::bitset<StateBufferSize> m_predictionWasCorrect;
	// [og-netcode-v2-input-relay item 45] `m_isResimulated` — the per-slot "this
	// slot's state came from a resim replay" bit — IS GONE, and with it the gate's
	// entire derived-state machinery: the newest-first scan
	// (`getLastResimulationTick`), the frontier inheritance in `pushPredictionTick`,
	// and the five-site bit discipline that had to stay correct across
	// `pushPredictionTick` / `tryInsertingCorrectState` /
	// `tryInsertingResimulatedState` / `wipeCache` / `save_snapshot` or the gate
	// broke SILENTLY (which is exactly what happened, for months).
	//
	// It had exactly ONE production reader — the gate — verified by exhausting
	// readers before removal (design §1 blast radius): no serialization, no input
	// resolution, no state adoption, no probe (the probes count events, not bits).
	// So retiring the reader retired the value, and the T16 rule says to remove it
	// rather than leave a stored second copy of the truth for a future reader to
	// find and trust. `tryInsertingResimulatedState` keeps its item-42 `bool` return
	// — that is a REPORT of whether the slot existed, which the I6 probe counts, and
	// it never had anything to do with this bit.
	//
	// ⚠ IF YOU ARE HERE BECAUSE YOU WANT PER-SLOT RESIM PROVENANCE FOR A
	// DIAGNOSTIC: it is genuinely gone, and re-adding it as a bitset would re-create
	// the discipline above. The event stream is already counted at Warning
	// (`ResimGateProbe`: prepares / finishes / replayTicks / replayOverruns), which
	// is what every diagnostic asking "was this tick replayed" has actually wanted.
	//
	// ---------------------------------------------------------------------
	// ⚠⚠ [og-netcode-v2-input-relay item 48, 2026-08-12] ANNOTATION, NOT A
	// REVERSAL. **THE BITSET ABOVE STAYS RETIRED AND EVERY WORD OF THIS BLOCK
	// STILL STANDS.** `m_isResimulated` is gone, the gate does not read per-slot
	// state, `getLastResimulationTick` is not coming back, and the five-site
	// discipline this block warns about is exactly the hazard that cost items
	// 31/42/43/44/45.
	//
	// WHAT CHANGED is that the DIAGNOSTIC the paragraph directly above turns
	// away now exists deliberately, as `m_stateProvenance` — an ENUM column,
	// not a bitset, whose value `Replayed` answers everything the old bit
	// could and whose other five values answer things it could not. It was
	// added ON TOP OF this warning rather than in ignorance of it, and it is
	// admitted only because three fences make the hazard un-shippable:
	//   1. it is not a bool and DOES NOT TAKE THE OLD NAME (every archived
	//      document binds `m_isResimulated` to trigger semantics);
	//   2. it has NO production reader, and that is MACHINE-CHECKED by
	//      `…TheProvenanceColumnCannotReachAnyProductionOutput`, which
	//      garbage-fills the column mid-lifecycle and asserts every production
	//      output is byte-identical — so "the gate derives from a per-slot
	//      column" is now a RED TEST rather than a discouraged practice;
	//   3. its readers exist on day one (the scenario LLTs and one Verbose
	//      `[ResimProbe.SlotMap]` line), so T16's stored-value-nothing-reads
	//      rule is satisfied rather than re-broken.
	//
	// ⛔ A READER OF THIS BLOCK MUST NOT CONCLUDE THE OLD MECHANISM IS BACK. If
	// you are about to wire provenance into a trigger, item 48's own non-goal
	// forbids it by name — not even to reproduce the legacy virgin-cache free
	// trigger — and you must argue against fence 2's independence case in your
	// design rather than weaken it here. Full contract: SlotStateProvenance.h.
	// ---------------------------------------------------------------------
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

	// [item 45] THE GATE. 0 == no resim pending. The ONE piece of cross-thread state
	// this class has any synchronization on; see the block above
	// `getPendingResimAnchorTick` for the four write sites and why the atomic is
	// load-bearing (two writers on two threads: GT set vs PT consume/wipe).
	//
	// ⛔ DO NOT "SIMPLIFY" THIS BACK TO A PLAIN `uint32`, and do not grow the gate to
	// a second shared word. The plain word carries a real lost-update window, not
	// merely formal UB — a consume's compare-then-clear can stomp a newer anchor,
	// which is a silently lost resim trigger, i.e. the very defect this change
	// exists to fix, reintroduced in miniature. Two shared words would need an
	// ordering argument between them that one word does not need.
	//
	// ⚠ THIS DOES NOT CONTRADICT ITEM 42's "NO ATOMICS" RULE. That rule governs the
	// PROBES — telemetry, where the answer was two objects, one per thread, because
	// a shared window costs a whole window's totals on a race. This is CONTROL state
	// with two legitimate writers and exactly one correct value; splitting it per
	// thread would mean two gates that disagree.
	std::atomic<uint32> m_pendingResimAnchorTick{ 0u };

	// [item 45] R2's capture — PHYSICS-THREAD-PRIVATE, hence plain. Written by
	// `captureResimAnchorForConsume` at prepare, read by `consumeCapturedResimAnchor`
	// on the completion edge. See the note at those methods for why the expected
	// value must be per cache rather than the manager's single min anchor.
	uint32 m_consumeExpectedAnchorTick = 0u;

	// [item 47] THE MONOTONIC LANDING COUNTER (GAME thread; W1 is its only
	// writer) and the PREPARE-TIME CAPTURE of it (PHYSICS thread; written by
	// `captureResimAnchorForConsume`, read by the classifier). Both plain, both
	// observational — the write rule reads neither. Full argument, including why
	// this is not a second gate word, at `getSlotLandingSeq`.
	uint32 m_landingSeq         = 0u;
	uint32 m_preparedLandingSeq = 0u;

	// [item 45] The pushed copy of `TimeConfig::resimTriggerPolicy` — see
	// `setResimTriggerPolicy`. Sourced from the TimeConfig default rather than a
	// literal so R-P1 holds.
	TimeConfig::ResimTriggerPolicy m_resimTriggerPolicy = TimeConfig{}.resimTriggerPolicy;

public:
	// [item 45] NON-COPYABLE AND NON-MOVABLE, stated rather than inherited.
	//
	// `std::atomic` is neither copyable nor movable, so both would be implicitly
	// deleted anyway; they are spelled out because the resulting compile error at a
	// future `cache = otherCache` is otherwise a puzzle about a member three
	// hundred lines up. THE SEMANTIC REASON THEY MUST STAY DELETED: a cache
	// duplicated while a resim is pending would give two objects one anchor, so the
	// CAS on either would consume a trigger the other still believes in. Caches are
	// created in place — `SimulationReconciliation::createCacheFor` uses
	// `try_emplace` so the map never needs a move — and live for the character's
	// registration; nothing legitimately copies one.
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



