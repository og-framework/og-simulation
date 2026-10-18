<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverThreadContext.h` — guards

Every prohibition that governs a line of `TickDriverThreadContext.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `TickDriverThreadContext.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `TickDriverThreadContext-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

---

## G-01 — `configure` marks the calling thread as the role's owner before it returns. Do not defer or drop the mark.

**Tag site:** `TickDriverThreadContext.h`, in `TickDriverThreadContext::configure`, immediately above the
statement that adds one to the role's count in `OwnerThreadCounts`.

**The prohibition.** Do not delete this statement, and do not move it out of `configure` into a later
call: not into the driver's construction, the first `WorldLockScope`, the first frame or a separate
"mark owner" step. The owner mark must hold from the moment `configure` returns.

**The consequence.** An inline host's step-world predicate (`accessFromStepWorldLockOrInlineOwnerThread`)
accepts the owner thread outside the world lock, and that is the only reason an inline host's world
writes and queries between steps pass. A host calls `configure` at world build and then creates the
adapters that check the predicate; an adapter that asserts the predicate on a call the owner thread makes
outside the lock (a registration between steps, before any driver exists) fails that assertion while
the mark is missing. The worker client's shadow predicate (`shadowAccessOnOwnerThread`) is the owner
mark alone, so it refuses every call until the mark is set.
`TickDriverThreadContext.Configure.PredicateHoldsForTheOwnerThreadBeforeAnyDriver` fails without the
mark in `configure`: with the mark moved into the `WorldLockScope` constructor, every one of its
sections fails.

**What breaks if it moves.** Inside `configure` its place relative to the two predicate assignments
does not matter: they store function pointers and consult no mark. It must stay inside `configure`,
on the thread that calls it, because the mark is thread-local: setting it from any other thread marks
that thread instead.

## G-02 — The destructor lowers the owner count only on the thread that configured the context. Do not drop the thread condition or turn it into a check.

**Tag site:** `TickDriverThreadContext.h`, in `~TickDriverThreadContext`, immediately above the `if` whose
condition guards the decrement of the role's count in `OwnerThreadCounts`.

**The prohibition.** Do not remove `std::this_thread::get_id() == m_configuringThread` from the
condition, and do not replace it with an `OG_CHECK` (or any other assertion) that the destructor runs on
the configuring thread. On any other thread the destructor must do nothing.

**The consequence.** The count is thread-local. Without the thread condition, a context destroyed on a
thread other than the one that configured it subtracts from the destroying thread's count: the owner
thread's count stays raised, and the destroying thread's count, zero until then, wraps to the largest
`uint32_t`, so that thread passes every owner query of the role from then on (the inline step
predicate and the shadow predicate accept it outside the lock).
`TickDriverThreadContext.OwnerMark.DestroyedOnAnotherThreadLeavesBothThreadsAlone` fails without the
condition. With an assertion in its place, a teardown that runs on another thread stops a checked build,
although leaving the count behind is harmless (rationale §3).

**What breaks if it moves.** The condition must stay on the decrement it guards, in the destructor: it
compares the thread that runs the destructor with the thread `configure` recorded, so it means nothing
anywhere else.
