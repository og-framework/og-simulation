<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SurfaceFrame.h` — guards

Every prohibition that governs a line of `SurfaceFrame.h`. Each entry has an **opaque, stable id**. In the
header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `SurfaceFrame.h` disagree, the header is authoritative and this file is stale.** Fix
this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved to §R
and its number is spent forever. A retired id may be **named** in prose or in a `static_assert` message; it
may never again appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** Why the tilt, the flat test and the angle-axis are shaped as
they are lives in `SurfaceFrame-rationale.md`. A guard is a prohibition plus the consequence of ignoring
it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is true
and actionable to a reader with no game engine, no other repository and no other file open.

---

## G-01 — The flat early-return stays. Never let a flat normal reach the general formula.

**Tag site:** `SurfaceFrame.h`, in the tilt template of `namespace detail`, immediately above
`if (isFlat(n))`.

**The prohibition.** Do not delete this early-return, and do not replace its condition by an exact
comparison against `simFrame::kUp`. A flat normal returns `v` itself: the same bits, `-0` included. Do not
reason that the Rodrigues expression "is the identity anyway" when the normal is up: it is the identity in
exact arithmetic, not in its bits.

**The consequence.** Measured with the early-return removed (2026-10-28,
`SurfaceFrame.FlatNormalsReturnTheirInputBits`, 1000 seeded vectors):
- at the exact `simFrame::kUp`, and at `kUp` with `-0` horizontal components, 667 of the 1000 vectors came
  back with different bits: every vector that has a `-0` component, which the sum `v + ...` turns into
  `+0`;
- at the near-flat normal `(5e-7, 1, -5e-7)`, which `isFlat` accepts, all 1000 changed, and `tiltMatrix`
  stopped being the exact identity.

So a caller whose flat path must return exactly what it returned before the tilt existed gets values that
are equal under `==`, or within 1e-6, but differ in their bits: an exact trace comparison or a determinism
pin of that caller fails, with nothing crashing.

**What breaks if it moves.** `tiltToSurface`, `tiltFromSurface` and `tiltMatrix` all reach the formula
through this one branch; the early-return covers all three only while it stays first in the template.
`SurfaceFrame.FlatNormalsReturnTheirInputBits` fails when it is removed (seen failing 2026-10-28).

---

## G-02 — The angle is `2 · atan2(|xyz|, w)`. Never `2 · acos(w)`.

**Tag site:** `SurfaceFrame.h`, in `surfaceFrame::toAngleAxis`, immediately above
`const float angle = 2.f * std::atan2(s, q.w);`.

**The prohibition.** Do not replace the angle with `2.f * std::acos(q.w)`, even though it is shorter and
needs no `s`.

**The consequence.** For a small rotation `w = cos(angle / 2)` is within a few float steps of 1, where
`acos` is steepest: the rounding of `w` becomes a large error in the angle. Measured with `acos`
(2026-10-28, `SurfaceFrame.AngleAxisReproducesTheTiltedSwingFrame`, 3600 aims): rebuilding the rotation
from the angle and axis missed the input by up to 2.01e-4 per matrix element on the flat set (4.47e-7 with
`atan2`) and by up to 2.0e-6 at a 10° tilt. A consumer that stores the angle-axis and rebuilds the
rotation from it gets an aim that is off by up to that much near zero.

**What breaks if it moves.** The angle is the only read of `s` and `q.w` together; the axis is `xyz / s`
whichever form is used, so a wrong angle is not visible in the axis.
`SurfaceFrame.AngleAxisReproducesTheTiltedSwingFrame` fails with `acos` (seen failing 2026-10-28).

---

## §R Retired ids

**None.**

The upward-normal precondition of the tilt is an `OG_CHECK` in the header, not a guard (rationale §2).

---

## Provenance

G-01 and G-02 were written with the header on 2026-10-28. Nothing in this file depends on any document
outside this submodule.
