<!-- SPDX-License-Identifier: MPL-2.0 -->
# SimTrig — rationale

Source: `SimTrig.h`, namespace `simTrig`. It is the trigonometry the simulation uses wherever an angle or a rotation
becomes simulation state: `sin`, `cos`, `sinCos`, `tan`, `asin`, `acos`, `atan`, `atan2`, plus `rotate` and
`orientedAngle`, the two glm helpers the simulation called that compute with the C library's trigonometry.
Added by og-input-latency task 22.

This file has no guards document: it states no prohibition. Its two failure modes are caught by tests instead
(§3, §6).

<!-- lint-external-ref: Jolt/Math/Trigonometry.h -- Jolt Physics v5.6.0, not part of this repository -->
<!-- lint-external-ref: Jolt/Math/Vec4.inl -- Jolt Physics v5.6.0, not part of this repository -->
<!-- lint-external-ref: Vec4::SinCos -- Jolt Physics v5.6.0, not part of this repository -->
<!-- lint-external-ref: sATan2 -- Jolt Physics v5.6.0 (Vec4::sATan2), not part of this repository -->
<!-- lint-external-ref: glm/ext/matrix_transform.inl -- vendored glm under glm/, which the lint does not index -->
<!-- lint-external-ref: glm/gtx/vector_angle.inl -- vendored glm under glm/, which the lint does not index -->

---

## §1 Why the simulation does not call the C library's trigonometry

Every platform ships its own maths library (UCRT on Windows, glibc on Linux, bionic on Android), and each rounds
`sinf`, `cosf`, `acosf` and `atan2f` its own way. The standard asks only that they be close, not that they agree.
A simulation that calls them therefore computes other bits on a Windows server than on a Linux server or an Android
client, even with the same compiler mode, and a peer whose state differs in the last bit sees a correction that
disagrees with its prediction.

It was measured before this file existed. A brawler golden recorded by the same source on x64 Windows and on arm64
Android (og-input-latency task 21, under emulation) differed only in values that came from `glm::acos`: 25 of
90,216 compared samples, all in one guard pose, by up to 1.19e-7. Every field the physics engine solves was already
identical on both.

## §2 What it is: a scalar port of Jolt's trigonometry

The functions are the algorithms of Jolt Physics 5.6.0 (`Jolt/Math/Trigonometry.h`, which forwards to
`Vec4::SinCos`, `Tan`, `ASin`, `ACos`, `ATan` and `sATan2` in `Jolt/Math/Vec4.inl`), written for one float instead
of four lanes. Jolt's own header exists for this reason: its comment says the standard functions "are not platform
independent and will lead to non-deterministic simulation". Jolt took the polynomials from Stephen L. Moshier's
Cephes library.

It is a port, not a call into Jolt, because og-simulation does not depend on Jolt: the physics engine is one of its
backends, chosen by the host. Code built on og-simulation can use `simTrig` with no physics engine at all.

The file carries Jolt's MIT licence and copyright line instead of this repository's MPL-2.0, because it is derived
from Jolt's code. `LICENSES/MIT.txt` holds the text.

**It is the same function as Jolt's, bit for bit.** Each operation is Jolt's, in Jolt's order, with Jolt's
constants. Jolt computes all four lanes of a selection and keeps one, where the port computes only the branch it
keeps; the kept value is the same. Measured by og-input-latency task 22 with MSVC `/O2 /fp:precise` on x64:
3.1 × 10⁹ evaluations (every 7th float up to ±1000 for `sin`, `cos` and `tan`, every 3rd in [−1, 1] for `asin`
and `acos`, every 101st up to ±3 × 10³⁸ for `atan`, 2 × 10⁷ random pairs for `atan2`, and ±0, ±1, values just
outside ±1, ±10³⁰, ±infinity and NaN) gave no output whose bits differed from Jolt's.
`SimTrig.MatchesJoltBitForBit` (og-simulation-tests) repeats a sparser sweep on every run.

## §3 Why every platform computes the same bits

The functions use only addition, subtraction, multiplication, division and square root of floats, comparisons, a
float-to-integer truncation of a value below 2³¹, an integer-to-float conversion of that value, and bit operations
on the sign. IEEE 754 rounds each of these correctly, so every conforming platform gets the same result, on two
conditions:

- **No contraction.** A compiler that fuses a multiply and an add into one instruction rounds once instead of
  twice. The modules that compile simulation code build with `/fp:precise` on MSVC and `-ffp-contract=off` on
  clang (og-input-latency task 20), which forbid it.
- **No reordering.** A change to the order of an expression changes its rounding. The bit-for-bit test against
  Jolt (§2) and `SimTrig.RecordedOutputsAreFixed`, which pins eleven outputs to their bits, fail on it.

Measured (task 22): one probe, built with MSVC `/O2 /fp:precise` for x64 and with the Android NDK's clang 18
`-O3 -ffp-contract=off` for arm64 (run under emulation), wrote every output of one sweep to a file: 820,606 inputs
each for `sinCos` and `tan` (up to ±1000), 1,782,580 each for `asin` and `acos` (up to ±1.5), 105,798 for `atan`,
500,000 `atan2` pairs and 200,000 `rotate` matrices. Every output that is a number has the same bits on both.

What is left to the hardware is the sign of a NaN, which IEEE 754 does not fix: x86 produces a negative NaN and ARM a
positive one. So the 19,482 `atan2(±0, ±0)` outputs of that sweep are NaN on both, with different sign bits, and so
is the `cos` of an infinite argument.

Jolt's own version has one more platform-dependent step, the float-to-integer conversion of a quadrant of 2³¹ or
more, or of NaN: the x86 instruction returns `0x80000000` and Jolt's ARM path saturates, so Jolt's `Sin(1e30f)` is
−infinity on x64 and NaN on arm64 (measured). The port defines the result as `INT32_MIN`, the x86 value, for those
arguments, and its `sin(1e30f)` is −infinity on both.

## §4 Where it differs from the C library

| case | `simTrig` | C library |
|---|---|---|
| `asin`, `acos` of a value outside [−1, 1] | clamped: `asin(1.0000001f)` is π/2, `acos(-1.5f)` is π | NaN |
| `asin`, `acos` of NaN | π/2 (with the NaN's sign) and 0 | NaN |
| `atan2(±0, ±0)` | NaN (zero divided by zero) | ±0 or ±π |
| `sin`, `cos` of a very large argument | the first step of the reduction is exact only while the quadrant fits in 16 bits (an argument below about 10⁵, Jolt's comment); at 10³⁰ the result is infinite or NaN | accurate |

The clamp removes the NaN that the C library returns for a dot product of two unit vectors rounded just above 1.

**Accuracy** (task 22, maximum absolute error against double-precision `std::` functions over the probe's sweep):

| function | range | max error |
|---|---|---|
| `sin`, `cos` | ±1000 | 7.8e-8 |
| `asin` | [−1, 1] | 1.7e-7 |
| `acos` | [−1, 1] | 3.3e-7 |
| `atan` | ±3 × 10³⁸ | 1.4e-7 |
| `atan2` | random pairs in [−4, 4]² | 2.8e-7 |

`acos` is `π/2 − asin`, as in Jolt, so near 1 its error is absolute rather than relative: a small angle carries the
same absolute error as a large one.

## §5 `rotate` and `orientedAngle`

`simTrig::rotate(m, angle, axis)` is glm's `rotate` for a 4×4 matrix (`glm/ext/matrix_transform.inl`) with the
cosine and sine taken from `simTrig::sinCos`: the same axis normalisation, the same products in the same order, and
`m`'s fourth column copied. `simTrig::orientedAngle(from, to)` is glm's two-dimensional `orientedAngle`
(`glm/gtx/vector_angle.inl`) with `simTrig::acos`: the clamped dot product's angle, negated unless the 2-D cross
product is strictly positive.

So wherever the C library and `simTrig` return the same cosine and sine (about 72 % of random angles on Windows),
they return the same matrix and the same angle bit for bit. `SimTrig.RotateAndOrientedAngleEqualGlmWhereTheTrigAgrees`
checks it on 100,000 random cases.

## §6 Cost

Measured by og-input-latency task 22 on a brawler simulation built on og-simulation, stepped on one thread with
Jolt (two recorded scenes, a Win64 Development test executable, 5 rounds of 20 runs per scene, the executables before
and after the change on one tree): +0.12 µs per tick (+0.7 %) on a 2-character scene of 16.7 µs per tick, and
+0.64 µs per tick (+1.8 %) on a 7-character scene of 36.0 µs per tick. The second is within the spread between that
scene's rounds after the change (53.5 to 55.2 ms per 1,480 ticks).
