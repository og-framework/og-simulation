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
  the pose set moves together.

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
`kRenderCorrectionMinErrorCm` (0.1 cm), or some quaternion component beyond `kRenderCorrectionMinRotationError`
(0.02) of the identity, either sign. Both are the reference engine's thresholds, compared per component as it compares
them. A body in only one snapshot (it joined or left during the replay) carries no error: there is nothing it was
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

`RenderCorrectionSmoother::correctPoses(corrections, frame, params, poses, summary)` drains the ring every frame and keeps
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

On the frame it is due, each body of the correction (present in the pose set) is accumulated into that body's
`RenderCorrectionSmoother::BodyError`, exactly as the reference engine accumulates a replication error:
- the decay window is reset to start at this frame's *next* step: lastStep = next − 1,
  endDecayStep = lastStep + steps, where steps is the configured duration (§5), or 0 for a snap (§6);
- if the window is open (lastStep < endDecayStep), the new error is **added** to whatever error the body still
  carries (errorCm += error, errorRotation = errorRotation × error rotation) and the previous-step error is set to
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
- its size is strictly below `RenderCorrectionParams::maxCorrectionCm` (250 cm, the reference default), or
- `desyncSeconds` is positive and its size is strictly below the distance the body covers in `desyncSeconds` at its
  corrected speed (`RenderCorrectionBody::speedCmPerSecond`; 0.6 s, the reference default). A fast body (a projectile)
  may therefore be corrected by more than the maximum and still be blended.

A snapped body is drawn at the plain interpolated pose, which crosses the correction in one step: the pop. With
correctionSeconds = 0 every correction snaps, which is how a caller turns the smoothing off to compare
(`RenderCorrection.Snap.AtOrAboveTheMaximumOrWithZeroSecondsTheWholeErrorIsDropped`,
`RenderCorrection.Snap.TheDesyncTimeTimesTheSpeedKeepsAFastBodySmoothing`).

## §7 Apply, and the straddle

After the update, every body whose window is still open has its error added to its pose in the pose set:
- position: pose + lerp(fromError, errorCm, alpha);
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

## §8 Per-body lifetime

The smoother keeps one `BodyError` per body key, in a fixed array of `MaxBodies` entries, so a frame allocates
nothing. Before the accumulations, every error whose body is no longer in the pose set is released (the body left);
a correction is not accumulated for a body absent from the pose set. So the errors in use are always a subset of the
pose set's bodies and the array never overflows (an `OG_CHECK` holds it). An error whose window closed is released at
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
| `maxErrorCm` | the largest error accumulated this frame, snapped ones included |
| `maxAppliedCm` | the largest offset added to a pose this frame |

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
- logging and diagnostics; `appliedErrorCm` reads the offset a body received on the current frame, for a trace.

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
