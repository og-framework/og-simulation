<!-- SPDX-License-Identifier: MPL-2.0 -->
# `LatencyBudgetProbe.h` — guards

Every prohibition that governs a line of `LatencyBudgetProbe.h`. Each entry has an **opaque,
stable id**. In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be
typed.

**If this file and `LatencyBudgetProbe.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is
moved to §R and its number is spent forever. A retired id may be **named** in prose; it may never
again appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** The narrative lives in `LatencyBudgetProbe-rationale.md`.
A guard is a prohibition plus the consequence of ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine, no other repository and no other file open.

---

## G-01 — the mailbox publishes an item with a RELEASE store of the tail

**Tag site:** `LatencyBudgetProbe.h`, `SpscMailbox::tryPush`, immediately above
`m_tail.store(tail + 1u, std::memory_order_release);`.

**The prohibition.** Do not weaken this store to `relaxed`, do not move it above the item write
on the line before it, and do not replace `m_tail` with a plain integer.

**Consequence.** This store is what makes the item's bytes visible to the consumer before the index
that publishes them. Relaxed or reordered, the consumer's `drain` can read a slot the producer has
not finished writing: a stamp with a torn time or tick, joined to the wrong hop. The failure is
invisible to a single-threaded test, and the two-thread case in the test suite catches gross loss or
reorder, not a weakened order on a strongly ordered CPU.

**What breaks if it moves.** It pairs with `drain`'s `acquire` load of `m_tail`; the head index
carries the mirror pair for slot reuse (`LatencyBudgetProbe-rationale.md` §7). The orders are only
sufficient for one thread on each side.

---

## G-02 — a stamp for an older tick than its ring slot holds is dropped, never joined

**Tag site:** `LatencyBudgetProbe.h`, `LatencyBudgetProbe::stamp`, immediately above
`if (tick < slot.tick)`.

**The prohibition.** Do not let a stamp whose tick is older than the tick its ring slot currently
holds evict that slot, overwrite one of its points, or join one of its hops. It is counted in
`staleStamps` and dropped.

**Consequence.** Ring slots are indexed by `tick % ticksPerLane`, so a late stamp for tick `t`
lands in the slot now holding `t + k·ticksPerLane`. Joining it there attributes one tick's start
to another tick's end — a mis-attributed sample that looks like any other. Evicting the newer tick
for it throws away live data in favour of dead data. Dropping and counting is the only outcome
that keeps every reported percentile made of correctly paired stamps.

---

## G-03 — the first stamp of a point wins; later ones are duplicates

**Tag site:** `LatencyBudgetProbe.h`, `LatencyBudgetProbe::stamp`, immediately above
`if (slot.hasPoint(pointIndex))`.

**The prohibition.** Do not let a second stamp of the same point for the same (stream, lane, tick)
overwrite the first. It is counted in `duplicateStamps` and dropped.

**Consequence.** Hosts stamp at sites that see the same tick several times on purpose: an input
re-sent in several redundant datagrams, a relayed input carried by several replication rounds, a
tick integrated twice by a clock stall. The hop is defined by the **first** time the tick passed
that point. Last-writer-wins would measure the last redundant copy instead and inflate every
transport hop by the redundancy depth.

---

## §R Retired ids

None.
