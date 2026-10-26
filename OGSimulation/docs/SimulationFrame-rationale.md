<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationFrame.h` — rationale

The narrative behind `SimulationFrame.h`: the two compile-time knobs that select the simulation's
axis frame and length unit, the frame vocabulary that code is written against, the legacy wrappers
that let a centimetre, Z-up literal mean the same thing under every knob value, and the two log
helpers. The header keeps its licence, its code, its `static_assert`s and one `⛔G-nn` tag;
everything that explains it lives here.

**If this file and `SimulationFrame.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in
`SimulationFrame-guards.md`, reachable from the `⛔G-nn` tag at that site.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction (glm and the standard library only). Everything below is true to a reader with no game
engine and no other repository. Where a game engine, another library or a planned change is named, it
is named as **provenance only**: nothing below depends on the reader being able to open it.

---

## §1 What the header is for

og-simulation is moving from the frame and unit it inherited from Unreal Engine (centimetres, Z up,
left-handed) to SI metres in a right-handed Y-up frame, so that vectors and transforms cross the
network, the simulation and the physics backend unconverted, and conversion happens only at the
engine edges.

The move is staged behind two knobs, so that each step is a one-line change that can be checked on
its own:

| knob | type | shipped value | the other value |
|---|---|---|---|
| `kAxisFrame` | `AxisFrame` | `YUpRightHanded` | `UeZUpLeftHanded` (Unreal's axes, the legacy frame) |
| `kUnitsPerMetre` | `float` | `1.f` (metres) | `100.f` (centimetres, the legacy unit) |

`simFrame` is `BasicSimFrame` instantiated at the two knobs. Code is written against `simFrame` and
never against a literal axis, a component pick or a bare unit factor (§6). The template is public so
that a test can instantiate all four knob combinations in one binary, which is how
`SimulationFrameTest.cpp` checks every member.

At the legacy knobs (`UeZUpLeftHanded`, 100 units per metre: the values both knobs had before either
was flipped) every member returns exactly what the code it replaces computed, bit for bit, `-0`
included. Routing a site through the header therefore changes no float the game computes; only
flipping a knob does.

The staging, as planned when the header was added: route every site with the knobs at their legacy
values; flip the units; flip the axes; then remove the wrappers and the knobs, keeping the
vocabulary; then adopt the native turn convention of §3.

---

## §2 The target frame (D1)

The target frame is **right-handed, +Y up, −Z forward, +X right**: Godot's frame, and the convention
Jolt documents (right-handed, Y up). glTF is right-handed and Y up as well. That frame is
`YUpRightHanded`, the frame the simulation runs in since the axis flip; the frame it inherited from
Unreal is `UeZUpLeftHanded`.

| frame | forward | right | up | handedness |
|---|---|---|---|---|
| `UeZUpLeftHanded` | +X | +Y | +Z | left |
| `YUpRightHanded` | −Z | +X | +Y | right |

The map from the Ue frame to the Y frame sends forward to forward, right to right and up to up:
og = (ue.y, ue.z, −ue.x), written M below. M is a signed permutation with determinant −1, a
reflection. A reflection moves true vectors by M and pseudovectors (rotation axes, angular velocity,
torque, cross products) by −M. Every formula built from dot products, cross products, rotations about
a routed axis and quaternions is covariant under it, so formula code needs no edit; only the four
classes of site in §6 do.

---

## §3 The planar chart and the turn sign (D3): P now, N later

Many quantities are two-dimensional: a ground-plane direction, a stick input, a planar angle. They
are stored in a **chart**, a choice of two axes in the ground plane.

- **P (now, and through both knob flips).** The chart is (forward, right). A positive turn goes from
  forward towards right, as Unreal's yaw does ("Turning around (0=Forward, +Right, -Left)"). The turn
  axis is `cross(kForward, kRight)`: +Z in the Ue frame, −Y in the Y frame. Under P every planar
  value, angle and turn component keeps its value across both flips; only 3-D vectors move.
- **N (a planned later change).** The chart becomes (forward, left), turns go counter-clockwise about
  `kUp`, and the vertical swing pitches about `kRight`. Every authored planar angle is negated and
  every stored planar vector mirrored. Doing that at the same time as a units or axis flip would make
  a mirror indistinguishable from a frame bug, which is why P comes first.

Under N three members change: `kTurnAxis` becomes `kUp`, `kVerticalTurnAxis` becomes `kRight` and
`kChartOrientation` becomes +1, and their three `static_assert`s change with them. `planar` and
`compose` change; `forwardRight` and `fromForwardRight` do not (§5).

---

## §4 The axis tables and the three identities

The two tables are the `static constexpr` members `kUp`, `kForward`, `kRight`, `kTurnAxis`,
`kVerticalTurnAxis`, `kChartOrientation` and `kUpIndex`:

| member | Ue frame | Y frame |
|---|---|---|
| `kForward` | (1, 0, 0) | (0, 0, −1) |
| `kRight` | (0, 1, 0) | (1, 0, 0) |
| `kUp` | (0, 0, 1) | (0, 1, 0) |
| `kTurnAxis` | (0, 0, 1) | (0, −1, 0) |
| `kVerticalTurnAxis` | (0, 1, 0) | (−1, 0, 0) |
| `kChartOrientation` | +1 | −1 |
| `kUpIndex` | 2 | 1 |

The pseudovector members are written as literals and **checked** against the frame axes by
`static_assert`s inside the template, for every instantiation:
- `kTurnAxis == cross(kForward, kRight)`;
- `kVerticalTurnAxis == cross(kUp, kForward)`;
- `kChartOrientation == dot(cross(kForward, kRight), kUp)`.

Writing a pseudovector constant as a cross product of frame axes is what makes it right in both
frames (rule 3 of §6). `kChartOrientation` is the determinant of the map whose columns are forward,
right and up: +1 in the Ue frame, −1 in the Y frame.

Two more `static_assert`s make the picks of §5 sound: every axis is a signed basis vector (one
component ±1, the others 0), and forward, right and up name three different components.

---

## §5 The picks: chart picks and input-space picks

A **pick** reads a component along a frame axis; a **place** writes one. Every pick in the header is a
signed index pick: the component at the axis's index, negated when the axis is negative. The indices
and signs are derived at compile time from the tables of §4 (`simFrameDetail::axisIndex`), so the
tables are the only place a frame is spelled. That is also why a pick is never a dot product: see
`SimulationFrame-guards.md` G-01.

| member | what it does | Ue frame | Y frame |
|---|---|---|---|
| `height(v)` | the signed pick along `kUp` | `v.z` | `v.y` |
| `withHeight(v, h)` | `v` with its up component replaced by `h` | (x, y, h) | (x, h, z) |
| `planar(v)` | (forward, right) | (x, y) | (−z, x) |
| `compose(p, h)` | the inverse of (`planar`, `height`) | (p.x, p.y, h) | (p.y, h, −p.x) |
| `flatten(v)` | zeroes the up component only | (x, y, 0) | (x, 0, z) |
| `turnComponent(a)` | the signed pick along `kTurnAxis` | `a.z` | `−a.y` |
| `forwardRight(v)` | (forward, right), input space | (x, y) | (−z, x) |
| `fromForwardRight(p, h)` | the inverse of (`forwardRight`, `height`) | (p.x, p.y, h) | (p.y, h, −p.x) |
| `alongUp(diagonal)` | the **unsigned** component at `kUpIndex` | `d.z` | `d.y` |
| `forwardColumn(m)` | the signed column of a transform along `kForward` | `m[0].xyz` | `−m[2].xyz` |

**Two kinds of 2-D pick.** The *chart* picks (`planar`, `compose`, `turnComponent`) follow the chart
of §3 and change under N. The *input-space* picks (`forwardRight`, `fromForwardRight`) never change:
they serve code whose 2-D values are defined relative to the viewer or the character rather than to
the chart (the stick-to-world mapping, motion matching on stick input, spawn sort keys). Under P the
two kinds have identical bodies, so each call site is classified once, when it is routed, and the N
change touches only the chart kind.

`alongUp` is unsigned because it serves inertia diagonals: a diagonal entry is a moment about an
axis, and it does not change sign when the axis does.

`forwardColumn` reads where a transform sends `kForward`: the column at the forward axis's index,
negated when that axis is negative. A body posed by rotating `kForward` onto an aim keeps its aim in
that column, so the pose's forward is read the same way in both frames. Reading column 0 by index
instead would read the body's right vector in the Y frame, where column 0 is `kRight`'s image. It is
a pick like the others: one column copied by index, negated per component, so it returns the bits of
`m[0]` in the Ue frame and of `m[2]` negated in the Y frame, `-0` included. The product
`m * vec4(kForward, 0)` gives the same direction but adds the other columns' zero-weighted terms and
loses `-0`.

---

## §6 The routing rules

When a site is moved onto the header:
1. **A literal axis** becomes the vocabulary constant (`kUp`, `kForward`, `kRight`).
2. **A component pick** becomes `height`, `withHeight`, `planar`, `compose`, `flatten` or `alongUp`.
   - **2b.** A pick in **input space** (the stick-to-world mapping, the motion matcher, the spawn sort
     keys) uses `forwardRight` / `fromForwardRight` instead. Those never change under N.
3. **A pseudovector constant** becomes a cross product of frame axes (`kTurnAxis` is
   `cross(kForward, kRight)`, `kVerticalTurnAxis` is `cross(kUp, kForward)`; an idle-pose axis
   (0, −1, 0) in the Ue frame is `cross(kForward, kUp)`), and a pseudovector pick becomes
   `turnComponent`.
4. **A cross product used as a true direction** (for example the second axis of a tangent frame,
   cross(n, u)) is multiplied by `kChartOrientation`. Without the factor the reflection of §2
   mirrors it, and every value built on it mirrors with it. `SimulationFrameTest.cpp` reproduces this
   case: with the factor the tangent frame maps by M; without it the second axis comes out negated.

Formula code (dot products, cross products, a rotation about a routed axis, quaternions) needs no
edit. A pure axis in production code is written with the vocabulary, never with a legacy wrapper (§7).

---

## §7 The legacy wrappers

A test literal, or an authored constant, is written today in Ue centimetres. A **legacy wrapper**
takes that literal and returns the same physical quantity in the frame and unit the knobs select, so
the literal keeps its number and its meaning through both flips.

**Lengths.** At 100 units per metre every length wrapper returns its argument unchanged. At 1 unit per
metre it returns the correctly rounded metre value, computed in double and rounded once:
- `legacyCm(cm)` returns cm / 100;
- `legacyCm2(cm2)` (inertia, torque, squared distances) returns cm2 / 10⁴;
- `legacyPerCm3(perCm3)` (densities) returns perCm3 × 10⁶.

`legacyCm` has a `double` overload for double-typed tolerances; it returns its argument at 100 and
cm / 100.0 at 1. There is no integer overload: an `int` argument is ambiguous between the two and
does not compile, so a caller states the type it means.

Dividing in double and rounding once gives the correctly rounded result for every float input. A
float cannot be exactly 100 (or 10⁴) times a midpoint between two floats, so the exact quotient
lies at least about 2⁻³¹ of its magnitude away from one, far outside the double's 2⁻⁵³ rounding error;
the product by 10⁶ is exact in double. A sample of 200,000 random float bit patterns agreed with
exact rational rounding for all three wrappers.
That differs, by one ULP for some values, from computing cm × 0.01f in float.

**The narrowing step.** The metre results go through `narrowToFloat`, a round trip through the
float's bit pattern, rather than a plain `static_cast`. Measured with MSVC (`/std:c++20 /fp:fast`)
on this header: in a constant expression, a float produced from a double keeps the double's value,
so `float(0.3) == 0.3f` and `legacyCm(30.f) == 0.3f` evaluated false, although the bit pattern
(`0x3E99999A`) was the correctly rounded float. The round trip makes constant evaluation see the
same float as run time. A header `static_assert` checks `legacyCm(30.f) == 0.3f` and
`legacyCm(42.f) == 0.42f` at 1 unit per metre, and fails if the step is replaced by a plain cast.

**Vectors and rotations.** The axis maps are exact signed permutations, derived from the tables of §4
(they place the Ue x, y and z components at the forward, right and up slots):

| wrapper | maps by | Y frame result | scale |
|---|---|---|---|
| `legacyPosition(x, y, z)` | M | (y, z, −x) | length |
| `legacyAxes(x, y, z)` | M | (y, z, −x) | none |
| `legacyExtents(x, y, z)` | \|M\| | (y, z, x) | length |
| `legacyAxesUnsigned(x, y, z)` | \|M\| | (y, z, x) | none |
| `legacyAxial(x, y, z)` | −M | (−y, −z, x) | none |
| `legacyTorque(x, y, z)` | −M | (−y, −z, x) | squared length |
| `legacyInertia(ixx, iyy, izz)` | \|M\| | (iyy, izz, ixx) | squared length |
| `legacyRotation(w, x, y, z)` | (w, −M v) | (w, −y, −z, x) | none |
| `legacyPlanarCm(x, y)` | the P chart (unchanged) | (x, y) | length |

Which one to use:
- a direction, or a vector already in the physics backend's own metres: `legacyAxes`; such half
  extents: `legacyAxesUnsigned`;
- a point or an offset with a length: `legacyPosition`; box half extents in centimetres:
  `legacyExtents`;
- a rotation axis or an angular velocity: `legacyAxial`; a torque: `legacyTorque`;
- an inertia diagonal: `legacyInertia`; a rotation: `legacyRotation`;
- a planar length vector: `legacyPlanarCm`.

Never wrap a non-length: angles, times, counts, gains, ratios, masses, friction and restitution keep
their literals.

**Why the quaternion rule is (w, −M v).** A quaternion's vector part is a rotation axis scaled by the
sine of half the angle, so it is a pseudovector and maps by −M. With q mapped that way, rotating a
mapped vector by the mapped quaternion equals mapping the rotated vector.
`SimulationFrameTest.cpp` checks this over seeded cases at all four knob combinations, within 1e-6.

The extents, the inertia diagonal and the unsigned axes map by |M| because a half extent and a
diagonal moment are magnitudes along an axis, not components of a vector: they permute, but never
change sign.

---

## §8 The inverses

A recorder that stores values in one canonical frame (Ue centimetres, Z up, chart P) reads them back
with the inverses, in double:
- `toLegacyCm(units)` returns units at 100 and units × 100 at 1 (exact for a float input);
- `toLegacyAxes(v)` inverts `legacyAxes`: (−v.z, v.x, v.y) in the Y frame;
- `toLegacyAxial(v)` inverts `legacyAxial`: (v.z, −v.x, −v.y) in the Y frame;
- `toLegacyPosition(p)` is `toLegacyAxes` followed by `toLegacyCm`;
- `toLegacyRotation(q)` is (w, `toLegacyAxial` of the vector part);
- `toLegacyPlanar(p)` is the identity under chart P. It converts a chart value (no unit), and it is
  the one inverse that changes under N.

---

## §9 The log rule and the two log helpers

A log line that reports simulation state prints metres and the simulation's own axes, whatever the
knobs are, so a field name always matches its unit:
- each length prints with `%.*f` and `lengthDecimals(n)`, where n is the number of decimals the line
  prints today; `lengthDecimals` returns n at 100 units per metre and n + 2 at 1, so the printed
  precision is unchanged in absolute terms;
- each length field whose name ends in `Cm` prints its suffix with `%s` and `kLengthFieldSuffix`,
  which is `"Cm"` at 100 units per metre and `"M"` at 1;
- each vector prints its components as stored.

At 100 units per metre the text is byte-identical to what the line printed before it was routed; at 1
the same line prints metres, with the `M` suffix and two more decimals per length. Lines
that report an engine's own objects (its component flags, its console variables, its build identity)
are not simulation state and keep the engine's units.

`kUnitsPerCentimetre` (U / 100) and `kUeCentimetresPerUnit` (100 / U) are the unit factors as
constants, for the few places that need a factor rather than a wrapped literal.

---

## §10 Tests

`SimulationFrameTest.cpp` (tag `[SimulationFrame]`) instantiates all four `BasicSimFrame`
combinations and checks:
- at the Ue frame and 100 units per metre, every wrapper, pick and inverse returns its input bit for
  bit, `std::signbit` of `-0` included;
- both axis tables, and the three identities of §4 at run time;
- the Y frame's picks and maps against the tables of §5 and §7, and the inverses as exact round trips;
- `planar` / `compose` / `flatten` / `withHeight` and `forwardRight` / `fromForwardRight` round-trip
  exactly at every combination;
- `forwardColumn` against the table of §5, and against a rotated `kForward` over seeded rotations
  (about `kTurnAxis` and about random axes) within 1e-6;
- 30 cm becomes `0.3f` and 42 cm `0.42f`, bit for bit, at run time and in a `static_assert`;
- the two log helpers;
- the quaternion rule of §7 over seeded cases;
- the tangent frame of §6 rule 4 with and without `kChartOrientation`, and the turn component of a
  cross product, over seeded cases.

It also asserts `simFrameDetail::picksKeepTheSignOfZero` for all four combinations at compile time.
