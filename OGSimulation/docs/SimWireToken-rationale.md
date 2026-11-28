<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimWireToken.h` — rationale

The narrative behind `Network/SimWireToken.h`: og-simulation's part of a build's wire token, and the small compile-time
type every part of the token is written in. The header keeps its licence, its docs pointer and its code; everything that
explains it lives here.

**If this file and `SimWireToken.h` disagree, the header is authoritative and this file is stale.** Fix this file; do not
soften the header to match it.

The header states no prohibition, so there is no guards file: what could go wrong is a compile error (§3).

**Provenance.** Written with the header by og-session-runtime task 21 (2026-11-24). Before it, a game built the whole token
from the six version constants itself, and so had to include the header of every part that owns one.

---

## §1 What a wire token is

Two peers can play together only if they read each other's bytes the same way. Each layout that crosses the network has a
version number, and a game puts all of them in one short string, the **wire token**, that a client presents at login. A
server refuses a client whose token differs from its own, before the player is admitted into a session that cannot work.

The token is a list of fields separated by `.`; each field is a short prefix followed by a version in decimal without
leading zeros, e.g. `c15`. The parts of the network stack each own some fields, and each part exports them as one
**fragment**. A game joins the fragments, in a fixed order, into the whole token. A version bump in one part then changes
that part's fragment and nothing else: the game's code does not change, only the string its own pin expects.

## §2 og-simulation's fragment: `simWireToken::kSimFragment`

`kSimFragment` is `c<correction>.r<ring>.b<bundle>.e<envelope>`, built from og-simulation's four wire versions:

| field | constant | header | today |
|---|---|---|---|
| `c` | `correctionStateBuffer::kWireFormatVersion` | `CorrectionStateBufferCodec.h` | 15 |
| `r` | `relayedInputRing::kWireFormatVersion` | `RelayedInputRingCodec.h` | 3 |
| `b` | `inputRedundancyBundle::kWireFormatVersion` | `InputRedundancyBundleCodec.h` | 3 |
| `e` | `kSimEnvelopeVersion` | `Network/SimEnvelope.h` | 1 |

So today's fragment is `c15.r3.b3.e1`. The first three are the version bytes the three payloads carry; the fourth is the
first byte of every sim message (`SimEnvelope-rationale.md`). The order is the order the fields entered the token, and it is
part of the format: a peer that wrote the same versions in another order would present a different token.

## §3 The type: `Token`, `tokenOf` and `join`

- **`Token`** is a fixed buffer of `kCapacity` (32) characters and a length; `view()` reads it as a `std::string_view`. It
  needs no allocation, so it can be a `constexpr` variable and be compared in a `static_assert`.
- **`tokenOf({ {prefix, version}, ... })`** writes each field as its prefix and the version in decimal (one to three digits
  for a `uint8_t`), with a `.` between fields.
- **`join({ fragment, ... })`** writes the fragments one after another with a `.` between them. An empty fragment adds
  nothing, not even a separator, so a part with no field of its own can still take part in a join.
- **All three are `consteval`.** They run only at compile time, so writing past the buffer is not undefined behaviour at
  run time: it is not a constant expression, and the build fails. 32 characters hold the longest whole token today's six
  fields can make, `c255.r255.b255.e255.ro255.ck255` (31 characters); a field added to any fragment must keep the longest
  token inside the buffer, or the test row that writes it (§4) stops compiling.

## §4 The tests

`SimWireTokenTest.cpp` in og-simulation-tests (tag `[Network][SimWireToken]`):

- a `static_assert` pins `kSimFragment` to `c15.r3.b3.e1`. A bump of any of the four versions fails it, together with the
  pin of the whole token in the game that joins this fragment;
- a `static_assert` writes six-field tokens through `tokenOf` and `join` (today's `c15.r3.b3.e1.ro1.ck2`, older ones, and the
  longest, 31 characters): the format rows the game's own token builder carried before task 21;
- a `static_assert` joins a fragment, an empty one and another, and expects no extra separator;
- one test case repeats the fragment's value at run time, so the file shows in the suite's count.

`WireVersionKnobTest.cpp` pins the three codec versions on their own.
