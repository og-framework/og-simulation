#pragma once
// SPDX-License-Identifier: MPL-2.0

// ---------------------------------------------------------------------------
// CorrectionStateBufferCodec — the CORRECTION STATE payload (server -> clients).
//
// Engine-agnostic byte-codec for the every-frame authoritative state correction
// (og-netcode-v2-input-relay T4; InputRelayDesign.md §3 "STATE", D1/D3).
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
// at RECEIPT keyed by capture tick (T1/T3), state is corrected every frame keyed
// by the AUTHORITY tick. Nothing in either message says which input produced
// which state — so a client resimulating through a corrected tick cannot know
// which relayed input the authority actually fed into it.
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
// field. The version BYTE itself is emitted by the UE buffer's NetSerialize (it
// is transport framing, not payload), and the refusal path lives in
// USimmableUpdateComponent::OnRep_CorrectionState, which compares the received
// byte against this constant symbolically — so a mismatched build still fails
// loudly with no edit at the fence site. The relay ring's own version byte
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
	// composite). See THE WIRE FENCE above.
	inline constexpr std::uint8_t kWireFormatVersion = 2;

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
