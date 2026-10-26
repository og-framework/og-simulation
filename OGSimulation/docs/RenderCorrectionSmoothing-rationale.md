<!-- SPDX-License-Identifier: MPL-2.0 -->
# `RenderCorrectionSmoothing.h` — rationale

The narrative behind `RenderCorrectionBody`, `RenderCorrectionT`, `RenderCorrectionParams`, `RenderCorrectionFrame`,
`renderCorrectionDurationSteps`, `captureRenderCorrection`, `recordRenderCorrectionSpeeds`,
`RenderCorrectionCaptureOutcome`, `RenderCorrectionCapture` and `RenderCorrectionSmoother`. The header keeps its licence, its docs pointer, its code and its `static_assert` and
`OG_CHECK` messages; everything that explains it lives here. The header has no guard tag, so there is no guards file.

**If this file and `RenderCorrectionSmoothing.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by construction: STL and
glm only, no logging. Everything below is true to a reader with no game engine and no other repository. Where it
names a game engine, it names the behaviour this header reproduces, not a file the reader needs.

---

## §1 What it is for

A predicting client resimulates when the authority's state for a past tick disagrees with its own: it restores that
tick, replays the ticks after it, and continues from the corrected present. The render path of `RenderInterpolation.h`
draws a little in the past, between two published steps. The steps it has already drawn, or is about to draw, came
from the old timeline; the steps published after the resimulation come from the corrected one. The blend between the
last old step and the first corrected step therefore crosses the whole correction in one step: a body jumps by the
correction's size in about 17 ms. On remote characters, which are corrected most often, that reads as a pop.

The usual cure, and what a physics engine with rewind does, is to keep drawing the body where it was and blend the
difference out over a short time. This header is that cure, on top of the pose set `interpolateRenderPoses` builds:
- **capture** (§2): when a resimulation lands, the difference per body between the pose the client had published for
  the present tick and the corrected pose of the same tick, as a `RenderCorrectionT`;
- **carry** (§3): the correction rides the first published step of the corrected timeline;
- **decay** (§4, §5): `RenderCorrectionSmoother` accumulates each correction when the render reaches that step and
  decays it linearly to zero over a configured time, counted in physics steps;
- **snap** (§6): a correction too large to hide is dropped, and the body shows the corrected pose at once;
- **apply** (§7, §8): every frame, the remaining error is added to the pose set, so everything the caller draws from
  the pose set moves together;
- **per-body mode** (§14): a body whose declaration says `SnapInRootFrame` carries no error of its own and is drawn in
  its root's smoothed frame.

The reference is the linear error decay of Unreal Engine 5.6's Chaos physics (the single-particle proxy's
interpolation error data, its pull from the physics state and its rewind data's pre-resimulation cache). §9 lists
every place this header differs from it.

## §2 Capture: the shown head against the corrected head

`captureRenderCorrection(shown, corrected, physicsStep, correction)` compares two snapshots of the **same tick**:
- *shown* is the last snapshot published before the resimulation: the present tick on the old timeline. It is the
  newest pose the client had committed to drawing;
- *corrected* is the same tick after the replay, filled from the same storage by the same `fillRenderSnapshot`, so the
  two differ only by the correction.

Per body present in both (keyed by simulatable id and declaration index), the error is:
- position: shown − corrected, in double precision;
- rotation, only for a body that carries one: the inverse of the corrected rotation times the shown rotation, the
  rotation that takes the corrected pose to the shown one **in the corrected body's own frame**; flipped to the
  hemisphere of the identity (a negative w negates it) and normalized.

So corrected position + error is the shown position and corrected rotation × error rotation is the shown
rotation: adding the whole error to the corrected pose redraws exactly what was shown.

A body is recorded only when its error is not negligible: some position component beyond
`kRenderCorrectionMinError` (0.001 m, the reference engine's 0.1 cm), or some quaternion component beyond
`kRenderCorrectionMinRotationError` (0.02) of the identity, either sign. Both are the reference engine's thresholds,
compared per component as it compares them. The position threshold is a length in metres; the rotation threshold is a
quaternion component. A `static_assert` after the struct definitions pins the position threshold and the default
`maxCorrection` (§6) bit for bit. A body in only one snapshot (it joined or left during the replay) carries no error: there is nothing it was
shown at.

`recordRenderCorrectionSpeeds(correction, storage)` then writes each recorded body's speed, the length of its linear
velocity in the corrected storage, for the snap rule (§6). It walks the storage exactly as `fillRenderSnapshot` does,
so a body's key is the same in both.

**Where the capture runs.** The caller captures on the thread that steps the simulation, after the replay and before
the step that follows it: only there do both poses exist. The shown pose is a copy of the last published snapshot the
caller keeps; the corrected pose is in the storage until the next step overwrites it. The reference engine does the
same: it caches each body's pose when the rewind starts and computes the error after the last replayed step, on the
physics thread, then hands it to the game thread with the results. `RenderCorrectionCapture` (§13) is that glue as one
object: it keeps the copy, fills the corrected pose and calls both functions above in this order.

## §3 Carry: the correction rides the first corrected step

`RenderCorrectionT::physicsStep` is the **carrier step**: the physics step of the first snapshot published after the
resimulation, the first one on the corrected timeline. The caller passes it through a lock-free single-producer ring
(`SpscRing` in `Mailbox.h` fits), not through the snapshot channel: the channel keeps only the newest few snapshots and
may skip one the render never needed, but a correction must never be skipped, or that correction pops.

`RenderCorrectionSmoother::correctPoses(corrections, frame, params, poses, summary, next)` drains the ring every frame and keeps
each correction **pending** until the render's bracket reaches it: it is accumulated on the first frame whose *next*
step (`RenderInterpolationFrame::nextStep`) is at or after its carrier step. That is the frame on which the bracket
first blends towards corrected data.

**The ring is drained inside `correctPoses`, after the bracket is chosen.** The frame is an input, so the caller must
call `interpolateRenderPoses` first. With that order a correction is always visible in time: the stepping thread pushes
it before it publishes the carrier step, so a reader that sees the carrier step in the channel also sees the
correction in the ring. Drained before the bracket instead, a carrier step published in between would be drawn
without its correction. The frame summary counts that case as `late` (a correction whose carrier step is at or before
the *next* step of an earlier frame); it is accumulated anyway, one frame late.

The pending list holds `MaxPending` corrections (8 by default). The bracket lags the newest step by the render delay,
a few steps, and at most one correction is published per step, so a few are ever pending; a correction that finds the
list full is dropped and counted as `pendingDrops`.

## §4 Accumulate

On the frame it is due, each body of the correction (present in the pose set, and not in `SnapInRootFrame`, §14) is
accumulated into that body's `RenderCorrectionSmoother::BodyError`, exactly as the reference engine accumulates a
replication error:
- the decay window is reset to start at this frame's *next* step: lastStep = next − 1,
  endDecayStep = lastStep + steps, where steps is the configured duration (§5), or 0 for a snap (§6);
- if the window is open (lastStep < endDecayStep), the new error is **added** to whatever error the body still
  carries (errorPosition += error, errorRotation = errorRotation × error rotation) and the previous-step error is set to
  the same value; otherwise the body's error is reset to zero.

A second correction during a decay therefore adds to the remainder and restarts the full window from the sum
(`RenderCorrection.Accumulate.ASecondCorrectionAddsToTheRemainingErrorAndRestartsTheWindow`).

## §5 Decay

`renderCorrectionDurationSteps(correctionSeconds, stepSeconds)` converts the time to whole physics steps as the reference engine
does: the floor of the seconds over the fixed step, both widened from single to double precision first. With 0.3 s
and the project's 0.016667 s step that is **17** (the float 0.3 widens to 0.30000001, the float 0.016667 to 0.0166670,
and their ratio is 17.9996); with an exact 1/60 s step it is 17 as well. Zero seconds give zero steps, which is a snap
(§6).

Every frame, after the accumulations, every body error is **updated** to the frame's *next* step: the steps since the
last update are counted, and for each of them (at most as many as are left in the window) the error is scaled by
(count − 1) / count, where count is the number of steps left in the window at the start of the update, computed in
single precision as the reference engine computes it. The rotation error is slerped towards the identity by the same
factor. The value before the scaling is kept as the previous-step error.

So an error accumulated at step s with a window of N steps is (N − 2)/(N − 1) of itself at s, (N − 3)/(N − 1)
at s + 1, and zero at s + N − 2; at s + N − 1 the window has closed and the body's error is released. With
N = 17: 15/16 at the carrier step, one sixteenth less per step, zero 15 steps later, released after 16 steps, about
0.27 s (`RenderCorrection.Decay.LinearToZeroOverTheDurationMinusOneSteps`). A *next* step that does not advance decays
nothing; one that goes back (a render clock that moved backwards) decays nothing and moves the reference step back.

When the bracket's *next* step jumps several steps in one frame, the loop scales by the same factor several times,
because the reference engine computes count once per update. That is reproduced, not corrected: the jump is rare (a
hitch), and the error still reaches zero at the window's end.

## §6 Snap

A body's correction is not blended, and the body's whole error (including what remains of earlier corrections) is
dropped, unless **either**:
- its size is strictly below `RenderCorrectionParams::maxCorrection` (2.5 m, the reference default of 250 cm; §2),
  or
- `desyncSeconds` is positive and its size is strictly below the distance the body covers in `desyncSeconds` at its
  corrected speed (`RenderCorrectionBody::speed`; 0.6 s, the reference default). A fast body (a projectile)
  may therefore be corrected by more than the maximum and still be blended.

A snapped body is drawn at the plain interpolated pose, which crosses the correction in one step: the pop. With
correctionSeconds = 0 every correction snaps, which is how a caller turns the smoothing off to compare
(`RenderCorrection.Snap.AtOrAboveTheMaximumOrWithZeroSecondsTheWholeErrorIsDropped`,
`RenderCorrection.Snap.TheDesyncTimeTimesTheSpeedKeepsAFastBodySmoothing`).

## §7 Apply, and the straddle

After the update, and after the root-frame pass of §14, every body whose window is still open has its error added to
its pose in the pose set:
- position: pose + lerp(fromError, errorPosition, alpha);
- rotation (a body that carries one): pose × slerp(fromRotation, errorRotation, alpha), in the body's local frame
  as captured (§2). A body without a rotation keeps the rotation the pose set gives it.

alpha is the frame's interpolation alpha (`RenderInterpolationFrame::alpha`, narrowed to single precision as the
interpolation narrows it), so the error moves continuously across the bracket as the pose does.

fromError is the previous-step error, **except on the straddle**: the bracket whose *prev* step is before the carrier
step and whose *next* step is at or after it (frame.hasPrev && frame.prevStep < correctionStep). There *prev* is the
last step of the old timeline, which already shows the uncorrected pose, so the blend starts from the error the body
carried **before** this correction was added (zero for a fresh one) and ends at the decayed error of the carrier step.

This is the one mechanical difference from the reference engine, and it exists because of what each one draws. The
reference engine's game thread also receives the replayed steps, so on that frame its *prev* is the corrected pose of
the previous tick and it adds the whole new error there. This render path never sees replayed steps: *prev* is the old
pose, which equals the corrected pose plus the new error. Starting the blend from the pre-correction error therefore
draws exactly what the reference engine draws on that bracket, and the rendered path is continuous at both ends of it:
at alpha = 0 it is the old pose the previous frame ended on, at alpha = 1 the corrected carrier pose plus the
decayed error the next bracket starts from
(`RenderCorrection.Straddle.TheRenderedPathStaysContinuousAcrossTheCorrectedStepAndMatchesChaossCorrectedPrev`, which
also shows the same run with zero seconds jumping by more than 40 % of the correction in one frame).

A `SnapInRootFrame` body (§14) has no error, so none of this applies to it; what it draws on the straddle is §15.

## §8 Per-body lifetime

The smoother keeps one `BodyError` per body key, in a fixed array of `MaxBodies` entries, so a frame allocates
nothing. Before the accumulations, every error whose body is no longer in the pose set is released (the body left);
a correction is not accumulated for a body absent from the pose set. So the errors in use are always a subset of the
pose set's bodies and the array never overflows (an `OG_CHECK` holds it). A body in `SnapInRootFrame` (§14) never
creates one; it may hold a carrier record instead (§15), kept in a second fixed array of `MaxBodies` entries and
released with the errors when its body leaves the pose set, so that array never overflows either (an `OG_CHECK`
holds it too). Its mode is fixed by its declaration, so a key keeps its mode while its simulatable stays in the pose set;
only a simulatable id reused by a different simulatable type, with no frame in between in which the id is absent from
the pose set, would carry an earlier body's remaining error over, as it would for any reused key. An error whose window closed is released at
the update that finds it closed
(`RenderCorrection.Lifetime.AnErrorIsReleasedWhenItsBodyLeavesThePoseSetAndNoneIsCreatedForAnAbsentBody`).

## §9 Differences from the reference engine

| what | reference engine | here | why |
|---|---|---|---|
| where the error rides | the last replayed step's results; the game thread merges it into the next results it moves to | the first published corrected step (§3) | the replayed steps are never published here |
| the straddle bracket | *prev* is the replayed corrected pose, plus the whole new error | *prev* is the old pose, blended from the pre-correction error (§7) | same drawn pose; this side has no replayed pose |
| two errors for one body reaching the game thread together | the merge keeps one of them (a map append overwrites the key) | both are accumulated, in order | an overwrite loses a correction, which pops |
| the speed for the desync rule | the largest of the game-thread body's, the previous result's and the next result's velocity | the corrected head's velocity (§2) | this side has no velocity in its snapshots; the corrected one is the speed the body continues with |
| directional decay, velocity-based error smoothing | present, off by default (multiplier 0, experimental flag off) | not implemented | both default off |
| per-body settings override | an optional per-proxy settings struct | one `RenderCorrectionParams` per frame | the project uses the defaults |
| precision | double positions and quaternions, a single-precision decay factor and alpha | the same | the decay is reproduced bit for bit |
| a body drawn in its parent's smoothed frame (`SnapInRootFrame`, §14) | no counterpart: each proxy smooths its own error | the body drops its own error and takes its root's applied offset | the game asks for an attached body that shows its corrected pose at once and stays on its root |

`RenderCorrection.Decay.EveryStepMatchesChaossLinearDecayBitForBit` drives the smoother and an engine-free
transcription of the reference engine's error type (kept in the test, in the engine's own shape) through 24,000
random frames (corrections, snaps, multi-step jumps, a step going back, three parameter sets) and compares the error,
the previous-step error, both rotations, the applied offset and the resulting pose, bit for bit, on every frame
outside a straddle.

## §10 The frame summary

`RenderCorrectionFrame` is written once per `correctPoses` call:

| field | meaning |
|---|---|
| `corrections` | corrections accumulated this frame |
| `smoothedBodies`, `snappedBodies` | bodies whose error was accumulated and blended, or dropped (§6) |
| `late` | corrections whose carrier step an earlier frame's *next* step had already reached (§3) |
| `pendingDrops` | corrections dropped because the pending list was full (§3) |
| `finished`, `finishedSteps`, `maxFinishedSteps` | windows that closed this frame, and their length in steps from the accumulation |
| `active` | bodies whose error was added to the pose set this frame |
| `maxError` | the largest error accumulated this frame, snapped ones (§6) included, `SnapInRootFrame` bodies (§14) not |
| `maxApplied` | the largest offset added to a pose this frame by the apply loop (§7); the root-frame pass (§14) does not add to it |
| `policySnaps` | correction bodies not accumulated because their pose body is `SnapInRootFrame` (§14) |
| `rootFramed` | `SnapInRootFrame` bodies moved by their root's applied offset this frame (§14) |
| `maxPolicySnap`, `maxPolicySnapDeg` | the largest position error and rotation error angle among the bodies counted in `policySnaps` |
| `straddleSnaps` | `SnapInRootFrame` bodies given *next*'s rotation this frame because the bracket straddles their carrier step (§15) |

## §11 What stays with the caller

- the parameters, read per frame (`RenderCorrectionParams`), and the fixed physics step they are converted with;
- the ring, its size and what to do when it is full (count the drops);
- deciding which published snapshots the capture keeps (`noteShownSnapshot`, only on a peer that predicts), and
  passing `onStepAfterReplay` the resimulation count, the integrated tick and the step (§13); the capture object
  keeps the copy, fills the corrected snapshot, and skips a capture whose two snapshots are of different ticks (for
  example after a hard resynchronization moved the clock);
- counting the capture outcomes, and pushing a `Captured` correction into the ring;
- calling `correctPoses` right after `interpolateRenderPoses`, every frame, and drawing everything from the pose set
  it returns;
- passing `correctPoses` the bracket's *next* snapshot, the copy `interpolateRenderPoses` writes through its
  `nextCopy` argument, on every frame (§15); without it a `SnapInRootFrame` body is blended across the straddle;
- logging and diagnostics; `appliedErrorPosition` reads the offset a body received on the current frame, for a trace. It
  returns 0 for a `SnapInRootFrame` body (§14): that body has no error of its own and is drawn with its root's offset.

## §12 Tests

`RenderCorrectionSmoothingTest.cpp` (tag `[RenderInterpolation]`):
`RenderCorrection.Capture.ErrorIsShownMinusCorrectedWithTheRotationInTheCorrectedBodysFrame`,
`RenderCorrection.Capture.ChaosThresholdsATenthOfACentimetrePerAxisOrTwoHundredthsOfAQuaternionComponent`,
`RenderCorrection.Capture.ABodyInOnlyOneOfTheTwoSnapshotsCarriesNoError`,
`RenderCorrection.Speeds.RecordedFromTheStoragesLinearVelocityForTheCorrectedBodiesOnly`,
`RenderCorrection.Duration.FloorOfTheSecondsOverTheStepConvertedAsChaosDoes`,
`RenderCorrection.Decay.EveryStepMatchesChaossLinearDecayBitForBit`,
`RenderCorrection.Decay.LinearToZeroOverTheDurationMinusOneSteps`,
`RenderCorrection.Snap.AtOrAboveTheMaximumOrWithZeroSecondsTheWholeErrorIsDropped`,
`RenderCorrection.Snap.TheDesyncTimeTimesTheSpeedKeepsAFastBodySmoothing`,
`RenderCorrection.Accumulate.ASecondCorrectionAddsToTheRemainingErrorAndRestartsTheWindow`,
`RenderCorrection.Straddle.TheRenderedPathStaysContinuousAcrossTheCorrectedStepAndMatchesChaossCorrectedPrev`,
`RenderCorrection.Pending.ACorrectionWaitsForTheBracketToReachItsStepAndALateOneIsCounted`,
`RenderCorrection.Pose.AtTheCorrectionTheRenderedPoseIsTheShownOneAndARotationlessBodyKeepsItsRotation`,
`RenderCorrection.Lifetime.AnErrorIsReleasedWhenItsBodyLeavesThePoseSetAndNoneIsCreatedForAnAbsentBody`,
`RenderCorrection.NoAllocation.TheCorrectionTheSmootherAndItsFrameAreFlatValues`.

`RenderCorrectionCaptureTest.cpp` (tag `[RenderInterpolation]`), the capture object of §13:
`RenderCorrection.CaptureObject.NoNewResimWhenTheGrantedCountHasNotMovedAndOutIsUntouched`,
`RenderCorrection.CaptureObject.NoShownHeadConsumesTheResimWithoutTouchingOut`,
`RenderCorrection.CaptureObject.TickMismatchWhenTheIntegratedTickIsNotTheShownSnapshotsTick`,
`RenderCorrection.CaptureObject.NoErrorWhenEveryBodyIsWithinTheThresholds`,
`RenderCorrection.CaptureObject.CapturedIsShownMinusCorrectedAtTheIntegratedTickWithTheCorrectedSpeeds`,
`RenderCorrection.CaptureObject.ResetForgetsTheShownSnapshotAndTheResimCount`,
`RenderCorrection.CaptureObject.NoAllocationTheCaptureIsAFlatValue`, and
`RenderCorrection.CaptureObject.MatchesTheLooseMemberHostLogicOverRandomPublishResimAndTickSequences`. The last one keeps
an independent copy of the host logic this object replaced (the same checks written inline against loose members, with
the counters and the ring) and drives both through 48,000 random steps: publishes skipped or kept, resimulations
granted one or two at a time, bodies joining and leaving, corrections below and above the thresholds, integrated ticks
off by one or more, and a small ring that overflows. It compares the outcome, the counters and every correction that
leaves the ring, bit for bit, and counts each outcome, so a run that stops reaching one fails.

`RenderCorrectionBodyPolicyTest.cpp` (tag `[RenderInterpolation]`), the per-body mode of §14:
- `RenderCorrection.BodyPolicy.ASnappedBodyCarriesNoErrorAndItsRootStillSmooths`: a root with a 2 cm error and a
  `SnapInRootFrame` body with 117° and 2 cm, at the carrier frame with no *prev*: the body has no error, its rotation
  is the pose's, its position is the pose's plus the root's applied offset, the counters read one snap of 117°, and the
  root's error and pose are byte-equal to a run without the body.
- `RenderCorrection.BodyPolicy.TheSnappedBodyKeepsItsOffsetFromItsRootOnEveryFrame`: two simulatables, each with a
  snapped body, a smoothed body and a root, over 5,000 random frames (steps advancing by −1 to 3, straddles, roots
  snapped above the maximum, three parameter sets), with a *next* snapshot whose poses differ from the supplied ones:
  the §14 invariant to 1e-3 cm on every frame, and the exact rotation, which is *next*'s on the frames an
  independent model of §15's carrier records says straddle a carrier step and the supplied one on every other frame;
  `straddleSnaps` equals the model's count every frame. It counts root-framed frames, straddled root frames, frames
  given the carrier rotation and root snaps, so a run that stops reaching one fails.
- `RenderCorrection.BodyPolicy.BodiesWithoutThePolicyAreBitExactWithAndWithoutASnappedNeighbour`: the same random
  frames run with the snapped bodies and without them. Every other body's `BodyError` and pose are byte-equal between
  the two runs and the frame summary's existing fields are equal; all three are equal in value to an independent
  transcription of the smoother as it was before §14. The transcription is compared by value, not by bytes: the compiler may fold the
  shared identity slerp differently in the two copies and give a zero of the other sign. The two smoothers are passed
  their *next* snapshots (§15); the transcription has none.
- `RenderCorrection.BodyPolicy.OnTheStraddleASnappedBodyTakesTheCarrierPose`: a root with a 2 cm error and a
  `SnapInRootFrame` body whose rotation differs between the old step and the carrier step, drawn through the real
  `blendRenderPoses`. On the straddle at alpha 0.3 and 0.8 the body's rotation is *next*'s, byte for byte, and
  `straddleSnaps` is 1; its position is the blended one plus the root's applied offset, so the §14 offset holds to
  1e-3 cm and the body is not at *next*'s position. On the four brackets after it (both steps corrected) the rotation
  is the plain blend, byte for byte, and `straddleSnaps` is 0. Every frame checks that the blend differs from *next*'s
  rotation, so the comparison can fail.

## §13 The capture object: `RenderCorrectionCapture`

`RenderCorrectionCapture` is the stepping thread's half of the smoothing in one object, so a host calls it
instead of rebuilding the glue of §2. It owns three things: a copy of the last snapshot the caller published (the
shown head), a scratch snapshot it fills with the corrected head, and the resimulation count it last saw. All three
are flat members: the object is trivially copyable (a `static_assert` holds it), a call allocates nothing, and it
logs nothing.

**The calls**, all on the thread that steps the simulation:
- `noteShownSnapshot(published)`, after each publish on a peer that predicts (one that keeps ticks to resimulate
  from). It copies the snapshot. An authority never resimulates, so it need not call it.
- `onStepAfterReplay(grantedResims, integratedTick, physicsStep, storage, out)`, once per step, after any replay and
  before the step simulates: the one point where the storage holds the corrected present tick. `grantedResims` is a
  count of granted resimulations that only grows (the step driver's diagnostics count, `grantedResims`);
  `integratedTick` is the tick the storage now holds (`currentIntegratedTick`); `physicsStep` is the step about to
  run, the correction's carrier step (§3).
- `reset()` forgets the shown head and the count, for a session that starts again with its count at zero.
  `hasShownSnapshot` and `resimsSeen` read them back.

**The order**, which returns at the first step that applies:

| outcome | when | what it did | `out` |
|---|---|---|---|
| `NoNewResim` | the count equals the one it last saw | nothing | untouched |
| `NoShownHead` | no snapshot was noted yet | took the new count | untouched |
| `TickMismatch` | the corrected head, filled from the storage at `integratedTick` with `fillRenderSnapshot`, is not of the shown head's tick | took the count, filled the scratch | untouched |
| `NoError` | `captureRenderCorrection` recorded no body (§2's thresholds) | took the count, filled the scratch | step, tick, no body |
| `Captured` | some body moved beyond the thresholds | took the count, captured, then `recordRenderCorrectionSpeeds` | the correction |

Every outcome but the first takes the new count, so a resimulation is captured at most once and one that finds
nothing to compare is not retried a step later, against a newer tick. Several resimulations granted between two calls
are one capture: the replay already folded them into the corrected head. `out` is written only on the last two
outcomes, and only its first `bodyCount` bodies; the bodies after them keep whatever the caller's value held.

**What the caller does with the outcome:** count it (the counts are the window line a host prints), and on `Captured`
push `out` into the ring of §3. The object holds no correction of its own, so the caller's `out` may live on the
stepping thread's stack. Nothing else reads the object: it has no synchronization and needs none, because the snapshot
copy, the scratch and the count are touched only by the thread that steps.

## §14 Per-body correction mode

A declaration may say that its body is not smoothed: `RenderCorrectionMode::SnapInRootFrame`
(`PhysicsDeclaration-rationale.md` §7). `fillRenderSnapshot` copies the mode and the root mark into every
`RenderBody` (`renderCorrection`, `isRoot`; `RenderSnapshot-rationale.md` §5), and the interpolation copies
the bodies of its *next* snapshot into the pose set, so the smoother reads both from the pose set it is
handed. The mode is a compile-time property of the declaration. There is no runtime switch: a body in
`SnapInRootFrame` takes the path below on every frame.

**What it is for.** A body attached to a moving root (a weapon held by a character) is corrected
together with the root. Smoothing its own error blends its rotation across whatever it did between the
old and the corrected timeline, which can show motion that never happened. Dropping its error alone
would draw it at its corrected pose while the root still glides by its own decaying error, so it would
leave the root by that error. Drawing it in its root's smoothed frame keeps both: its corrected rotation
and its corrected place relative to the root, from the first frame that draws the corrected step.

**Accumulate.** For a correction body whose pose body is `SnapInRootFrame`, no `BodyError` is created or
changed: there is no window, and `smoothedBodies` and `snappedBodies` do not count it. It is counted in
`policySnaps`, its position error length is folded into `maxPolicySnap` and its rotation error angle,
2·acos(min(1, |w|)) in degrees, into `maxPolicySnapDeg`. `corrections` still counts the correction it
came in. `maxError` does not include it.

**The root-frame pass.** After the update and before the apply loop of §7, with the same alpha, every
pose body in `SnapInRootFrame` is moved by its root's applied offset:
- the root is the pose body with `isRoot` and the same `simulatableId`, found within that simulatable's
  key range of the sorted pose set;
- if the root has an error in use whose window is open, the body's position becomes
  pose + `appliedErrorPosition` of the root's error, the same function and the same expression the apply loop
  then uses to move the root itself, and `rootFramed` counts the body;
- the rotation is the pose's, untouched.

The offset is read from the root's `BodyError`, not from the root's already moved pose, so the order of
the two loops does not matter.

**The invariant.** On every frame, rendered(body) − rendered(root) = pose(body) − pose(root), up to the
rounding of the two single-precision positions, and rendered(body).rotation = pose(body).rotation exactly,
except on the straddle of the body's own carrier step, where it is *next*'s rotation exactly (§15).
So a snapped body keeps the offset from its root that the interpolation gave it, whether the root is
smoothing, has snapped (§6) or has no error, on a straddle (§7) or not.

**Only a translating root.** The root's correction error is composed as a translation only. A root with a
rotation fails an `OG_CHECK`: composing its frame would also need its rotation error applied to the
body's lever arm, which no root needs today. A body in `SnapInRootFrame` with no root in the pose set
fails an `OG_CHECK` too. A root that declares `SnapInRootFrame` fails one in `fillRenderSnapshot`
(`RenderSnapshot-rationale.md` §5). All three are runtime checks with no unit test.

**Everything else is unchanged.** A body without the mode takes exactly the path of §4–§8, bit for bit,
whether or not a snapped body of the same simulatable is present: the snapped body never takes an error
slot, never feeds `maxError`, and the root-frame pass writes only snapped bodies.
`appliedErrorPosition(frame, simulatableId, declarationIndex)` returns 0 for a snapped body: it has no
`BodyError`, and the offset it is drawn with is its root's.

**The capture is unchanged.** `captureRenderCorrection` and `RenderCorrectionCapture` still record a
snapped body's error, so the step-side capture is the same for every body; only the game-thread smoother
decides, and it counts what it dropped.

## §15 The straddle for a snapped body

A `SnapInRootFrame` body (§14) carries no error, so on every frame it is drawn at the interpolated pose in its root's
frame. The interpolation itself still blends it across the straddle (§7): *prev* is the last step of the old timeline
and *next* the carrier step, so for one step the body's rotation turns from the old pose to the corrected one. When
the correction changed the body's rotation (an attached body that the corrected timeline has already turned), that
blend draws, for a frame or two, a rotation that neither timeline had.

So `correctPoses` takes the bracket's *next* snapshot as its optional last argument, `next` (the copy
`interpolateRenderPoses` writes through `nextCopy`; `RenderInterpolation-rationale.md`), and on the straddle such a
body takes *next*'s rotation unblended:
- **The carrier record.** When §14's accumulate drops a correction body for its mode, it also records the body's key
  and the correction's carrier step (`RenderCorrectionT::physicsStep`, §3) in a `RenderCorrectionSmoother::PolicyCarrier`,
  in a fixed array of `MaxBodies` entries. A later correction for the same body overwrites the step.
- **The pass**, after the update and before the root-frame pass of §14. For each record: when the frame has a *prev*,
  *prev* is before the carrier step and *next* at or after it (prevStep < correctionStep ≤ nextStep), `next` is
  given, and the body carries a rotation, the pose body's rotation becomes *next*'s body's rotation, and
  `straddleSnaps` counts it.
- **Release.** A record is released in that pass on the first frame with no *prev*, or whose *prev* is at or after
  the carrier step: the bracket has left the straddle, and from there the blend is between two corrected steps. It is
  also released, before the accumulations, when its body leaves the pose set (§8). A *next* step that went back
  before the carrier step keeps the record and takes nothing.
- **Not the position.** On the straddle the root is still blending from its old-timeline pose, compensated by its
  pre-correction error (§7). The body's position stays the blended one plus the root's applied offset (§14), so it
  stays where the root's frame puts it. *next*'s position is where the body will be at alpha 1; copied now, it would
  leave the root by up to (1 − alpha) times the root's motion over the step and its correction. Only the rotation,
  the part whose blend shows an orientation neither timeline had, is taken.
- **Without `next`** (a null pointer, the default) nothing is taken and the body is blended as before; the records
  are still kept and released, so a caller may pass it on some frames only.

**What a viewer sees.** No in-between rotation at a correction. The corrected rotation appears up to one step early:
from the first straddle frame (alpha below 1) the body shows the carrier step's rotation, at most one step's worth of
rotation ahead of where the blend would have it. On every other bracket the rotation is the plain blend.

Nothing else changes: a body without the mode is never recorded, so the paths of §4–§8 are untouched, and the step-side
capture records the snapped body's error as before (§14).
