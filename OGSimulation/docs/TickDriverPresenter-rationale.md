<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverPresenter.h` — rationale

The narrative behind `PresentControls`, `TickDriverPresenterConfig`, the `tickDriverPresenter` constants and
helpers, the concepts `TickDriverRenderSource`, `TickDriverPresentClock` and `TickDriverPresentWorldHost`, and
`BasicTickDriverPresenter` (alias `TickDriverPresenter`). The header keeps its licence, its docs pointer, its
code, two guard tags and its `static_assert` and `OG_CHECK` messages; everything that explains it lives here.
Prohibitions are in `TickDriverPresenter-guards.md`.

**If this file and `TickDriverPresenter.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by construction:
STL, glm and og-simulation. Everything below is true to a reader with no game engine and no other
repository.

---

## §1 What it is for

A tick driver steps the simulation at a fixed rate on one thread and the engine draws frames at its own
rate on another. The presenter is the render half between them:
- **on the step's thread**, after every step, it publishes the poses of every body into a snapshot channel
  (J5, a `SnapshotChannel` of four `RenderSnapshotT<N>`), and before every step it captures the correction a
  resimulation made to the poses the render already holds (J7, an `SpscRing` of sixteen `RenderCorrectionT`);
- **on the game thread**, once per frame, it interpolates the poses at a render time a fixed number of steps
  behind the host time, adds the correction smoothing, moves each render target's root through a render sink,
  writes the poses into a world host's shadow world, copies them into the visualization state, and checks and
  reports what it did.

The interpolation is og-simulation's `interpolateRenderPoses` (`RenderInterpolation-rationale.md`) and the
smoothing is `RenderCorrectionSmoother` with its capture `RenderCorrectionCapture`
(`RenderCorrectionSmoothing-rationale.md`). This header adds the order they run in, the per-frame controls,
the target table, the checks and the lines.

## §2 The template and the doors

`BasicTickDriverPresenter`, over `Snapshot`, `EngineT`, `SourceT` and `Allocator`:
- **`Snapshot`** must be a `RenderSnapshotT<N>` (`static_assert`).
- **`EngineT`** models `TickDriverEngine` (`TickDriverConcepts.h`). The presenter uses its `frameNumber()` for
  the `frame=` fields, its `logEnabled`/`log` for the lines and its `kHostDiagnostics` switch (§9). It never
  uses the engine's `nowSeconds()` (guard G-01).
- **`SourceT`** models `TickDriverRenderSource`: the game's step-side reader. It names the session's `Storage`
  and the game's `PhysicsComposite`, gives `kMaxSimulatableSlots` as a constant, and answers `storage()`,
  `savesTicks()` (true on a peer that saves ticks for resimulation, a predicting client),
  `grantedResims()` (the step driver's granted-resimulation count) and `integratedTick()` (the manager's
  current integrated tick). The tick driver's concepts give the presenter's step doors only an outcome, a
  deadline and a step number, so the source is how they reach the storage and the step driver.
- **`Allocator`** (default `std::allocator<char>`) is rebound for every container the presenter owns: the
  target table, the displacement samples and the two line buffers. A test passes a counting allocator to see
  every allocation (§11). `TickDriverPresenter<Snapshot, EngineT, SourceT>` is the default.

The doors:
- `publish_Step` (an outcome, a deadline) and `captureCorrection_Step` (a physics step), on the step's thread: the
  shape of `TickDriverPresenterSteps`, which the header's self-check asserts.
- `present_GameThread` (clock, controls, sink, world host) and `updateVisualization_GameThread` (storage), on the
  game thread, once per frame, in that order.
- `onRegistered` and `onUnregistered` (an id and a slot), on the game thread, for the target table (§11).
- `renderPoses()`, `renderPosesValid()`, `renderFrame()` and `renderCorrectionFrame()` read the last frame's
  results.

The game-thread doors take their collaborators per call, each checked by a concept:
- the **clock** (`TickDriverPresentClock`): `hostNowSeconds()` and `stepIntervalSeconds()`, the tick driver's
  host time and current step interval, and `noteShadowRestore`, the tick driver's window sample of one shadow
  restore (§4);
- the **render sink** (`TickDriverRenderSink`, `TickDriverConcepts.h`): `applyRootPose`, `readAnchor`, `name`;
- the **world host** (`TickDriverPresentWorldHost`): `hasShadowWorld()`, `restore_GameThread` (the frame number
  and a restore note, §4) and `overwriteShadowPoses_GameThread` (§6).

The three new concepts each have a self-check namespace with one conforming shape and at least two
non-conforming ones.

## §3 The step side

**Publish (J5).** `publish_Step` takes a slot (`beginWrite`), fills it from the source's storage with
`fillRenderSnapshot`, and commits it. On a peer that saves ticks it also hands the snapshot to the capture
(`noteShownSnapshot`): the pose the render will draw for that tick. The `OG_CHECK` on the slot holds because
the game thread holds at most two snapshots at once (the pair that brackets the render time, released before
`interpolateRenderPoses` returns), and a four-slot channel keeps a slot free while two are held.

Three `static_assert`s hold the step side's shape:
- `N` equals `kMaxSimulatableSlots` times the composite's declaration count (`declarationCountOf`), because
  the snapshot holds one body per declaration in every slot and `fillRenderSnapshot` drops the excess;
- the snapshot is trivially copyable, so filling a slot in place allocates nothing;
- the delay clamp (`kMaxInterpolationDelaySteps`, 2) is at most the channel's capacity minus two, the
  furthest a four-slot channel brackets (§4).

**Capture (J7).** `captureCorrection_Step` runs before the step and after any replay: the one point where the
storage holds the corrected present tick. It calls `onStepAfterReplay` with `grantedResims()`, the integrated
tick and the step, and counts the outcome: every outcome but `NoNewResim` is a resimulation, `TickMismatch`
adds a tick mismatch, `NoError` a resimulation below the smoothing's thresholds, and `Captured` pushes the
correction into J7, counted as captured when the ring took it. The counters are relaxed atomics: the step's
thread writes them and the game thread's window line reads them. A peer that never resimulates never moves
the granted count, so it captures nothing.

## §4 The game-thread side: order, render time and the bracket

`present_GameThread` runs, in this order:
1. the world host's shadow restore, when it has a shadow world (`restore_GameThread`, with the engine's frame number
   and a note that forwards each restore's duration and refusal to `noteShadowRestore` on the clock);
2. the apply (§5): the held check, the pose build, the sink writes, the applied check, the trace and the
   window lines;
3. the shadow overwrite (§6).

The order is a data dependency. The overwrite writes the poses the apply built into the shadow world, which
the restore has just reset to the newest saved tick; overwritten first, the shadow world would hold the
previous frame's poses (`TickDriverPresenter.Differential.InterpolationAt100FpsAgainst60HzSteps` fails with
the overwrite moved before the apply, on its first frame).

**The restore note.** The world host times a restore and knows whether the shadow world refused the saved
state; it calls the note with the restore's duration in seconds and `refused`, once for every newer saved
state it restores and not on a frame without one. The tick driver's window counts those notes into its
`shadowRestores=`, `shadowRestoreUs=` and `shadowRefused=` fields. The presenter's door is the one place that
holds both the restore call and the clock, so it passes a note that forwards both values unchanged to
`noteShadowRestore`. Both concepts require the pair: a world host whose `restore_GameThread` takes only the
frame number fails `TickDriverPresentWorldHost`, and a clock without `noteShadowRestore` fails
`TickDriverPresentClock` (the self-checks `RestoreWithoutNote` and `MissingRestoreNote`). The differential
compares both models' notes after every frame; with the forwarding dropped, or with `refused` inverted, every
differential case fails on its first frame.

**The render time** (guard G-01) is `clock.hostNowSeconds()` minus the clamped delay times
`clock.stepIntervalSeconds()`. The delay is the controls' `delaySteps`, clamped to [0, 2] by
`clampedDelaySteps` with the engine-style clamp (`clampOf`: a NaN delay becomes 2). Two steps behind is the
default the brawler ships with. `TickDriverPresenter.Differential.DelayZeroTwoAndOutOfRange` runs 0, 2, 3.5
and −1; with the clamp removed the 3.5 and −1 runs fail.

**The bracket** is `interpolateRenderPoses` with the channel, the render time, `newestOnly` and the park
snap distance (the controls' `parkSnap`, the game's distance above which a body's jump between two steps
is a pooled object being parked, not motion). It also copies the bracket's next snapshot into
`m_renderNext`, every frame, which the smoothing and the trace read. It returns false until a first snapshot
is published; the apply then writes nothing.

**The smoothing.** `correctPoses` drains J7 and adds the error terms to the poses, with
the controls' `correctionSeconds`, `maxCorrection`, `desyncSeconds` and the configuration's
`stepSeconds`. It runs right after the bracket because it reads the bracket's frame. Everything that reads
the poses afterwards (the apply, the shadow overwrite, the visualization copy) therefore moves with the
correction.

`m_renderPosesValid` turns true on the first frame that built poses and stays true; the shadow overwrite and
the visualization copy wait for it.

## §5 The apply through the render sink

For every render target, in the target table's order:
- `readAnchor(id)` first. An empty result means the target is gone (its engine object was destroyed while
  the id is still registered); the target is then skipped, exactly as before a write. The anchor read
  before the write is also the `from=` position of the target's `first` line.
- The pose is the root body of the id (`findRenderBody` with `rootDeclarationIndex`); an id with no body in
  the poses is skipped.
- `applyRootPose(id, position, rotation)` writes it. The rotation is the body's when it carries one
  (`hasRotation`), else empty: the sink keeps the target's own rotation.
- The position the sink returns is the position the target holds after the write. The presenter records it
  as the target's applied position: the held check compares with it, the next frame's displacement is
  measured from it, and the trace prints it.

`positionOf` turns whatever the sink returns (an optional or a plain vector, single or double precision)
into an optional double-precision position. A sink that returns double-precision positions keeps the checks
exact (§8); a single-precision sink still works, with the checks at float precision.

## §6 The shadow overwrite (the world host's door)

A world host that keeps a shadow world for game-thread queries (a worker client: its step world is busy on
the step's thread) must hold the bodies where they are drawn. When `hasShadowWorld()` and the poses are
valid, `present_GameThread` calls `overwriteShadowPoses_GameThread` with the poses, a target enumeration
(`targetSlots`) and a visitor:
- the enumeration calls a callable with the id and the slot of every live target (its anchor reads
  non-empty), in the target table's order, with the slot it was registered with. The order is the table's because the first
  out-of-tolerance body of a window is the one its SHADOW Warning names.
- The world host, for every declaration of the slot whose pose exists and whose body is bound, writes the
  pose into the shadow body (the pose's rotation when it carries one, else the body's own) and calls
  the visitor with the id, the declaration index, the pose and a `readBack` callable.
- The visitor, with diagnostics on, calls `readBack()` (the transform the shadow world's reader reports for
  that body), measures the distance and the rotation angle to the pose, accumulates the window's shadow fields
  and emits the first `[SimHost.RenderSync] SHADOW` Warning of a window. With diagnostics off it does nothing
  and the read-back never runs.

The presenter calls the restore before the overwrite in every frame (§4), so a world host may assert that its
restore ran this frame before it writes the poses.

## §7 The visualization copy

`updateVisualization_GameThread` first copies every simulatable's state into its visualization
state (`updateVisualizationAll`). Then, once poses are valid, it writes the pose of every declaration except
the root into the visualization state's body state (and the rotation when the body state carries one), for
every simulatable in the storage. The visualizations of the non-root bodies follow the drawn character; the
root keeps the newest tick, because the render sink already moved the engine's root object to the drawn pose.

## §8 The checks, the windows and the lines

All of this is diagnostics (§9). Every line is formatted with `formatHostLine` into `m_line` and sent to the
engine only when `logEnabled` accepts its level. The format strings are the engine-hosted lines (§13), with
their lengths in metres (§10). Levels: the four checks' lines and LATE are Warnings; the window lines, `first`, the correction
event and the trace are Log.

**The held check** (`checkRenderTargetsHeld`, before the pose build). For every target that was applied and
is live: the anchor against the applied position. Above `kHeldTolerance` (1e-8 m, 1e-6 cm) the first one of a window is a
`[SimHost.RenderApply] MOVED` Warning. That tolerates one rounding of the sink's write (a sink can land a
single-precision pose through a double-precision move from the old position), and anything larger is
something else moving the anchor between two applies (guard G-02).

**The applied check** (`checkRenderApply`, after the apply). For every target: missing when it is gone or has
no root body; else the anchor against the requested pose. Exact matches are counted; the first one outside
`kAppliedTolerance` on any axis of a window is a `MISMATCH` Warning. `kAppliedTolerance` is the no-move
threshold of the engine this presenter was moved from: that engine ignores a move that stays within 1e-4 cm on
every axis (with the rotation unchanged), and its threshold is a single-precision constant, so the presenter
holds `static_cast<double>(1.e-4f)` centimetres, in metres (§10). A blended idle pose that moves less
than that leaves the anchor where it was, and must not count as a mismatch.
`TickDriverPresenter.Differential.ChecksMovedMismatchAndShadow` provokes all three Warnings.

**The distance and comparison helpers** (`distanceOf`, `equalWithin`, `exactlyEqual`, `maxOf`, `minOf`,
`clampOf`, `absOf`, `atan2Of`) are spelled as the engine-hosted math they replace: the distance sums the three
squared differences in x, y, z order before one square root, `maxOf` returns its first argument only when the second is less, and
`atan2Of` returns 0 for two zero arguments. They are not `std::max` or `std::clamp`, which differ on a NaN and on the sign of
a zero.

**The shadow check** (§6): out of tolerance above `kShadowPoseTolerance` (1e-5 m, 1e-3 cm) or
`kShadowPoseToleranceRadians` (1e-4 rad). The world host's physics may hold poses in other units and in
single precision, so a read-back differs from the written pose by that rounding.

**The windows.** Every `kWindowFrames` (600) presented frames the apply window logs one `[SimHost.RenderApply]`
line (frames, frames with poses, characters = targets registered, the two checks' counts and extremes, the
path the applied positions travelled, the applied step range, the channel's drops, the mode, the clamped
delay, the bracket's counts and the per-frame displacement as p50/p99/max by nearest rank); then, with a
shadow world, one `[SimHost.RenderSync]` line; then, on a peer that runs prediction, one
`[SimHost.RenderCorrection]` line (the capture's counters since the last window, the smoothing's sums and
extremes, and the controls). The windows then reset; the correction window keeps the capture counters as
its new start.

**The event lines.** A LATE Warning, at most once per correction window, when a correction reached the game
thread after its carrier step had been drawn. With `traceFrames`, a `[SimHost.RenderCorrection] event` line
per frame that accumulated a correction, and a `[SimHost.RenderTrace]` line per applied target per frame
(the applied position, the next snapshot's root pose, host and render time, the bracket and the correction
offset of the root). The trace prints a target's last applied position even when its engine object is gone;
it reads nothing from the sink. The `first` line is logged once per target registration, on its first apply.

## §9 The diagnostics switch

Every check, counter, window, line and line buffer is inside `if constexpr (kHostDiagnostics)`, where
`kHostDiagnostics` is `EngineT::kHostDiagnostics`. The constants they use are declared unconditionally, so
one build instantiates both settings (the tests run every case with both). With diagnostics off the
presenter interpolates, smooths, applies, overwrites and copies exactly as with them on, and emits nothing;
the shadow read-back, the anchor reads of the checks and the trace are skipped. The anchor read before each
write is not diagnostics: it is the liveness test (§5).

## §10 Units

Every length the presenter takes or passes is an og-sim length, in metres (`SimulationFrame.h`): the poses,
the sink's positions, `maxCorrection`, `parkSnap` and the tolerance constants. No name carries a unit.

**The length constants** are metre literals with the physical size of the centimetre values they shipped with:
the default `maxCorrection` (2.5 m, shipped as 250 cm), `kHeldTolerance` (1e-8 m, 1e-6 cm), `kAppliedTolerance`
(`static_cast<double>(1.e-4f)` cm in metres, 9.999999747378752e-7, so the applied tolerance keeps its
single-precision value widened to double) and `kShadowPoseTolerance` (1e-5 m, 1e-3 cm). Each literal has the bits
the unit flip gave the constant. A `static_assert` beside the constants pins all four bit for bit.
`kShadowPoseToleranceRadians` is an angle.

**The log lines report og-sim state**, so every length prints in metres and in og-sim axes: the printed
components are the poses' and the sink's own. Each length field's name ends in `M`, so a name always matches
its unit, and each length prints with a fixed `%.<n>f`, where n is two more than the decimals the field printed
in centimetres (the same resolution). The positions in parentheses (`from`, `to`, `queried`, `rendered`,
`capsule`, `applied`, `pos`, `nextPos`) and the trace's `err=` offset print their components the same way, with
no suffix. Times, angles, `alpha` and `delaySteps` are not lengths and keep their decimals.

The length fields are: in the `[SimHost.RenderApply]` window `maxError`, `maxHeldDrift`, `path` and the three
`disp` values (one suffix for the `p50/p99/max` field); in the `[SimHost.RenderSync]` window `shadowMaxError`; in
the `[SimHost.RenderCorrection]` window `maxError`, `maxApplied`, `max` and `maxPolicySnap`; `drift` (MOVED),
`error` (MISMATCH and SHADOW) and the event line's `maxError`: 13 fields on 9 format strings.

`TickDriverPresenter.Units.EveryLengthFieldIsNamedInMetres` runs the differential rig with frame tracing on
and requires the length fields it sees to be exactly these 13, each named with `M`, and to carry values.

**History.** While the simulation's unit could still change (og-sim-metres-yup, 2026-10), the suffix printed
through `%s` and a suffix helper of `SimulationFrame.h` (Cm in centimetres, M in metres), each length's
decimals through `%.*f` and a decimals helper (n in centimetres, n + 2 in metres), and each suffixed value was
multiplied by a report-scale control that was always 1. In centimetres every line was byte for byte the line it
was before the routing. When the unit became fixed the helpers were frozen into the format strings (the literal
`M`, `%.<n + 2>f`) and the control was deleted, with the text unchanged in metres.

## §11 Allocation and the target table

Nothing allocates after construction on the step doors, `present_GameThread` or
`updateVisualization_GameThread`:
- the snapshots, the poses, the next copy, the corrections and the smoother are fixed-size members;
- the displacement samples are reserved at construction for `kWindowFrames` frames of
  `kMaxSimulatableSlots` targets (a target holds a slot), and cleared, not freed, at each window;
- the line buffers are reserved at construction (`kLineReserve`, `kDisplacementFieldReserve`) and
  `formatHostLine` grows one only for a longer line.
Every differential case counts the presenter's allocator around every door and requires zero.

**The target table** is a `std::unordered_map` keyed by the `unsigned int` id, filled by `onRegistered` (which
resets an entry that already exists) and erased by `onUnregistered`. Its iteration order sets the order of
the sink writes, of the `first` and trace lines, and which target a window's first MOVED, MISMATCH and SHADOW
Warning names. It is default-constructed, never reserved and never rehashed by the presenter, so its order is
the order a default `std::unordered_map` has after the same inserts and erases.
`TickDriverPresenter.Differential.JoinAndLeave` and the other differential cases fail on their first frame
when the table is reserved at construction: the sink writes and the `first` lines come out in another order.
An insert allocates one node, at registration, outside the frame; that is the one allocation after
construction.

## §12 Threads

The step doors run on the step's thread, inside a step, under the tick driver's world lock and step scope;
the game-thread doors and the target table on the game thread. What crosses: J5 (the channel), J7 (the ring)
and the four capture counters (relaxed atomics). The capture's state, the shown head, is the step's alone;
everything else is the game thread's. The presenter asserts no thread: the step hooks that call its step doors
check that they run inside a step.

## §13 Provenance

The presenter is the render half of the brawler's engine-hosted Jolt backend, moved into og-simulation
unchanged in behaviour, with every log format copied verbatim. In that backend's prose names:
- the snapshot publish and the correction capture became `publish_Step` and `captureCorrection_Step`;
- the shadow restore, the apply and the shadow overwrite, called in that order by its sync function, became
  `present_GameThread`; the restore's note to the frame host's window became the note the presenter forwards
  to the clock's `noteShadowRestore`;
- its pose build became `buildRenderPoses`, its apply `applyRenderSnapshot`, its overwrite
  `overwriteShadowPoses` (with the world half moved to the world host's door), its visualization copy
  `updateVisualization_GameThread`;
- its held check, applied check, trace and two window loggers became `checkRenderTargetsHeld`,
  `checkRenderApply`, `traceRenderFrame`, `logRenderApplyWindow` and `logRenderCorrectionWindow`;
- its six render console variables became `PresentControls`; its park-snap constant, the step time and the
  root declaration index became `parkSnap` and the `TickDriverPresenterConfig` fields; its owner's role query
  became `runsPrediction`; its engine frame counter became the engine's `frameNumber()`; its non-shipping
  preprocessor blocks became `if constexpr (kHostDiagnostics)`.

What changed, and why each change keeps the behaviour:
- the engine character pointer of each target became the render sink's id; a target whose engine object is
  gone is the one whose anchor read is empty;
- the owner-to-slot lookup of the shadow overwrite became the slot given to `onRegistered`: a target is
  registered only while its owner holds a slot, and it is unregistered before the slot is released;
- the publish check's message no longer says the game thread holds one snapshot; it holds two since the
  interpolation (§3);
- the apply window's displacement samples moved out of the window value into a reserved member, so a window
  reset does not construct a new vector;
- the delay clamp, written twice in the old code (in the pose build and in the window line), became one
  `clampedDelaySteps`;
- later, the length constants, the length field names and the length decimals were routed through
  `SimulationFrame.h` (§10), with every constant and every line unchanged in centimetres. After the flip to
  metres, the length constants became metre literals with the same bits, and the names and decimals literals in
  the format strings (§10).

The guards G-01 and G-02 have no predecessor: the old backend explained the render time and the two
tolerances only in its rationale.
