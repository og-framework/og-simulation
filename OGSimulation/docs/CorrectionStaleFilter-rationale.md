<!-- SPDX-License-Identifier: MPL-2.0 -->
# `CorrectionStaleFilter.h` — rationale

The narrative behind `CorrectionStaleFilter` and `NewestTickFilter`: the receive-side rule that drops a
correction older than, or equal to, the newest one already accepted for the same character. The header
keeps its licence, its code and one `⛔G-nn` tag; everything that explains it lives here.

**If this file and `CorrectionStaleFilter.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in
`CorrectionStaleFilter-guards.md`, reachable from the `⛔G-nn` tag at that site.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free and
socket-free (STL only: `<array>`, `<optional>`, `<limits>` and the fixed-width integers). Everything
below is true to a reader with no game engine, no transport and no other repository.

---

## §1 What it is for

A transport that delivers corrections unreliably, without per-character sequencing, can hand the
receiver a correction for a character after a newer one for the same character: two datagrams
reordered on the path, or a re-delivered copy. A replication system that keeps only a property's latest
value never shows the receiver such an older value. This filter gives an unsequenced transport the same
property at the consumer: **per character, keep the tick of the newest correction accepted, and drop
every correction whose tick is at or below it.**

The rule's inputs are a key and a tick. For a correction the caller passes the correction's own tick,
the `u32` the codec writes at `kTickOffset` (`CorrectionStateBufferCodec.h`). The filter reads nothing else; in particular it does
not consult the correction cache (guard G-01).

## §2 The rule, and its three verdicts

`staleTick::admit(newest, tick)` is the whole rule. Both classes call it.

| state | tick | verdict | effect |
|---|---|---|---|
| nothing accepted yet | any, `0` included | `StaleTickVerdict::Admitted` | `newest` becomes `tick` |
| `newest` = N | above N | `StaleTickVerdict::Admitted` | `newest` becomes `tick` |
| `newest` = N | equal to N | `StaleTickVerdict::Duplicate` | dropped, counted in `duplicateDropped` |
| `newest` = N | below N | `StaleTickVerdict::Stale` | dropped, counted in `staleDropped` |

- **"Nothing accepted yet" is `std::nullopt`, not a sentinel tick.** Tick 0 is an ordinary tick and is
  admitted as the first one.
- **Equal is dropped.** The rule is "at or below", not "below". A correction for a tick already accepted
  is not newer than what the receiver has. `CorrectionStaleFilter.DuplicateAndOlderTicksAreRefusedAndCountedApart`
  pins it; §8 records that it fails when the comparison is changed to `<`.
- **The two drops are counted apart** because they are different events: an older tick is reordering on
  the path, an equal tick is the same tick seen twice. A window line that reports both can tell which one
  a link is doing.
- **Admission only moves `newest` forward.** A dropped correction leaves the state unchanged, so a run
  of drops cannot lower the bar. A reorder trace 10, 12, 11, 13 admits 10, 12 and 13
  (`CorrectionStaleFilter.ReorderTraceAdmitsTenTwelveThirteen`).
- `admit` returns `bool` (admitted or not); `DropCounts::count` maps the verdict to that `bool` and bumps
  the matching counter.

## §3 `CorrectionStaleFilter`: one entry per sim id

- **Storage:** `std::array<std::optional<std::uint32_t>, kSimIdCount>`, indexed by the sim id. The id is a
  `std::uint8_t` and `kSimIdCount` is defined as that type's range (256), so every id the signature
  accepts indexes the array: no bounds check exists because none can fail. The array is a fixed
  member; nothing allocates.
- **Id 0 is an ordinary index here.** Whether id 0 can reach the filter is the decoder's concern; the
  filter treats it like any other id (`CorrectionStaleFilter.SimIdsAreIndependent` admits on 0 and 255).
- **Keys are independent.** A drop for one id never touches another's entry.
- **The counters are per filter, not per id.** `staleDropped()` and `duplicateDropped()` total every
  key's drops.
- **`forget(simId)`** clears one id's entry, so the next correction for that id is admitted whatever its
  tick. It is meant to be called when the roster removes the character, so that the filter holds an entry
  only for characters the receiver knows, and a later character given the same id starts from nothing
  (`CorrectionStaleFilter.ForgetReArms`). It does not touch the counters.

## §4 `NewestTickFilter`: the same rule for one key

A stream with a single key, such as a periodic clock publish whose newest tick wins, needs the same rule
without the array. `NewestTickFilter` holds one `std::optional<std::uint32_t>` and the same two counters,
and decides through the same `staleTick::admit`, so the two can never drift apart
(`CorrectionStaleFilter.NewestTickFilterKeepsTheSameRuleForOneKey`).

## §5 Threading: one thread, no atomics

Both classes are plain data, used from one thread: the thread that drains received messages. They hold
no atomic, take no lock, and check no thread id. The draining thread may differ from one session phase
to another (before a session binds, while it steps, at its teardown); what the classes require is
that no two calls overlap, which a single draining owner gives by construction. A caller that drains from
two threads at once has a defect in the caller, not one this class could detect.

## §6 Why not the correction cache's own bit

The correction cache already records, per slot, whether a correction landed there
(`m_containsCorrectTick`). It is the wrong source for this decision, for the two reasons guard G-01 gives:
it is per slot of a 60-tick ring, cleared as slots recycle, so it cannot say "a newer correction was
already accepted"; and it is written from two threads, so a third reader on the drain's thread would be a
new crossing. The filter keeps its own state, on the drain's thread, and the cache is never asked.

## §7 The tick domain

Ticks are compared as raw `std::uint32_t`, with no wrap-around handling. At 60 ticks per second a
`u32` tick counter wraps after about 828 days of one session. After a wrap every new tick is below the
stored one, so a session that long would drop every later correction for a key until `forget` or a new
filter. No handling is added for it.

Every member is `constexpr`, so the filter can be exercised in a constant expression; the tests use it
at run time.

## §8 Tests, and the check seen failing

`Network/CorrectionStaleFilterTest.cpp` in og-simulation-tests, tagged `[Network][CorrectionStaleFilter]`:

| case | pins |
|---|---|
| `CorrectionStaleFilter.InOrderTicksAreAdmitted` | 0, 1, 2, 5, 6, 100 all admitted; no drop counted |
| `CorrectionStaleFilter.DuplicateAndOlderTicksAreRefusedAndCountedApart` | equal → `duplicateDropped`, older → `staleDropped`, the newest unchanged, the next newer tick admitted |
| `CorrectionStaleFilter.SimIdsAreIndependent` | ids 0, 1, 2 and 255 each keep their own newest; id 254, never used, has none |
| `CorrectionStaleFilter.ForgetReArms` | after `forget`, an older tick is admitted for that id only; counters kept |
| `CorrectionStaleFilter.ReorderTraceAdmitsTenTwelveThirteen` | 10, 12, 11, 13 → 10, 12, 13; one stale drop |
| `CorrectionStaleFilter.NewestTickFilterKeepsTheSameRuleForOneKey` | the one-key class: reorder, duplicate, `forget` |

**Seen failing.** With the comparison in `staleTick::admit` changed from `tick <= *newest` to
`tick < *newest`, the duplicate case fails (`duplicateDropped() == 1u` expands to `0 == 1`), and so do
the independence, `forget` and one-key cases, which each include a duplicate; the in-order and reorder
cases, which have none, pass.
