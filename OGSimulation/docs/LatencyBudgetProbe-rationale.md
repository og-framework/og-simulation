<!-- SPDX-License-Identifier: MPL-2.0 -->
# `LatencyBudgetProbe.h` — rationale

The narrative behind `latencyBudget::LatencyBudgetProbe`, the per-hop latency instrument, and its
two helpers `SpscMailbox` and `ScalarWindowStats`. The header keeps its licence, its code and one
`⛔G-nn` tag per guard; everything that explains it lives here.

**If this file and `LatencyBudgetProbe.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in
`LatencyBudgetProbe-guards.md`, reachable from the `⛔G-nn` tag at that site.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction (STL only). It names no engine, no transport and no thread library. Where a host
appears below it is "the host": whatever program owns the clock, the threads and the log.

---

## §1 What it is

A fixed-size instrument that answers one question per hop of a networked simulation's input and
state paths: *how long did a tick spend between point A and point B?* Each completed window
(default 10 s) yields p50 / p95 / p99 / max per hop, plus counters for every stamp it could not
use.

The probe owns no clock. Every stamp carries a `double` of seconds from a monotonic clock the
host supplies, and every stamp of one probe must come from the **same** clock. Durations are only
ever differences of two stamps of one process, so the clock's epoch never matters.

## §2 Hops are abstract, so the probe outlives any one transport or thread model

A hop is a **route**: a stream, a start point and an end point (`kHopRoutes`). The probe knows the
names `InputSampled`, `Captured`, `Handed`, `LeftProcess`, `ServerReceived`, `Consumed`,
`ClientReceived`, `StepEnd` and `Rendered`; it does not know where a host stamps them. So the same
probe measures a build whose physics runs on a worker the engine joins, a build with a dedicated
simulation thread, and a build with its own socket, and the numbers stay comparable hop by hop.

| hop | stream | from → to | meaning |
|---|---|---|---|
| `H1` | Input | InputSampled → Captured | the host's input sample → captured into the sim tick |
| `H2` | Input | Captured → Handed | captured → handed to the transport |
| `H3` | Input | Handed → LeftProcess | an input-stream message handed to the transport → it left the process |
| `H3c` | State | Handed → LeftProcess | a state (correction) message handed to the transport → it left the process |
| `H4` | Input | ServerReceived → Consumed | the authority received a capture → the step that consumed it |
| `H4r` | Input | ServerReceived → Handed | the authority received a capture → its relay handed to the transport |
| `H5` | State | StepEnd → Handed | the authority's step for tick T ended → the state for T handed to the transport |
| `H6` | Input | ClientReceived → Consumed | a client received a relayed capture → the step that consumed it |
| `H6c` | State | ClientReceived → Consumed | a client received a state for tick T → the first step that could use it |
| `H7` | State | StepEnd → Rendered | the newest state → the first presentation point after it |
| `WIRE` | — | direct sample | half the round-trip time, fed by `addSample` |

`Handed` and `LeftProcess` are shared by several hops. That is safe because a probe instance
belongs to one role: on an authority `Handed` in the input stream is the relay hand-off, on a
client it is the input send. Routes the role does not measure are masked out (§4).

`kClientHops` and `kServerHops` are the two role masks. `H7` is in both because a listen host
presents too; on a headless authority it measures the host's per-frame presentation point, not a
render.

## §3 The key, and why stamps may arrive in any order

A stamp is keyed by **(stream, lane, tick)**:

- **stream** separates the two key domains. Input-stream ticks are *capture* ticks (the tick at
  which a peer sampled the input); state-stream ticks are *simulation* ticks. Without the split a
  relayed capture for tick 500 and a correction for tick 500 of the same character would land in
  one slot and join each other's hops.
- **lane** is the host's per-character id. Several characters share every tick number, and on an
  authority every connected peer has its own capture-tick clock.
- **tick** selects a ring slot, `tick % ticksPerLane`, and the slot stores the tick it holds.

Each slot holds one timestamp per point. When a stamp completes a route (both points present, in
either arrival order) the duration `to − from` becomes that hop's sample, once (`resolvedMask`).
**The timestamps decide, not the arrival order:** a host whose end stamp reaches the probe before
the start stamp (because the start crossed a thread through a mailbox) still gets the right
number. This is the property that lets one owner thread assemble stamps produced on two threads.

## §4 What is dropped, and how it is counted

Nothing the probe cannot pair is ever turned into a sample. Every such stamp is counted instead.

| counter | scope | incremented when |
|---|---|---|
| `noStart` | per hop | a slot is finalized with the route's end point but not its start |
| `noEnd` | per hop | a slot is finalized with the route's start point but not its end |
| `outOfOrder` | per hop | a route's end time is earlier than its start time, or a direct sample is negative or not finite |
| `overflow` | per hop | a sample arrives when the window already holds `maxSamplesPerHop` |
| `staleStamps` | probe | a stamp's tick is older than the tick its ring slot holds (guard G-02) |
| `duplicateStamps` | probe | a point is stamped a second time for the same key (guard G-03) |
| `laneOverflow` | probe | a stamp names a new lane while `maxLanes` lanes are in use |

A slot is **finalized** when a newer tick takes its ring position, or when `forgetLane` releases
its lane. Finalizing counts `noStart` / `noEnd` for every unresolved route of an enabled hop and
clears the slot. **Disabled hops are never counted**: on an authority, every relayed input is the
end point of `H2` with no start, and counting that would make `H2`'s `noStart` the number of
relayed inputs.

Reading the counters: `noEnd` on `H7` is the number of states superseded before any presentation
point (more than one step between two presentation points). `noEnd` on `H5` is the number of
ticks whose state was not handed to the transport (a host that rotates which characters it sends
per tick produces these by design). A non-zero `outOfOrder` on a routed hop means two stamps of
one key came from different clocks or from a mis-keyed site, and is a wiring fault.

## §5 Windows and percentiles

`closeWindowIfDue(now, out)` drives the window. Its first call only starts the window. Once
`now − start ≥ windowSeconds` it fills `out` with one `HopSummary` per **enabled** hop, in `Hop`
order (a disabled hop has no entry, so a host can print every entry without filtering), resets
every per-window counter and sample, and starts the next window at `now`. A sample belongs to the
window in which its second stamp landed.

Percentiles are **nearest-rank**: the p-th percentile of `n` sorted samples is the sample at rank
`⌈p·n⌉` (1-based). `nearestRankIndex` computes it in integer per-mille arithmetic, so p95 of 100
samples is exactly the 95th and no floating-point rounding can move a rank. A hop with `n = 0`
reports zeros; read `n` before the percentiles.

## §6 Pending stamps

Two operations stamp a point on every slot that is waiting for it, at one host-supplied time:

- `stampPending(stream, from, to, t)` — every live slot with `from` set at or before `t` and `to`
  unset gets `to = t`. A host calls it at a moment that ends a hop for everything queued before it:
  "everything handed to the transport before this flush has now left".
- `stampNewestPending(stream, from, to, t)` — the same, but only the **newest** such tick of each
  lane. Older pending ticks are left open and are finalized later as `noEnd`. A host uses it for a
  presentation point, which shows the newest state and never the ones it superseded.

The `from ≤ t` test keeps a stamp from ending a hop that had not started by then, which matters
when the `from` stamps reach the probe late through a mailbox.

## §7 `SpscMailbox` — the one crossing the probe needs

The probe has one owner thread and no internal synchronization. A host that produces stamps on a
second thread posts `Event`s into an `SpscMailbox` and drains it on the owner thread into
`apply(event)`. `Event` is trivially copyable (asserted) and covers all four operations: a stamp,
the two pending stamps and a direct sample.

- **One producer thread and one consumer thread.** The mailbox is lock-free and correct only for
  that shape. A second producer races on `m_tail`. A host whose producer work runs on different
  OS threads must still serialize it (one at a time, each handing over to the next).
- **The memory orders.** `tryPush` writes the item, then publishes it with a `release` store of the
  tail (guard G-01); `drain` reads the tail with `acquire`, consumes, then releases the head;
  `tryPush` reads the head with `acquire` before reusing a slot. Each side's release pairs with
  the other side's acquire.
- **Full means drop and count.** A full mailbox never blocks the producer: `tryPush` returns false
  and increments a counter the consumer collects with `takeDroppedCount`. A latency probe that
  could stall the thread it measures would change what it measures.
- **The capacity is a power of two**, so the slot index is a mask of a monotonic 64-bit counter
  that never wraps in practice.
- The items live inline (`std::array`), so a large mailbox belongs on the heap.

## §8 `ScalarWindowStats`

A min / mean / max / count accumulator for one scalar sampled per host frame, reset per window.
It ignores non-finite values. It exists so a host can report a per-window summary of a value that
is not a latency (a time-scale factor, for example) next to the probe's lines without a second
windowing scheme.

## §9 Sizes and limits

- **Memory.** `maxLanes × 2 streams × ticksPerLane` slots of about 90 bytes, plus
  `maxSamplesPerHop` doubles reserved per hop. The defaults (8, 64, 4096) come to about 90 KB of
  slots and 360 KB of sample storage, allocated once in the constructor. Nothing allocates after
  construction.
- **The longest measurable hop** is bounded by `ticksPerLane` ticks of the key domain: a hop whose
  end lands more than `ticksPerLane` ticks after its start finds its slot reused and is counted as
  stale. At 60 ticks per second the default is about one second.
- **A clock that jumps backwards** (a hard resynchronisation of a predicting client) produces
  stale stamps until the new ticks pass the old ones in the ring, at most one ring's worth. The
  count says so; no sample is mis-attributed.
- **Tick wrap** at 2³² ticks is not handled (over two years at 60 Hz).
- `maxSamplesPerHop` bounds one window. With 8 lanes at 60 ticks per second a 10 s window holds
  up to 4,800 samples per hop, so the default can overflow on a full authority; `overflow` says
  when, and the percentiles are then over the first `maxSamplesPerHop` samples of the window.
