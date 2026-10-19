<!-- SPDX-License-Identifier: MPL-2.0 -->
# `RenderInterpolation.h` — guards

Every prohibition that governs a line of `RenderInterpolation.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `RenderInterpolation.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `RenderInterpolation-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

---

## G-01 — The walk releases the newer snapshot as soon as it steps to an older one. Do not drop or defer that release.

**Tag site:** `RenderInterpolation.h`, in `interpolateRenderPoses`, inside the bracket walk's loop,
immediately above the `channel.release(next)` that runs when the peeked older snapshot is still at or
after the render time.

**The prohibition.** Do not delete this release, and do not replace it with "release everything the
walk peeked once it ends". Each step of the walk must give back the snapshot it has just stepped past,
so that the walk never holds more than two snapshots: the current *next* and the older one it is
looking at.

**The consequence.** The function holds snapshots of a pooled channel whose writer recycles only
snapshots the reader does not hold. A `SnapshotChannel<T, N>` never drops a step and never recycles
its newest snapshot only while the reader holds at most `kMaxHeldKeepingNewest` = N − 2 of them, which
is why the function asserts that the channel allows two. Without this release a walk that reaches back
two or three steps (a render time far behind the newest, or before the oldest) holds three or four
snapshots of a four-slot channel: holding three, the writer may recycle the newest snapshot while it
fills the next one; holding four, it finds every slot held and drops the step it was about to
publish. A walk that never reaches back past one step looks correct, so the defect shows only on the
long walks. `RenderInterpolation.Slots.AtMostTwoHeldAndAllReleasedOnEveryPath`
counts the held snapshots on every path and fails without the release.

**What breaks if it moves.** The release must stay after the peek of the older snapshot, never before
it: while the reader holds a snapshot the channel indexes `back` from that held snapshot's place, so a
writer's commit during the walk does not shift the indices; with nothing held, the next peek is
indexed from the live newest and may return a newer snapshot than the one just released.
