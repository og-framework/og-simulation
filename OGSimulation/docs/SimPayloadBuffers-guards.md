<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimPayloadBuffers.h` — guards

Every prohibition that governs the three engine-free payload buffers in `Network/SimPayloadBuffers.h`. Each
entry has an **opaque, stable id**. A single line `// ⛔G-nn  docs/SimPayloadBuffers-guards.md` sits exactly
where the forbidden edit would be typed. All three sites are in `CorrectionStateBuffer`.

**If this file and `SimPayloadBuffers.h` disagree, the header is authoritative and this file is stale.** Fix
this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved to §R and
its number is spent forever. A retired id may be **named** in prose; it may never again appear as a `⛔G-nn`
**tag**.

⛔ **Nothing in this file is a rationale.** The narrative lives in `SimPayloadBuffers-rationale.md`. A guard is
a prohibition plus the consequence of ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is true and
actionable to a reader with no game engine, no other repository and no other file open.

---

## G-01 — `assignFromWire` sets the used length to the received length, never to the capacity

**Tag site:** `SimPayloadBuffers.h`, `CorrectionStateBuffer::assignFromWire`, immediately above
`m_usedBytes = static_cast<std::uint16_t>(wire.size());`.

**The prohibition.** Do not set `m_usedBytes` to `kBufferBytes`, to the previous `m_usedBytes`, or to any value
other than the length of the span just copied in. Do not clear or pad the copy to the capacity either.

**Consequence.**
- `payload()` returns the used prefix. With any other length, every reader of a received buffer's `payload()`
  sees bytes that were not on the wire: the tail of whatever the buffer held before.
- The received length is the only record of how many bytes the sender wrote. The sim envelope's correction
  framing has no length field of its own (the transport's message length gives it), so a check of a received
  correction's length against its state composite has only `usedBytes()` to read. A buffer that always reports
  384 B passes every short or truncated payload through such a check.
- `getAppliedCaptureTick` (guard G-03) decides "nothing received" from the used length. A short payload that
  reported the capacity would read a join key out of storage the wire never wrote.

**What breaks if it moves.** `SimPayloadBuffers.CorrectionPayloadRoundTripsThroughAssignFromWire` fails: the
received buffer's `usedBytes()` and `payload()` no longer equal the sender's.

## G-02 — `write` resets the used length before the codec writes

**Tag site:** `SimPayloadBuffers.h`, `CorrectionStateBuffer::write(value, tick, appliedCaptureTick)`,
immediately above `m_usedBytes = 0u;`.

**The prohibition.** Do not delete the reset, and do not move it after the call to
`correctionStateBuffer::write`.

**Consequence.** `writeToBuffer` only ever raises the used length (a high-water mark). Without the reset, a
publish into a buffer that last held a longer payload (one assigned from the wire, or a larger composite) keeps
the old length, and `payload()` sends the stale tail after the new state. Moved after the codec call, the reset
zeroes the length the codec just set, and `payload()` is empty.

**What breaks if it moves.** `SimPayloadBuffers.CorrectionWriteResetsTheWatermarkAfterALongerWirePayload`
fails (deleted: 300 B instead of 16 B; moved after: 0 B).

## G-03 — `getAppliedCaptureTick` answers the sentinel when no header was written or received

**Tag site:** `SimPayloadBuffers.h`, `CorrectionStateBuffer::getAppliedCaptureTick`, immediately above
`if (m_usedBytes < correctionStateBuffer::kHeaderBytes)`.

**The prohibition.** Do not delete the length test, and do not compare against anything but
`correctionStateBuffer::kHeaderBytes` (the 8 B that hold the tick and the applied capture tick).

**Consequence.** A buffer that has never been written or received holds zero bytes, and a payload shorter than
the header holds no join key. Without the test the read returns 0, or whatever the storage last held, and 0 is
an ordinary capture tick: the session's first. A client would then join a correction to an input no authority
named. `kNoInputCaptureTick` is the explicit "no capture stands behind this" value every reader already handles.

**What breaks if it moves.** `SimPayloadBuffers.CorrectionWithoutAHeaderAnswersTheSentinel` fails.

---

## §R Retired ids

None.
