<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationFrame.h` — rationale

The narrative behind `SimulationFrame.h`: the simulation's frame and length unit, the frame vocabulary
that code is written against, and the history of how the simulation moved into that frame. The header
keeps its licence, its code, its `static_assert`s and one `⛔G-nn` tag; everything that explains it
lives here.

**If this file and `SimulationFrame.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in
`SimulationFrame-guards.md`, reachable from the `⛔G-nn` tag at that site.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction (glm and the standard library only). Everything below is true to a reader with no game
engine and no other repository. Where a game engine, another library or a past change is named, it
is named as **provenance only**: nothing below depends on the reader being able to open it.

---

## §1 What the header is for

og-simulation works in **SI metres in a right-handed, Y-up frame**, so that vectors and transforms
cross the network, the simulation and the physics backend unconverted. A game engine that uses
another frame or unit converts at its own edge, once, on the way in and on the way out.

`simFrame` is that frame as a vocabulary: its axes (§4), its picks (§5) and the one unit factor an
engine edge needs (§8). Code is written against `simFrame` and never against a literal axis, a
component pick or a bare unit factor (§6), so a reader can see which vectors are frame vectors and
every pick keeps the sign of zero (`SimulationFrame-guards.md` G-01).

The frame and the unit are fixed. Each member is a literal, and a `static_assert` after the struct
pins the whole table, so a change to the frame is a visible, deliberate edit of both. §7 records how
the simulation got here.

---

## §2 The frame (D1)

The frame is **right-handed, +Y up, −Z forward, +X right**: Godot's frame, and the convention Jolt
documents (right-handed, Y up). glTF is right-handed and Y up as well.

| forward | right | up | handedness | length unit |
|---|---|---|---|---|
| −Z | +X | +Y | right | the metre |

Unreal Engine, the engine the simulation was first written against, is left-handed and Z up (forward
+X, right +Y, up +Z) and measures in centimetres. The map from Unreal's frame to this one sends
forward to forward, right to right and up to up: og = (ue.y, ue.z, −ue.x), scaled by 1/100 for a
length. It is a signed permutation with determinant −1, a reflection, so pseudovectors (rotation
axes, angular velocity, torque, cross products) map by its negative. Every formula built from dot
products, cross products, rotations about a routed axis and quaternions is covariant under it, so
formula code needed no edit when the simulation moved; only the four classes of site in §6 did.

---

## §3 The planar chart and the turn sign (D3): N, the native chart

Many quantities are two-dimensional: a ground-plane direction, a stick input, a planar angle. They
are stored in a **chart**, a choice of two axes in the ground plane.

- **N (shipped).** The chart is (forward, left). A positive planar angle turns counter-clockwise
  about `kUp` seen from above, from forward towards left, so the turn axis is `kUp`, which is
  `cross(kForward, kLeft)`, and the vertical swing pitches about `kRight`. This is the frame's own
  right-handed sense: forward, the chart's second axis and up form a right-handed triple, as the 3-D
  frame does (`kChartOrientation` is +1, §4).
- **P (before, provenance).** While the simulation moved from Unreal's frame (§7) the chart was
  (forward, right), and a positive turn went from forward towards right, as Unreal's yaw does
  ("Turning around (0=Forward, +Right, -Left)"). The turn axis was `cross(kForward, kRight)`, −Y, the
  vertical turn axis `cross(kUp, kForward)`, −X, and `kChartOrientation` was −1. P kept every planar
  value, angle and turn component unchanged while the 3-D frame moved; only 3-D vectors moved. Moving
  to N at the same time as the unit or axis change would have made a mirror indistinguishable from a
  frame bug, which is why P came first.

The change from P to N (og-sim-metres-yup task 26) changed three members, `kTurnAxis`,
`kVerticalTurnAxis` and `kChartOrientation`, added `kLeft`, and changed `planar` and `compose` to the
(forward, left) pair; `forwardRight` and `fromForwardRight` did not change (§5). The 3-D frame did not
change. Every authored planar angle was negated and every stored planar vector mirrored, which is
exact, so a game's behaviour is unchanged by it: a turn by −θ about `+Y` is the turn by θ about −Y.

---

## §4 The axis table and the three identities

The table is the `static constexpr` members `kUp`, `kForward`, `kRight`, `kLeft`, `kTurnAxis`,
`kVerticalTurnAxis`, `kChartOrientation` and `kUpIndex`:

| member | value |
|---|---|
| `kForward` | (0, 0, −1) |
| `kRight` | (1, 0, 0) |
| `kLeft` | (−1, 0, 0) |
| `kUp` | (0, 1, 0) |
| `kTurnAxis` | (0, 1, 0) |
| `kVerticalTurnAxis` | (1, 0, 0) |
| `kChartOrientation` | +1 |
| `kUpIndex` | 1 |

The derived members are written as literals and **checked** against the frame axes by
`static_assert`s inside the struct:
- `kLeft == -kRight`;
- `kTurnAxis == cross(kForward, kLeft)`, which is `kUp`;
- `kVerticalTurnAxis == cross(kForward, kUp)`, which is `kRight`;
- `kChartOrientation == dot(cross(kForward, kLeft), kUp)`, which is +1.

Each message names the chart-P value the assertion replaced (§3), so an edit back to it is a visible,
deliberate change.

Writing a pseudovector constant as a cross product of frame axes is what makes it right in any frame
(rule 3 of §6). `kChartOrientation` is the determinant of the matrix whose columns are forward, the
chart's second axis (left) and up: +1, because (forward, left, up) = (−Z, −X, +Y) is a right-handed
triple. Under chart P the second axis was right, and (−Z, +X, +Y) gave −1.

Two more `static_assert`s make the picks of §5 sound: every axis is a signed basis vector (one
component ±1, the others 0), and forward, right and up name three different components, with left on
right's component. A last one, after the struct, pins every value of the table bit for bit (§1).

---

## §5 The picks: chart picks and input-space picks

A **pick** reads a component along a frame axis; a **place** writes one. Every pick in the header is a
signed index pick: the component at the axis's index, negated when the axis is negative. The indices
and signs are derived at compile time from the table of §4 (`simFrameDetail::axisIndex`), so the
table is the only place the frame is spelled. That is also why a pick is never a dot product: see
`SimulationFrame-guards.md` G-01.

| member | what it does | result |
|---|---|---|
| `height(v)` | the signed pick along `kUp` | `v.y` |
| `withHeight(v, h)` | `v` with its up component replaced by `h` | (x, h, z) |
| `planar(v)` | (forward, left), the chart (§3) | (−z, −x) |
| `compose(p, h)` | the inverse of (`planar`, `height`) | (−p.y, h, −p.x) |
| `flatten(v)` | zeroes the up component only | (x, 0, z) |
| `turnComponent(a)` | the signed pick along `kTurnAxis` | `a.y` |
| `forwardRight(v)` | (forward, right), input space | (−z, x) |
| `fromForwardRight(p, h)` | the inverse of (`forwardRight`, `height`) | (p.y, h, −p.x) |
| `alongUp(diagonal)` | the **unsigned** component at `kUpIndex` | `d.y` |
| `forwardColumn(m)` | the signed column of a transform along `kForward` | `−m[2].xyz` |

**Two kinds of 2-D pick.** The *chart* picks (`planar`, `compose`, `turnComponent`) follow the chart
of §3, and they changed when the chart went from P to N. The *input-space* picks (`forwardRight`,
`fromForwardRight`) never change: they serve code whose 2-D values are defined relative to the viewer
or the character rather than to the chart (the stick-to-world mapping, motion matching on stick input,
spawn sort keys). Under P the two kinds had identical bodies, so each call site was classified once,
when it was routed, and the change to N touched only the chart kind. Under N they differ in the sign
of the second component: `planar(v).y` is `-forwardRight(v).y`.

`alongUp` is unsigned because it serves inertia diagonals: a diagonal entry is a moment about an
axis, and it does not change sign when the axis does.

`forwardColumn` reads where a transform sends `kForward`: the column at the forward axis's index,
negated when that axis is negative. A body posed by rotating `kForward` onto an aim keeps its aim in
that column, so the pose's forward is read the same way whatever the frame. Reading column 0 by index
instead would read the body's right vector, because column 0 is `kRight`'s image. It is a pick like
the others: one column copied by index, negated per component, so it returns the bits of `m[2]`
negated, `-0` included. The product `m * vec4(kForward, 0)` gives the same direction but adds the
other columns' zero-weighted terms and loses `-0`.

---

## §6 The routing rules

When a site is written against the header:
1. **A literal axis** becomes the vocabulary constant (`kUp`, `kForward`, `kRight`).
2. **A component pick** becomes `height`, `withHeight`, `planar`, `compose`, `flatten` or `alongUp`.
   - **2b.** A pick in **input space** (the stick-to-world mapping, the motion matcher, the spawn sort
     keys) uses `forwardRight` / `fromForwardRight` instead. Those never change under N.
3. **A pseudovector constant** becomes a cross product of frame axes (`kTurnAxis` is
   `cross(kForward, kLeft)`, `kVerticalTurnAxis` is `cross(kForward, kUp)`; an idle-pose axis is
   `cross(kForward, kUp)`), and a pseudovector pick becomes `turnComponent`.
4. **A cross product used as the chart's second axis** (for example the second axis of a tangent
   frame, cross(n, u), whose coefficients are chart values) is multiplied by `kChartOrientation`.
   Without the factor the reflection of §2 would have mirrored it when the frame moved, and every value
   built on it with it. The factor is a property of the chart, so it changed when the chart did. A
   cross product that only fixes a 3-D direction, with no chart value built on it (a drawing's
   perpendicular, say), does not take it: it takes the fixed sign of the direction it means.

Formula code (dot products, cross products, a rotation about a routed axis, quaternions) needs no
edit. A vocabulary constant that enters arithmetic is used directly, or through a `constexpr` local,
never through a run-time `const` copy: measured with MSVC under `/fp:fast`, a run-time copy of an axis
was not folded the way the literal was, and an exact zero component of a cross product came out `-0`
where the literal gave `+0`.

---

## §7 History: how the simulation moved (og-sim-metres-yup, 2026-10)

The simulation was written in Unreal's frame and unit: centimetres, Z up, left-handed. It moved to
the frame of §2 in stages, each a change that could be checked on its own (provenance: the
og-sim-metres-yup initiative, 2026-10):
1. **Two knobs.** An axis-frame knob (the Unreal frame or the Y-up frame) and a units-per-metre knob
   (100 or 1) selected the frame, and the vocabulary was a template over both. A test instantiated
   all four combinations in one binary.
2. **Routing at the old values.** Every axis, pick and frame-dependent constant was written against
   the vocabulary, and every centimetre literal went through a *wrapper* that returned the argument
   unchanged at 100 units per metre and the correctly rounded metre value at 1 (computed in double
   and rounded once; for an axis, the signed permutation of §2). With both knobs at their old values
   every float the game computed was unchanged, bit for bit, `-0` included.
3. **The unit flip**, then **the axis flip**: one line each, each gated by a comparison of a recorded
   game trace within absolute bounds.
4. **The cleanup.** A script rewrote every wrapper call to the shortest literal that round-trips the
   float the wrapper returned in the real build, and checked each bit pattern. Then the knobs, the
   template, the wrappers and their inverses were deleted, leaving the vocabulary of §4 and §5 with
   the values of the Y-up frame in metres. The inverses (from this frame back to Unreal centimetres)
   survive only in a game's recorded-trace test, which stores its trace in the old frame.

5. **The native chart.** Then the chart went from P to N (§3): the three chart members, `kLeft`,
   `planar` and `compose` changed, every authored planar angle was negated and every stored planar
   vector mirrored. The 3-D frame and the unit did not change, and a recorded game trace compared
   exactly through its planar converter's mirror.

Three findings from that work are still true of the toolchain:
- **A float made from a double in a constant expression.** With MSVC (`/std:c++20 /fp:fast`), in a
  constant expression a float produced from a double kept the double's value, so `float(0.3) == 0.3f`
  evaluated false although the bit pattern was the correctly rounded float. The wrappers narrowed
  through a round trip of the float's bit pattern (`std::bit_cast`) to make constant evaluation and
  run time agree.
- **The sign of a zero** (§6): a run-time copy of a vocabulary axis, or a negated axis
  (`-kUp * x`, `kUp * -x`), gives `-0` components where a literal had `+0`. `withHeight`, `compose`
  and `fromForwardRight` place a value without touching the other components, so they keep `+0`.
- **The engine edge converts once.** Every conversion between Unreal's frame and this one happens in
  the engine adapter, never in og-simulation (§8).

The log rule of §9 and the wire versions were frozen at the same cleanup: each printed length field
was spelled through a suffix helper and each length's decimals through a decimals helper while the
unit could change; both became literals once it could not.

---

## §8 The unit factor for an engine edge

`kUeCentimetresPerUnit` (100: a simulation length unit, the metre, is 100 Unreal centimetres) is the
one unit factor the header keeps. og-simulation itself never uses it: it is for an engine adapter that
reads or writes Unreal centimetres (a console variable in centimetres, a component location) and
converts at its edge. A `static_assert` after the struct pins it.

---

## §9 The log rule

A log line that reports simulation state prints metres and the simulation's own axes:
- each length prints with a fixed `%.<n>f`, where n is two more than the number of decimals the line
  printed in centimetres, so the printed precision is unchanged in absolute terms;
- each length field's name ends in `M` (`maxErrorM=`, `driftM=`), so a name always matches its unit;
- each vector prints its components as stored, so in this frame's axes.

Lines that report an engine's own objects (its component flags, its console variables, its build
identity) are not simulation state and keep the engine's units.

---

## §10 Tests

`SimulationFrameTest.cpp` (tag `[SimulationFrame]`) checks:
- every pick and place returns the exact bits of its input over a set of edge floats (±0, ±1,
  denormals, ±infinity, large and seeded values), and `std::signbit` of `-0` through each one;
- the axis table and the identities of §4 at run time (`kLeft` and the three chart members), and the
  unit factor;
- the picks against the table of §5;
- `planar` / `compose` / `flatten` / `withHeight` and `forwardRight` / `fromForwardRight` round-trip
  exactly;
- `forwardColumn` against a rotated `kForward` over seeded rotations (about `kTurnAxis` and about
  random axes) within 1e-6.

The header also asserts `simFrameDetail::picksKeepTheSignOfZero` for `simFrame` at compile time.
