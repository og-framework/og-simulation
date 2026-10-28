<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SurfaceFrame.h` — rationale

The narrative behind `SurfaceFrame.h`: the tilt of a surface against the simulation's up, its inverse, the
flat test that keeps a flat surface bit-exact, the up of a pose, and a canonical angle-axis form of a
rotation. The header keeps its licence, its code, an `OG_CHECK` and two `⛔G-nn` tags; everything that
explains it lives here.

**If this file and `SurfaceFrame.h` disagree, the header is authoritative and this file is stale.** Fix
this file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in `SurfaceFrame-guards.md`,
reachable from the `⛔G-nn` tag at that site.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by construction
(glm, the standard library and `SimulationFrame.h` only). Everything below is true to a reader with no game
engine and no other repository.

---

## §1 What the header is for

A surface is given by its unit normal `n`. The **tilt** `G(n)` is the rotation that takes `simFrame::kUp`
to `n` along the shortest arc: about the horizontal axis `cross(kUp, n)`, by the angle between `kUp` and
`n`. A consumer builds a quantity in the world's horizontal frame (an aim, a plane of motion) and tilts it
onto the surface with `tiltToSurface`, or brings a surface-relative quantity back with `tiltFromSurface`.
`tiltMatrix` is the same rotation as a matrix, for a consumer that composes it with other rotations.

Two companions sit with it: `surfaceUpOf`, the up of a pose (§4), and `toAngleAxis`, a canonical
angle-and-axis form of a rotation (§5), for a consumer that stores a tilted rotation compactly and rebuilds
it later.

Everything is in `namespace surfaceFrame`.

---

## §2 The tilt: Rodrigues without trigonometry

With `k = cross(kUp, n)` and `c = dot(kUp, n)` for a unit `n`, `|k|` is the sine and `c` the cosine of the
tilt angle, and

    G(n) v = v + k × v + k × (k × v) · 1 / (1 + c)

This is Rodrigues' rotation formula `v + sin θ (k̂ × v) + (1 − cos θ) k̂ × (k̂ × v)` with the unit axis
`k̂ = k / |k|` folded in: `sin θ · k̂ = k`, and `(1 − cos θ) / sin² θ = 1 / (1 + cos θ)`. No angle is ever
formed, so the tilt calls no `sin`, `cos` or `acos`: it is a fixed sequence of multiplies, adds and one
division, and it has no near-pole special case, because `k` going to zero is exactly the rotation going to
the identity.

`tiltFromSurface` is the same expression with `-k`: the rotation by the opposite angle about the same axis,
the inverse of `G(n)`. Both share one template in `namespace detail`, so the flat test (§3) and the
precondition below exist once.

The factor `1 / (1 + c)` grows without bound as `n` turns towards `-kUp`. The tilt is only for surfaces a
body can stand on, whose normals point up, so the precondition `c > 0` is an `OG_CHECK`. It is not
reachable from a flat normal: §3's test runs first.

**Measured** (`SurfaceFrame.WalkableTiltIsARotationTakingUpToTheNormal`, 1000 seeded unit normals tilted up
to 45°, 2026-10-28): `|G kUp − n|` at most 1.79e-7 per component, `|GᵀG − I|` at most 1.19e-7 per element,
and `tiltFromSurface(n, tiltToSurface(n, v)) − v` at most 1.19e-7 for `v` in the unit cube. The test bounds
are 2e-7, 4e-7 and 4e-7. For the 30° normal `(0, cos 30°, sin 30°)`, `tiltToSurface(n, kForward)` is
`(0, 0.5, −0.8660254)` within 1e-7 (`SurfaceFrame.ThirtyDegreeSlopeTiltsForwardUpTheSlope`): forward is
tilted up the slope. Replacing `(1 + c)` by `(1 − c)` makes `|GᵀG − I|` 0.18, and that test fails (seen
failing 2026-10-28).

---

## §3 The flat rule

`isFlat(n)` is true when the horizontal part of `n` is at most `kFlatTolerance` (1e-6) long, compared as
squares: `dot(flatten(n), flatten(n)) <= kFlatTolerance * kFlatTolerance`. For a unit normal that is a tilt
of at most about 1e-6 rad. It reads only the horizontal part, so `-kUp` is flat too; the tilt is never meant
for it (§2).

A flat normal returns its input: `tiltToSurface` and `tiltFromSurface` return `v` itself, and `tiltMatrix`
returns the exact identity. This is what lets a consumer add a tilt to an existing computation without
changing a single bit of it on flat ground: on a flat surface, the code after the tilt sees exactly the
values it saw before the tilt existed. The general formula would not give that. In exact arithmetic it is
the identity at `n = kUp`, but in float it turns every `-0` component into `+0`, and a near-flat normal that
`isFlat` accepts moves every vector by rounding-level amounts (`SurfaceFrame-guards.md` G-01 has the
measurement). So the flat case is an early-return, never a special value fed into the formula.

The tolerance is wider than zero so that a normal that is flat up to the rounding of whatever produced it
(a contact normal of a horizontal face, a normalized sum) also takes the exact path.

---

## §4 `tiltMatrix` and `surfaceUpOf`

`tiltMatrix(n)` starts from the identity and replaces each column by its tilt, so its columns are the
images of the frame's three basis vectors. Flat, every column comes back unchanged and the result is the
identity in its bits. Tilted, `tiltMatrix(n) * v` equals `tiltToSurface(n, v)` up to rounding: the matrix
product sums its terms in a different order than the formula does.

`surfaceUpOf(pose)` is the pose's image of `kUp`: the column at `simFrame::kUpIndex` of the pose's
rotation part, read as a signed index pick (negated when `kUp` is a negative axis, the same way
`simFrame::forwardColumn` reads forward). The translation column is not read. A pose that only turns about
`±kUp` has a flat up (`SurfaceFrame.SurfaceUpOfAYawIsFlatAndOfATiltIsNot`, 360 angles about each sign); a
pose built from `tiltMatrix(n)` has `n` as its up, within 1e-7 at 30°.

---

## §5 `toAngleAxis`: the canonical angle and axis

`toAngleAxis(r)` converts a rotation matrix to its unit quaternion and reads the angle and the unit axis
from it. A rotation has two quaternions, `q` and `-q`. The header picks the one with `w >= 0`, so the angle
`2 · atan2(|xyz|, w)` lies in `[0, π]` and every rotation has one encoding, except a rotation by exactly π,
whose two axes `±a` are both kept as valid. A consumer that stores the pair and compares two of them (a
prediction against an authority, a replay against a recording) therefore compares one rotation with one
encoding, not with whichever sign the conversion happened to produce. Near a half turn the canonical form
is visible: a rotation by `π + δ` about `a` comes back as `π − δ` about `-a`
(`SurfaceFrame.AngleAxisIsCanonicalNearAHalfTurn`).

The angle uses `atan2` because it is well conditioned across the whole range: at small angles it reads the
angle from the sine part `|xyz|`, where `acos(w)` would have to recover it from a `w` that rounds to almost
exactly 1 (`SurfaceFrame-guards.md` G-02 has the measurement). Both `atan2` and `xyz / |xyz|` are unchanged
by a positive scale of the quaternion, so the `glm::normalize` of the converted quaternion only removes a
rounding-level scale; it is not what makes the result unit length.

A zero rotation has no axis. The header returns angle 0 about `kUp`
(`SurfaceFrame.AngleAxisOfTheIdentityIsZeroAboutUp`), so the identity reads as no turn about up.

**Measured** (`SurfaceFrame.AngleAxisReproducesTheTiltedSwingFrame`, 2026-10-28): for `r` = a tilt at 0°,
10°, 30° and 45° times each of 3600 turns about `kUp` (one per 0.1°), rebuilding the rotation from the angle
and axis with glm's angle-axis rotation reproduces `r` within 5.96e-7 per element, against a test bound of
1e-6; the angle never exceeds π.

---

## Provenance

Written with the header on 2026-10-28, for a consumer that aligns attacks with the ground under a body
(provenance only: nothing above depends on it).
