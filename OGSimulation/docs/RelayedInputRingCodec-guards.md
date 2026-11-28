<!-- SPDX-License-Identifier: MPL-2.0 -->
# `RelayedInputRingCodec.h` — guards

Every prohibition that governs the relay ring codec. Each entry has an **opaque, stable id**. A single
line `// ⛔G-nn  docs/RelayedInputRingCodec-guards.md` sits in `RelayedInputRingCodec.h` exactly where
the forbidden edit would be typed.

**If this file and `RelayedInputRingCodec.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to §R and its number is spent forever. A retired id may be **named** in prose; it may never again
appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** The narrative lives in `RelayedInputRingCodec-rationale.md`.
A guard is a prohibition plus the consequence of ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine, no other repository and no other file open. "The
host" is whatever program owns the replicated ring and its stage.

**Provenance.** The header carried these as comments until og-input-latency task 8 (2026-11-11)
converted it. Each entry's first version is the text of the comment it replaces; R0 corrections are
marked. Two compile-time checks replaced comments outright and have no entry here: the two
`static_assert`s on `clampDepth` (rationale §6). Three more are new with the re-send: the one on
`clampResendEntries`, the one keeping `kMaxResendEntries` below `kMaxDepth`, and the one tying
`kMaxEntryBytes` to `kMaxWireBytes` (rationale §7 for the first two, §8 and §9 for the third).

---

## G-01 — a resident capture tick is rewritten in place, never appended and never asserted on

**Tag site:** `writeLatest`, inside the scan for a resident entry, immediately above
`if (captureTickAt<InputType>(buf, i) == captureTick)`.

**The prohibition.** Do not make a write whose capture tick is already resident append a second entry,
and do not add a check that fails on it. The ring does **not** inherit the inbound redundancy bundle's
append-only, immutable-per-capture-tick rule (`inputRedundancyBundle::appendSlot` checks against a
duplicate capture tick).

**Consequence.** A re-stamp of an already-relayed tick (the same input relayed again after a tier
change moved `dA`) must update, not assert. An append would put two entries with one capture tick in
the ring, and a consumer that keys by capture tick would read whichever it met first. An assertion
reproduces the exact failure this payload exists to avoid: a persistent property written again on every
newer capture tick, driven the bundle's way, overflows its slot budget and then asserts.

**What pins it.** `RelayRing.ReplaceRoundTripCarriesTheStamp` (`WireFormat/RelayedInputRingTest.cpp`)
revises a resident tick and requires the count to stay 1 and the new `dA` and value to be read back.

---

## G-02 — the stage's capacity is the constant `kMaxDepth`, never a parameter or a configurable value

**Tag site:** `stageArrival`, immediately above
`outcome.accepted = writeLatest<InputType>(stage, captureTick, dA, input, static_cast<std::int32_t>(kMaxDepth));`.

**The prohibition.** Do not replace `kMaxDepth` in this call with a parameter, a session setting, a
console variable or any other value that can be configured. Do not give `stageArrival` a depth
parameter. Do not add an overload that takes one.

**Consequence.** A configurable value threaded in here at anything below `kMaxDepth` makes the second
and every later staged entry supersede the first. The ring then carries exactly one entry per round,
and the flush degenerates into the replace-latest write path it exists to replace — with no compile
error and no warning. The relay re-send count of og-input-latency task 8 is **not** a depth: it is
`flushStagedInto`'s `resendEntries`, and it must never reach this call.

**What pins it.** `Network/RelayRedundancyDepthTest.cpp`: a pair of `static_assert`s that the real
4-argument call is valid and that no 5-argument, depth-shaped call compiles; and a runtime case that a
burst of `kMaxDepth + 1` resides at exactly `kMaxDepth`. `RelayRing.StagedBurstSurvivesAtTheSessionDefaultDepth`
(`WireFormat/RelayedInputRingTest.cpp`) is the runtime companion. The literal substitution itself (a
smaller constant in this call) is caught only by those runtime cases.

---

## G-03 — `resetEntries` truncates to `kHeaderBytes`, never to 0

**Tag site:** `resetEntries`, immediately above `buf.bundleTruncateTo(static_cast<std::int32_t>(kHeaderBytes));`.

**The prohibition.** Do not truncate to 0, and do not skip the truncate.

**Consequence.** `detail::initHeaderIfEmpty` is lazy: it writes the header only when the buffer is
completely empty. Truncating to 0 re-arms that laziness, and a ring emptied that way reads as version 0
until the next write. On the receiving side `populateRemoteInputCache` classifies a version-0 ring as
`NeverWritten` and silently drops it. Skipping the truncate leaves the entry bytes behind a zero count:
a stage keeps its high-water mark, and a ring the host ships at `bundleByteNum()` bytes sends them.

**What pins it.** `RelayRing.ResetEntriesShrinksToTheHeaderAndKeepsTheVersion`
(`WireFormat/RelayedInputRingTest.cpp`).

---

## G-04 — an empty stage touches nothing: suppress-clear-on-empty, and no re-send

**Tag site:** `flushStagedInto`, immediately above `if (stagedCount == 0u)`.

**The prohibition.** Do not make a flush with an empty stage write the destination ring: not to clear
it, not to rewrite it, and not to re-send its last entries. Do not move the early return below any
statement that writes `dst`.

**Consequence.** Suppress-clear-on-empty is the skip-recovery mechanism, not an optimization. With an
empty stage `dst` stays byte-identical, so the replication system's property comparison finds no
change, the property is not dirty, the object is not scheduled and costs zero bytes. And a round the
replication system **skipped** under packet pressure stays resident and dirty across the empty frames
that follow and is retried, instead of being overwritten by an empty flush and losing its whole burst.
At `resendEntries = 0` that retry is the only recovery there is. A re-send on an empty frame would
rewrite `dst` every frame: the property would be dirty on every poll, each idle character would cost a
ring's bytes per frame, and the skipped round would be overwritten by its own tail.

**What pins it.** `RelayRing.FlushWithAnEmptyStageLeavesTheRingByteIdentical`
(`WireFormat/RelayedInputRingTest.cpp`); `RelayResend: an empty stage re-sends nothing and leaves the
ring byte-identical` (`Network/RelayResendTest.cpp`) for the re-send half.

---

## G-05 — the re-send tail is read from the published ring and bounded by the room the stage leaves

**Tag site:** `flushStagedInto`, immediately above
`const detail::ResendTail tail = detail::takeResendTail(dst, stage, stagedCount, stride, clampResendEntries(resendEntries));`.

**The prohibition.**
- Do not source the tail from anywhere but `dst`, the ring as the last flush published it. Do not keep
  it in the stage, and do not keep a second buffer for it in the host.
- Do not pass `resendEntries` without `clampResendEntries`.
- Do not take the tail from a count other than `stagedCount`: the room is `kMaxDepth − stagedCount`.

**Consequence.**
- **Kept in the stage**, the tail makes the stage non-empty on a frame with no arrivals, so G-04's
  early return never fires again, and it takes up to two of the stage's `kMaxDepth` slots from new
  arrivals.
- **Kept in a second buffer**, it is a copy of the ring that can disagree with what was published: an
  entry the replication system never saw could be re-sent, or an entry it did see could be missed. `dst`
  needs no copy: by induction, its newest `r` entries are exactly the last `r` the host flushed
  (rationale §15).
- **Unclamped or unbounded**, the flush could publish more than `kMaxDepth` entries. The ring would
  then exceed what `isAcceptableWireLength` accepts from a full-depth ring, and the receiving transport
  would refuse the whole payload. The `OG_CHECK` after this statement stops a checked build. A shipping
  build compiles `OG_CHECK` out.

**What pins it.** `RelayResend: an entry count above kMaxDepth is unreachable` and `RelayResend: the
tail is the newest entries the ring last published` (`Network/RelayResendTest.cpp`).

---

## G-06 — the destination is re-shaped to exactly the published payload: truncate, then grow

**Tag site:** `flushStagedInto`, immediately above `dst.bundleTruncateTo(static_cast<std::int32_t>(kHeaderBytes));`.

**The prohibition.** Do not drop the truncate, and do not grow `dst` by anything but the tail's bytes
plus the stage's entry bytes.

**Consequence.** The transport ships `bundleByteNum()` bytes. Without the truncate, a quiet round after a
burst leaves `dst` at its high-water mark, and the stale trailing entries ride the wire every round. A
wrong growth leaves entries the header does not count, or counts entries that are zero bytes.

**What pins it.** `RelayRing.FlushShrinksTheRingWhenAQuietRoundFollowsABurst`
(`WireFormat/RelayedInputRingTest.cpp`).

---

## G-07 — the flush writes the version byte from `kWireFormatVersion`, never from a literal

**Tag site:** `flushStagedInto`, immediately above `dst.writeToBuffer(kVersionOffset, kWireFormatVersion);`.

**The prohibition.** Do not write a number literal here, and do not copy the stage's version byte
instead.

**Consequence.** A literal is left behind by the next version bump: every flushed ring would then carry
the old version, and every receiver would drop it as `VersionMismatch`. With the constant, a bump
carries the flush with it.

---

## §R Retired ids

None.
