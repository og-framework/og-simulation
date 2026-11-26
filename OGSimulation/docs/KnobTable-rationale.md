<!-- SPDX-License-Identifier: MPL-2.0 -->
<!-- lint-external-ref: kRow -- the scratch row's name in og-session-runtime task 35's compile probe (section 4), quoted from the compiler's message; not a name in this tree -->
<!-- lint-external-ref: runtimeClamp -- the scratch probe's function-pointer variable (section 4); not a name in this tree -->
# `KnobTable.h` — rationale

`Config/KnobTable.h` describes a configuration knob as data: one row names the key, the role it is read on, its clamp, how it
is applied and read back, and its proof line. One generic intake reads, clamps, applies and logs every row. The header holds
the licence, a docs pointer and the code; this file carries the why.

**If this file and `KnobTable.h` disagree, the header is authoritative and this file is stale.** Fix this file; do not soften
the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** It names no game engine, no configuration file
format and no session: the section name in a row is the caller's, and the reader is any type with `readInt` and `readDouble`.

---

## 1. A row

A knob is two values (og-session-runtime task 35, design Q10 "config as data"):

| part | fields | why it is separate |
|---|---|---|
| `KnobIntake<Value>` | `section`, `key`, `role` (`Both`, `Authority` or `Prediction`) | it is everything a **read** needs, and it does not name the owner, so code that reads before the owner exists can name it (§3) |
| `KnobRow<Owner, Value>` | `intake`, `tag`, `name`, `proofLevel` (`Warning` unless the row says `Log`), `clamp`, `apply`, `stored` | everything the **apply** needs: the clamp, how the clamped value reaches the owner, and how the stored value is read back for the proof line |

`Value` is `int32_t` (read with `readInt`) or `double` (read with `readDouble`); the `KnobValue` concept admits nothing else. The
clamp is `Value(Value)`, the apply `void(Owner&, Value)`, the read-back `Value(const Owner&)`.

Rows are written with designated initialisers, so every field is named at its site:

```cpp
inline constexpr knobTable::KnobRow<DummyOwner, int32_t> kDummyKnobRow{
	.intake = { .section = "OGNetcode", .key = "DummyKnobTicks" },
	.tag    = "DummyKnob.Config",
	.name   = "dummyKnobTicks",
	.clamp  = &clampDummyKnobTicks,
	.apply  = [](DummyOwner& owner, int32_t ticks) { owner.dummyKnobTicks = ticks; },
	.stored = [](const DummyOwner& owner) { return owner.dummyKnobTicks; },
};
```

A **table** is a `std::tuple` of rows with one owner type; `applyKnobs(table, config, owner, world, log)` applies its rows in
order. The rows may mix value kinds.

## 2. What the intake does with a row, and the lines it writes

For each row whose role runs on the caller's world (`KnobWorld::Authority` or `KnobWorld::Prediction`; a `Both` row runs on
either), in table order:

1. **Read** the key once (`readKnob`). An absent key, or a value the reader cannot parse, is no value.
2. **Validate.** A `double` that is not finite is reported and not applied:
   `[<tag>] ini [<section>] <key> is not a finite number, keeping the compiled default`.
3. **Clamp.** A value the clamp moves is reported, then the clamped value is used:
   `[<tag>] ini [<section>] <key>=<value> out of range, clamped to <clamped>`.
4. **Apply** the clamped value to the owner.
5. **Prove**, unconditionally: `[<tag>] <name>=<stored> (<ini|compiled>)`, with the value **read back** from the owner through
   `stored`, never the request. The token is `ini` when step 4 ran and `compiled` otherwise.

Steps 2 and 3 are always at `Warning`; step 5 is at the row's `proofLevel`. The log callable receives the level and the line
separately (`void(KnobLevel, const char*)`), so the caller decides how a level is carried; og-sim-session's sink writes a
`Warning` line with its `[Warning]` token (`SessionKnobs-rationale.md` §6).

An `int32_t` prints as `%d`, a `double` as `%.4f`, the precision of the dilation doors' clamp lines. Each line is formatted with
`snprintf` into a buffer of the measured length, so a long value prints whole.

**A row of the other role is neither read nor applied, and prints nothing.** That is the rule the hand-written session intake
follows for its authority-only keys: their proof lines are authority lines.

## 3. Two halves: `readKnob` and `applyKnob`

`applyKnobs` is `readKnob` followed by `applyKnob`, row by row. The halves are public because a caller can need the read and the
apply at different times. og-sim-session's `readSessionKnobs` reads every key once, on the frame thread, before the manager
exists; the authority's manager is built and configured later, on its runner. So the session reads `RecentreSettleSamples`
through the row's intake in `readSessionKnobs`, keeps the reading in its `SessionKnobs` value, and passes it to `applyKnob`
inside `applyLatencyKnobs`, at the position the hand-written block had (`SessionKnobs-rationale.md` §2.5). The tests check that
the two halves give the same lines, reads and values as `applyKnobs` (§7).

## 4. A row that is incomplete does not compile

The text fields are `KnobText` and the three functions are `KnobFn`. Neither has a default constructor, `KnobFn` deletes its
`nullptr` constructor, and both constructors are `consteval`, rejecting an empty or null text and a null function by calling a
function that is not `constexpr`. So a row that leaves out its clamp, its apply or its read-back, sets one to `nullptr`, names an
empty key, or takes a function pointer that is not a constant, fails to compile. Measured on a scratch row of each kind,
clang-cl 19.1.5 and MSVC 2022 (og-session-runtime task 35):

| row | clang-cl | MSVC |
|---|---|---|
| no `.clamp` | `no matching constructor for initialization of 'KnobFn<int (int)>'` | `C2512: no appropriate default constructor available` |
| `.clamp = nullptr` | `invokes a deleted function` | `C2131: expression did not evaluate to a constant` |
| `.key = ""` | `constexpr variable 'kRow' must be initialized by a constant expression` (in `KnobText`) | `C2131` |
| `.clamp = runtimeClamp` (a variable) | the same as `.key = ""` | `C2131` |
| the complete row | compiles | compiles |

Two `static_assert`s beside the types pin the two properties the table rests on (no default; no `nullptr` function).

## 5. What a row cannot print

A row's proof line names **one** value. A key whose proof line carries several values (one line for two keys, or a value and a
second source), whose value is a word (`Off|Shadow|On`), or whose line has its own wording does not fit, and the table is not
bent to print it: the line would change. og-session-runtime task 35 moved one of the session's fourteen `[OGNetcode]` keys,
`RecentreSettleSamples`, whose two lines are the table's lines byte for byte, and kept the other thirteen as they were; its
notes and `SessionKnobs-rationale.md` §2 list each with its reason.

## 6. Worked example: adding a knob

A knob on an owner that a caller applies with `applyKnobs` is **one row**, and, when the value to run is not the compiled
default, **one line in the configuration file**. No product owner is applied that way today; only tests call `applyKnobs`. For
what a session knob costs, see the second bullet below. The test
`SessionKnobRows.ADummyKeyIsReadThroughMapConfigSourceByItsRowAlone` (og-sim-session) is this example run, with no other file
edited:

1. **The row**, in the table the owner is applied with: the row in §1, added to
   `inline constexpr std::tuple kTableWithTheDummyRow{ kOtherKnobRow, kDummyKnobRow };`. Its clamp is a `constexpr` function
   (`[0, 8]` here); put it beside the field it guards when the field's own setter clamps too, as `TimeConfig.h`'s clamps are.
2. **The ini line**, only to run a value other than the compiled one: `DummyKnobTicks=12` under `[OGNetcode]`.
3. **What the log then shows** (the session's sink, so each line carries its level token):
   ```
   [Warning][DummyKnob.Config] ini [OGNetcode] DummyKnobTicks=12 out of range, clamped to 8
   [Warning][DummyKnob.Config] dummyKnobTicks=8 (ini)
   ```
   Without the ini line, the second line alone: `[Warning][DummyKnob.Config] dummyKnobTicks=3 (compiled)`.

What the row does **not** cover:
- **The field and its setter** are the knob's subject, not its intake: a new `TimeConfig` field still needs its
  `TimeConfigDefaultsTest.cpp` entry and the host's configurability-lint entry, as every field does (`TimeConfig-rationale.md`
  §10).
- **A session knob, today.** og-sim-session's session knobs are read in `readSessionKnobs` and applied by the four `apply*`
  functions, at fixed positions in the start-up lines. Its one row (`RecentreSettleSamples`) is read and applied by the two
  halves (§3), not by `applyKnobs`. A new `[OGNetcode]` session knob costs five things:
  1. a `KnobIntake` constant and a `KnobRow<ManagerT, …>` in `sessionKnobRows`, as `RecentreSettleSamples` has
     (`SessionKnobs.h` :: `kRecentreSettleSamplesIntake`, `kRecentreSettleSamples`);
  2. two `SessionKnobs` fields, `hasIni…` and `configured…` (`SessionKnobs.h` :: `hasIniRecentreSettleSamples`,
     `configuredRecentreSettleSamples`), and their slot in the positional `SessionKnobs{ … }` that `readSessionKnobs` returns;
  3. a `readKnob` call in `readSessionKnobs`. That runs on the **frame thread**: `SessionRuntime::start` reads the knobs before
     it configures the session (`SessionRuntime.h` :: `readSessionKnobs`);
  4. an `applyKnob` call at its place in one of the `apply*` functions; `RecentreSettleSamples`'s is in `applyLatencyKnobs`.
     The `apply*` functions run inside `SessionRuntime::start`'s `configureSession`. On an authority that runs on the
     **runner**, which `runAuthorityLoop` hands it to (`SessionRuntime.h` :: `runAuthorityLoop`); on a client it runs on the
     frame thread;
  5. for a new tag, its route in the engine's log routes (the next bullet).

  A session-wide table that is one row per knob needs its readings carried from the frame thread's read to the owner's
  apply.
- **A new tag in an engine's log routes.** An adapter that routes a line by its tag (the Unreal adapter does) prints a tag it
  has no route for in its default category. Reuse a routed tag, or add the route.

## 7. Tests

- og-simulation-tests `Config/KnobTableTest.cpp` (`[KnobTable]`): a four-row table (three `int32_t` rows, two of them one per role,
  and a `double` row at `Log`) over a scripted reader that records each read: every absent key proves its compiled value in row order and
  reads only its world's keys; in-range values apply as `ini`; out-of-range values on both sides, `INT32_MIN` and `INT32_MAX`
  included, are reported, clamped and applied; a non-finite `double` (`inf`, `-inf`, `nan`) is reported and the compiled value
  kept; a row of the other role is neither read nor applied; an unparsable value is absent; and `readKnob` then `applyKnob` row by
  row gives the same lines, reads and values as `applyKnobs`. `static_assert`s pin the reader concept and the two type
  properties of §4.
- og-sim-session `tests/SessionKnobRowsTest.cpp` (`[Session][SessionKnobs]`): the session's `RecentreSettleSamples` row prints
  the kept `sessionKnobsLines` formats byte for byte on both roles (absent, and ten values from `INT32_MIN` to `INT32_MAX`); the
  dummy key of §6 through `MapConfigSource`; and its negative: with a table that lacks the dummy row, the dummy key is never read,
  by the table or by the session's own `readSessionKnobs`.
