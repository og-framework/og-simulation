#pragma once
// SPDX-License-Identifier: MPL-2.0

// ---------------------------------------------------------------------------
// CorrectionStateBufferCodec — the CORRECTION STATE payload (server -> clients).
//
// Engine-agnostic byte-codec for the authoritative state correction
// (og-netcode-v2-input-relay T4; InputRelayDesign.md §3 "STATE", D1/D3).
// [T39] Formerly "the EVERY-FRAME authoritative state correction" — it is not
// every-frame any more; see the cadence note at WHY THE SECOND FIELD EXISTS.
//
// Payload layout:
//
//   [tick               (u32)]  offset 0  — the tick the state belongs to
//   [appliedCaptureTick (u32)]  offset 4  — THE JOIN KEY (see below)
//   [state composite    (...)]  offset 8  — per-field serialized SimulationComposite
//
// ---------------------------------------------------------------------------
// WHY THE SECOND FIELD EXISTS — the join key.
//
// Input and state are now two independently-cadenced channels: input is relayed
// at RECEIPT keyed by capture tick (T1/T3), state is corrected keyed by the
// AUTHORITY tick. Nothing in either message says which input produced which
// state — so a client resimulating through a corrected tick cannot know which
// relayed input the authority actually fed into it.
//
// ⚠ [T39] "CORRECTED EVERY FRAME" WAS TRUE HERE UNTIL T39 AND IS NOT ANY MORE.
// `SimulationNetSync::sendCorrectionAll` now writes `TimeConfig::
// correctionRotationK` characters' buffers per tick, round-robin, so a given
// character's state rides this payload at `tickFrequency * K / N` Hz — 60 Hz at
// two characters and 20 Hz at six, with the shipped K = 2. The wire LAYOUT is
// untouched by that change; only how often it is written is.
//
// The join-key argument above is not weakened by the sparser cadence — it is
// STRENGTHENED by it. The reason the ref must ride the message rather than be
// inferred from a delay is that the two channels are independent, and reducing
// the state cadence makes them MORE independent: with corrections arriving every
// ceil(N/K) ticks, a client that guessed the applied capture from a schedule
// would be guessing across a wider gap. The `appliedCaptureTick` field answers
// it exactly, at whatever cadence the state arrives.
//
// `appliedCaptureTick` is that statement: "the state at `tick` was produced by
// the input captured at `appliedCaptureTick`". It is the ONLY thing that
// correlates the two channels, which is why it rides the state message itself
// rather than being inferred from a delay (a delay is the INTENDED schedule and
// can disagree with what the authority actually applied — see
// RelayDelaySpectrumDesign.md §5.3, where the actual ref always wins).
//
// The value is produced by SimulationNetSync::collectInputAll (T2) and read back
// on the client by SimulationReconciliation::injectCorrectionState, which stores
// it in the correction-cache slot PARALLEL to the state it corrects (D3) — one
// ref per character PER TICK, because T6 needs a ref for every resim tick, not
// one scalar for the newest.
//
// kNoInputCaptureTick (below) is the explicit "no client capture stands behind
// this tick's input" value (D1) — the authority substituted an input on a
// RemoteMoveQueue underrun. It is a WIRE value: it round-trips through this
// payload untouched and the client resolves those ticks to game-zero rather than
// to a lookup that would silently hit a stale entry.
//
// ---------------------------------------------------------------------------
// WHY THIS LIVES IN CORE (same rationale as InputRedundancyBundleCodec.h and
// RelayedInputRingCodec.h).
//
// The UE-side wrapper FSimulationStateSyncBuffer
// (Source/OGSimulationUnreal/SyncedSimulationStateBuffer.h) is a USTRUCT owning a
// UPROPERTY TArray<uint8> plus NetSerialize; it DELEGATES the payload layout
// here. Hoisting the layout into core is what lets the pure-C++ Low-Level-Tests
// round-trip the REAL production framing (WireFormat/CorrectionStateBufferCodecTest.cpp)
// — the LLT target links only Core + OGSimulation and cannot see a USTRUCT at all.
//
// BUFFER CONCEPT: the templates below operate on a `Buffer` exposing
//   - template <typename T> void writeToBuffer(std::uint32_t off, const T& v);
//   - template <typename T> T    readFromBuffer(std::uint32_t off) const;
// the same two method names the sync buffers and the two other codecs already
// use, so one buffer type backs all of them without a second adapter surface.
// (This codec needs no grow/size methods: the correction buffer is a
// fixed-capacity kBufferBytes array, not an append-grown payload.)
//
// ---------------------------------------------------------------------------
// THE WIRE FENCE. kWireFormatVersion is the SINGLE version fence of the input-
// relay increment, bumped 1 -> 2 by T4 because this payload grew the second
// field, and 2 -> 3 by ring-out task 2 because the STATE COMPOSITE gained a
// whole sub-simulation (the full reasoning is at the constant itself, below —
// it is a different KIND of change from T4's and the difference is the point).
// The version BYTE itself is emitted by the UE buffer's NetSerialize (it
// is transport framing, not payload), and the refusal path lives in the
// adapter's correction-state arrival callback (one adapter binds it to
// `USimmableUpdateComponent::OnRep_CorrectionState`), which compares the
// received byte against this constant symbolically — so a mismatched build still
// fails loudly with no edit at the fence site. The relay ring's own version byte
// (relayedInputRing::kWireFormatVersion) is a NEW property's first format and is
// deliberately NOT part of this fence.
// ---------------------------------------------------------------------------

#include "OGSimulation/SimulationSerialization.h"
#include "OGSimulation/SimulationComposite.h"

#include <cstdint>
#include <limits>

// [T2/T4 / input relay] The "no real input was applied" sentinel for the applied-
// capture-tick track and for the wire field above.
//
// The authority's remote branch ALWAYS produces an input for the tick, but on a
// RemoteMoveQueue underrun that input is a SUBSTITUTE (a value-initialised
// Move{}), not something a client ever captured. There is therefore no capture
// tick to name, and any real-looking number we invented would be a lie the
// relay's capture-tick join key would then act on. The sentinel says so
// explicitly: T4 replicates it on the correction state, and T6 resolves those
// ticks to the proxy's last-known / game-zero input rather than to a capture the
// client can look up.
//
// uint32 max is chosen because the capture-tick domain is the server tick counter
// starting at 0 and bounded by the receive-side future guard
// (RemoteMoveQueue::queueMove) — it can never legitimately reach this value,
// while 0 is a perfectly ordinary real capture tick (see the underrun-detection
// comment in SimulationNetSync::collectInputAll).
//
// HOME (T4): declared here rather than in SimulationNetSync.h (its T2 home)
// because it is now needed by three layers that must not depend on netsync — the
// correction cache (per-slot default), this wire codec, and the UE buffer. Kept
// at global scope, matching the rest of the og-simulation core.
inline constexpr std::uint32_t kNoInputCaptureTick = std::numeric_limits<std::uint32_t>::max();

namespace correctionStateBuffer
{
	// Wire-format version of the CORRECTION STATE payload. 1 = Stage-1 format
	// (tick + composite); 2 = the input-relay format (tick + appliedCaptureTick +
	// composite); 3 = the input-relay format with a state composite that carries a
	// ring-out sub-simulation. See THE WIRE FENCE above.
	//
	// ⛔⛔ [ringout task 2, 2026-09-13] 2 -> 3, AND THE REASON IS NOT THE ONE THIS
	// FENCE USUALLY FIRES FOR. What moved: `simulatableBrawler::State` grew
	// 326 -> 335 B when `brawlerRingout::InitialConditions` (4 B) and
	// `brawlerRingout::State` (5 B) were APPENDED to the composite. An append
	// leaves every preceding field at the byte offset it already had, and that is
	// exactly why movement-sim tasks 11, 27 and 50 each declined this bump when
	// they grew a slice. Ring-out is a DIFFERENT CLASS OF CHANGE and the
	// precedent does not carry:
	//
	//   * Those tasks added FIELDS to sub-simulations BOTH builds compiled in.
	//     Every offset a stale peer knew about stayed put, and the only cost was
	//     bytes it ignored.
	//   * This task appends a WHOLE SUB-SIMULATION that an older build does not
	//     have at all. The payload layout cannot express that difference: the
	//     bytes are self-consistent in both directions and say nothing about
	//     which build has the sub-simulation.
	//
	// ⛔ The direction that matters is NEW client / OLD server. The new client's
	// `readCompositeFromSyncedBuffer` walks 335 bytes out of a payload the old
	// server wrote 326 bytes into; the trailing 9 come from whatever the
	// fixed-capacity kBufferBytes array last held at those offsets. Ring-out
	// state — the dead bit and the absolute respawn tick — is then restored from
	// garbage on EVERY correction, silently. The version byte is the only thing
	// that turns that into the loud, expected build-mismatch refusal instead.
	//
	// ⚠ "That pair is not reachable, one machine builds both halves" is a claim
	// about how this project is BUILT AND SHIPPED, and it is false here:
	// `Source/*.Target.cs` declares three separate targets (Server, Client,
	// Editor), and `PLAYTEST_PORTABLE_README.md` documents three archives cooked
	// INDEPENDENTLY and handed to a host PC with nothing enforcing one revision —
	// its own "Things that will not work" list names build-mixing as a known
	// hazard. `Saved/StagedBuilds/Windows{Server,Client}/` currently holds a
	// stale pair from 2026-07-20. Mixed builds are ordinary practice here, which
	// is why the arrival callback's refusal path already tells the user to "get
	// matching builds from Saved/Archive/".
	//
	// The forward direction (OLD client / NEW server) was benign and stays
	// benign: the append meant every field the old build knew about decoded
	// correctly and 9 trailing bytes were simply never read. It is now fenced too
	// — a loud refusal, not a silent capability gap where a stale client watches
	// characters teleport with no local explanation for the respawn.
	inline constexpr std::uint8_t kWireFormatVersion = 3;

	// Payload layout.
	inline constexpr std::uint32_t kTickOffset               = 0;
	inline constexpr std::uint32_t kAppliedCaptureTickOffset = sizeof(std::uint32_t);
	inline constexpr std::uint32_t kPayloadOffset            = 2 * sizeof(std::uint32_t);

	// Header bytes that precede the state composite. A payload shorter than this
	// carries no readable ref (a never-replicated buffer), which is what
	// FSimulationStateSyncBuffer::getAppliedCaptureTick guards on.
	inline constexpr std::uint32_t kHeaderBytes = kPayloadOffset;

	// Writes one correction: (tick, appliedCaptureTick, state).
	//
	// The two scalars are written before the composite so the composite's own
	// offsets stay a pure function of its field list — the reader below re-derives
	// them the same way, which is what keeps the two sides in lockstep.
	template <typename Buffer, typename... Ts>
	void write(Buffer& buffer,
	           const SimulationComposite<Ts...>& state,
	           std::uint32_t tick,
	           std::uint32_t appliedCaptureTick)
	{
		buffer.writeToBuffer(kTickOffset, tick);
		buffer.writeToBuffer(kAppliedCaptureTickOffset, appliedCaptureTick);
		writeCompositeToSyncedBuffer(state, buffer, kPayloadOffset);
	}

	// Mirror of write(). Returns the tick and fills `outState`; the applied
	// capture tick is handed back through the out-parameter so the (much more
	// common) tick-only read stays a one-liner at the call sites that do not care.
	template <typename Buffer, typename... Ts>
	std::uint32_t readInto(const Buffer& buffer,
	                       SimulationComposite<Ts...>& outState,
	                       std::uint32_t& outAppliedCaptureTick)
	{
		const std::uint32_t tick =
			buffer.template readFromBuffer<std::uint32_t>(kTickOffset);
		outAppliedCaptureTick =
			buffer.template readFromBuffer<std::uint32_t>(kAppliedCaptureTickOffset);
		readCompositeFromSyncedBuffer(outState, buffer, kPayloadOffset);
		return tick;
	}

	// Reads ONLY the join key. Used by the UE buffer's getAppliedCaptureTick so a
	// consumer that already has the state does not pay a second composite read.
	template <typename Buffer>
	std::uint32_t readAppliedCaptureTick(const Buffer& buffer)
	{
		return buffer.template readFromBuffer<std::uint32_t>(kAppliedCaptureTickOffset);
	}
} // namespace correctionStateBuffer
