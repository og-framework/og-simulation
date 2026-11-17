<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimEnvelope.h` — guards

Every prohibition that governs the sim envelope in `Network/SimEnvelope.h`. Each entry has an **opaque, stable
id**. A single line `// ⛔G-nn  docs/SimEnvelope-guards.md` sits exactly where the forbidden edit would be typed.

**If this file and `SimEnvelope.h` disagree, the header is authoritative and this file is stale.** Fix this file;
do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved to §R and its
number is spent forever. A retired id may be **named** in prose; it may never again appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** The narrative lives in `SimEnvelope-rationale.md`. A guard is a
prohibition plus the consequence of ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is true and
actionable to a reader with no game engine, no other repository and no other file open.

---

## G-01 — a correction with another `corrVersion` passes through. Never refuse it.

**Tag site:** `SimEnvelope.h`, in `simEnvelope::detail::decodeCorrectionState`, immediately above
`if (message.corrVersion == correctionStateBuffer::kWireFormatVersion)`.

**The prohibition.** Do not add an `else` that refuses the message, do not return a refusal when the versions
differ, and do not check the payload's exact length before this comparison.

**The consequence.** A correction from a peer of another build is the only signal its receiver has that the two
builds differ. The receiver latches on it once: it logs an Error naming both versions, tells the player to get a
matching build, and ignores every later correction. A refusal here turns that one visible event into a counted
refusal per message, and the player sees a game that silently stops correcting. An exact-length check before
the comparison does the same, since another version's state has another length.

**What breaks if it moves.** `SimEnvelope.InnerVersionMismatchesPassThroughToTheirLatches` fails: a correction
with version 14 must decode as `None` with `corrVersion` 14 and its payload as the body.

---

## G-02 — a ring with another version passes through. Never refuse it.

**Tag site:** `SimEnvelope.h`, in `simEnvelope::detail::decodeRelayRing`, immediately above
`if (u8At(ring, relayedInputRing::kVersionOffset) == relayedInputRing::kWireFormatVersion)`.

**The prohibition.** Do not add an `else` that refuses the message, and do not check the ring's entry count or
exact length outside this branch.

**The consequence.** The ring's reader distinguishes a ring that was never written (version 0) from a ring of
another build (any other version) and reports each by name, without reading the entries. A refusal here replaces
both reports with one envelope count, and a build mismatch on the relay path is no longer told apart from noise.
An entry-count or length check outside the branch reads another build's layout as this build's, and refuses or
accepts it by accident.

**What breaks if it moves.** `SimEnvelope.InnerVersionMismatchesPassThroughToTheirLatches` fails: a ring with
version 2 must decode as `None` with the ring as the body.

---

## G-03 — `forEachInputBundle` checks the whole set before it visits any bundle. Never visit on the first walk.

**Tag site:** `SimEnvelope.h`, in `simEnvelope::forEachInputBundle`, immediately above
`return detail::walkInputBundleSet<S>(setBody, count, fn);`, the second walk.

**The prohibition.** Do not drop the first, checking walk, do not ignore its refusal, and do not pass `fn` to the
first walk to save the second.

**The consequence.** A set is checked front to back, so its truncation, a sim id 0 in a later entry, or trailing
bytes are found only after the earlier bundles were walked. Visiting on that walk hands the earlier bundles to
`fn` (into the server's input queue) and then refuses the message, so a malformed set is half applied. A refused
message is never applied at all.

**What breaks if it moves.** `SimEnvelope.ForEachInputBundleVisitsNothingInARefusedSet` fails: a set whose second
bundle is cut, whose second sim id is 0, or which has a trailing byte must visit nothing.

---

## §R — retired ids

None.
