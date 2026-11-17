<!-- SPDX-License-Identifier: MPL-2.0 -->
# `CorrectionStaleFilter.h` — guards

Every prohibition that governs a line of `CorrectionStaleFilter.h`. Each entry has an **opaque, stable
id**. In the header a single line `// ⛔G-nn  docs/CorrectionStaleFilter-guards.md` sits exactly where
the forbidden edit would be typed.

**If this file and `CorrectionStaleFilter.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to §R and its number is spent forever. A retired id may be **named** in prose; it may never again
appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** The narrative lives in `CorrectionStaleFilter-rationale.md`.
A guard is a prohibition plus the consequence of ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine, no other repository and no other file open.

---

## G-01 — a tick is stale by the filter's own newest tick, never by the correction cache's `m_containsCorrectTick`

**Tag site:** `CorrectionStaleFilter.h`, in `staleTick::admit`, immediately above
`if (newest.has_value() && tick <= *newest)`.

**The prohibition.** Do not replace this test, or add to it, a read of `StateCorrectionCache`'s
`m_containsCorrectTick`, whether directly or through `StateCorrectionCache::containsCorrectTick`, and do
not make the verdict depend on any other state of the correction cache. A tick is stale when it is at
or below `newest`, the tick of the newest correction this filter admitted for that key, and for no
other reason.

**Consequence.**
- **The bit answers a different question.** It says whether an authoritative correction has landed in
  one slot of the cache's ring of `StateBufferSize` (60) ticks. It is cleared whenever a slot is
  recycled for a new tick (`pushPredictionTick`, and `save_snapshot` when it takes a fresh slot) and
  by `wipeCache`; a tick that has left the ring has no slot at all (`getCacheIndex` returns
  `InvalidCacheIndex`). So a correction older than the newest admitted one can find no slot, or a
  slot whose bit is clear, and would be let through: an older state injected after a newer one, the
  one outcome this filter exists to prevent.
- **It is a two-writer word.** `tryInsertingCorrectState` sets the bit on the receive path (the game
  thread in today's engine host), and `pushPredictionTick` clears it on the prediction step; both are
  read-modify-writes of one `std::bitset` word. A read from the drain's thread adds a third, unsynchronized
  party to that word, and the decision would depend on which write it happened to see.

**What breaks if it moves.** Both classes in the header, `CorrectionStaleFilter` and
`NewestTickFilter`, decide through this one function. This comparison is the whole rule for both: an
edit here changes every caller of either.

---

## §R — retired ids

None.
