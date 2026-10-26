<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationFrame.h` — guards

Every prohibition that governs a line of `SimulationFrame.h`. Each entry has an **opaque, stable id**. In
the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `SimulationFrame.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to §R and its number is spent forever. A retired id may be **named** in prose or in a `static_assert`
message; it may never again appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** Why the frame, the charts and the wrappers are shaped as
they are lives in `SimulationFrame-rationale.md`. A guard is a prohibition plus the consequence of
ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine, no other repository and no other file open.

---

## G-01 — Every pick is a signed index pick. Never a dot product.

**Tag site:** `SimulationFrame.h`, in `BasicSimFrame::signedPick`, immediately above
`if constexpr (Negate)`.

**The prohibition.** A pick reads one component by index, negated when the axis is negative; a
place writes one component the same way. Do not replace `signedPick` or `signedPlace`, or the body of
any pick that calls them (`height`, `planar`, `forwardRight`, `turnComponent`, `forwardColumn`, `compose`,
`fromForwardRight`, `withHeight`, the legacy maps and the inverses, such as `toLegacyAxes`), with a dot product
against an axis constant, a sum of axis-weighted components, or a multiply by an axis or a mask.
A new pick added to `BasicSimFrame` is written the same way.

**The consequence.** In IEEE arithmetic a dot product is not the component it looks like:
- a `-0` component becomes `+0`: `dot((-0, 1, 1), (1, 0, 0))` is `-0 + 0 + 0`, which is `+0`;
- an infinite component in **another** slot turns the result into NaN, because `inf * 0` is NaN;
- a multiply by a mask keeps `-0 * 0 = -0` in the slot it was meant to clear.

So the routed code stops returning the exact bits of the component it replaced. In the Ue frame
(`UeZUpLeftHanded`, at either unit: a pick reads a component and never scales it) a pick must be
bit-identical to the `.x`, `.y` or `.z` it replaced, `-0` included: the sign of zero is visible to
`atan2`, `std::signbit`, a division and any raw float32 write of the value. Nothing crashes; the change
shows up as a value that is equal under `==` but differs in its bits, or as a NaN where an infinity was
harmless.

**What breaks if it moves.** Every pick in the header goes through this one helper (and its
`signedPlace` twin). The namespace-scope `static_assert` on `simFrameDetail::picksKeepTheSignOfZero`
fails the build when any **listed** public pick loses the sign of zero (the header checks the shipped
`simFrame`; `SimulationFrameTest.cpp` checks all four knob combinations). It cannot see a pick it
does not list: a new pick added without a line in `picksKeepTheSignOfZero` is protected only by this
guard.

---

## §R Retired ids

**None.**

The axis identities (`kTurnAxis`, `kVerticalTurnAxis` and `kChartOrientation` against the cross and
dot products of the frame axes), the signed-basis and distinct-component checks, the two allowed
knob values and the metre wrappers' narrowing were never guards: each is a `static_assert` in the
header from the start (rationale §4 and §7).

---

## Provenance

G-01 was written with the header on 2026-10-25 and describes the header as first shipped. Nothing in
this file depends on any document outside this submodule.
