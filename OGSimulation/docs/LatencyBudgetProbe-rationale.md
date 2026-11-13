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
`ClientReceived`, `StepEnd`, `Rendered`, `Healed` and `NetReceived`; it does not know where a host stamps them. So the same
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
| `H6L` | Input | ClientReceived → Healed | a client received a relayed capture → the first step that served it at its scheduled tick, prediction or resimulation, or the first correction that already contains it (§10) |
| `N4` | Input | NetReceived → ServerReceived | the authority's transport received a datagram carrying a capture → the host decoded that capture (§11) |
| `N6` | Input | NetReceived → ClientReceived | a client's transport received a datagram carrying a relayed capture → the host decoded it (§11) |
| `N6c` | State | NetReceived → ClientReceived | a client's transport received a datagram carrying a state for tick T → the host decoded it (§11) |
| `WIRE` | — | direct sample | half the round-trip time, fed by `addSample` |

`Handed` and `LeftProcess` are shared by several hops. That is safe because a probe instance
belongs to one role: on an authority `Handed` in the input stream is the relay hand-off, on a
client it is the input send. Routes the role does not measure are masked out (§4).

`kClientHops` and `kServerHops` are the two role masks. `H7` is in both because a listen host
presents too; on a headless authority it measures the host's per-frame presentation point, not a
render. `H6L` is a client hop only. The net hops are in neither: `kServerNetHops` (`N4`) and
`kClientNetHops` (`N6`, `N6c`) are separate masks a host ORs into its role mask (§11), and a
`static_assert` beside each role mask pins its exact hop set by name.

`H6L` is declared after `H7`, and `N4`, `N6` and `N6c` after `H6L`, each immediately before `WIRE`,
so every hop that existed before them keeps its enumerator value and its place in a window's output;
only `WIRE` moved, and it is still last. Likewise `Healed` and then `NetReceived` were added as the
last points.

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
| `healedByUse` | `H6L` only | an `H6L` sample was accepted and its `Healed` point was set by a use: a plain `stamp` or `stampHealedIfPending` (§10) |
| `healedByCorrection` | `H6L` only | an `H6L` sample was accepted and its `Healed` point was set by `stampHealedByCorrection` (§10) |

A slot is **finalized** when a newer tick takes its ring position, or when `forgetLane` releases
its lane. Finalizing counts `noStart` / `noEnd` for every unresolved route of an enabled hop and
clears the slot. **Disabled hops are never counted**: on an authority, every relayed input is the
end point of `H2` with no start, and counting that would make `H2`'s `noStart` the number of
relayed inputs.

Reading the counters: `noEnd` on `H7` is the number of states superseded before any presentation
point (more than one step between two presentation points). `noEnd` on `H5` is the number of
ticks whose state was not handed to the transport (a host that rotates which characters it sends
per tick produces these by design). `healedByUse + healedByCorrection` equals `n` on `H6L`: both
count accepted samples only, so a heal counted as `outOfOrder` or `overflow` is in neither. On
every other hop both are zero. A non-zero `outOfOrder` on a routed hop means two stamps of
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

Two more pending operations exist for `H6L` only, and are described with it in §10:
`stampHealedIfPending(lane, captureTick, t)` and `stampHealedByCorrection(lane, correctionTick,
delayTicks, t)`.

## §7 `SpscMailbox` — the one crossing the probe needs

The probe has one owner thread and no internal synchronization. A host that produces stamps on a
second thread posts `Event`s into an `SpscMailbox` and drains it on the owner thread into
`apply(event)`. `Event` is trivially copyable (asserted) and covers five operations: a stamp,
the two pending stamps, a direct sample and `stampHealedIfPending` (§10). `stampHealedByCorrection`
has no event kind: its host caller runs on the owner thread.

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

- **Memory.** `maxLanes × 2 streams × ticksPerLane` slots of about 100 bytes (one timestamp per
  point, eleven points), plus `maxSamplesPerHop` doubles reserved per hop, enabled or not. The
  defaults (8, 64, 4096) come to about 100 KB of slots and 490 KB of sample storage for the fifteen
  hops, allocated once in the constructor. Nothing allocates after construction.
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

---

## §10 `H6L` — the late-heal hop (og-input-latency task 6)

`H6` ends at `Consumed`, which a host stamps when a **prediction** step's scheduled read applied
the capture. A relayed capture that arrived after its scheduled tick is never consumed that way, so
it finalizes as `H6` `noEnd`: `H6` cannot see the late population. `H6L` is a second route from the
same start, `ClientReceived`, to a new point, `Healed`, that the late population can reach. **`H6`
is not redefined** (ruling Q7 = A, 2026-11-11), so archived `H6` numbers stay comparable.

**What sets `Healed`.** The first of two events, for capture `c` of character (lane) `A`:

1. **a use**: a step, prediction or resimulation, served `c` at its scheduled tick (a `Hit` of the
   scheduled read, or a resimulation's reference row when that capture was resident);
2. **a correction**: a correction for `A` at tick `X` arrived with `X ≥ c + dA`, where `dA` is the
   delay the authority applied to `A`'s captures. That correction's state already contains the
   effect of `c`.

**The three ways a host sets it, and why they differ.**

| operation | for | semantics |
|---|---|---|
| `stamp(Input, lane, c, Healed, t)` | a prediction `Hit` | the ordinary stamp, as `Consumed` is stamped beside it: first stamp wins and a repeat counts as a duplicate (G-03). A prediction `Hit` therefore closes both `H6` and `H6L`, at the same time. |
| `stampHealedIfPending(lane, c, t)`, and its `Event` | a resimulation's use | sets `Healed` only on a live slot for exactly tick `c` that has `ClientReceived` at or before `t` and no `Healed` yet. It never creates a slot and never counts a duplicate: a resimulation re-serves captures the prediction already served, on purpose, and those are not duplicate stamps of one event. |
| `stampHealedByCorrection(lane, X, dA, t)` | a correction's receipt | the same pending test, over every live slot of the lane whose tick is `≤ X − dA`. Each slot it heals is marked, so its sample counts in `healedByCorrection`. A correction with `X < dA` heals nothing. |

A heal never starts a hop: an operation that finds no `ClientReceived` sets nothing and adds no
`noStart`. A capture that is received, never served at its tick and never contained by a
correction before its ring slot is reused or its lane is forgotten finalizes as `H6L` `noEnd`.

**Known errors** (the host's own stamp sites are the host's documentation):

- **The first applied heal wins, not the earliest.** A host that posts uses through a mailbox and
  applies corrections directly can apply a correction before an earlier use reaches the probe.
  The sample is then the correction's. It needs the correction to arrive within one mailbox drain
  of the use; a client's lead puts the use several ticks earlier.
- **One `dA` per call.** The operation takes one delay for the whole lane. When the delay changed
  inside the ring's span, captures the authority applied with the old delay are judged with the
  new one.

---

## §11 Net residence hops — `N4`, `N6`, `N6c` (og-input-latency P8-5)

A host whose transport receives datagrams on its own network thread and queues them for the code
that decodes them adds a stage no other hop sees: the time a datagram waits between its receipt and
its decode. `NetReceived` marks the receipt. The three net hops end at the points the host already
stamps when it decodes a payload:

- `N4` (Input, `NetReceived` → `ServerReceived`): an authority's residence for a received capture;
- `N6` (Input, `NetReceived` → `ClientReceived`): a client's residence for a relayed capture;
- `N6c` (State, `NetReceived` → `ClientReceived`): a client's residence for a state.

**Who stamps `NetReceived`.** The code that decodes the payload, where it drains the transport's
receive queue. It stamps `NetReceived` under the same key as the decode stamp (stream, lane, capture
or simulation tick), with the time the transport recorded when the datagram arrived (og-net's
`recvSeconds`), not the time of the drain. The probe sees two stamps of one key, and §3 turns them
into one sample in either arrival order.

**The same clock.** That receive time is one more stamp of this probe, so it must come from the
clock every other stamp uses (§1). A transport that reads a different clock gives these hops an
offset equal to the two clocks' difference, and a negative residence is counted as `outOfOrder`
(§4).

**Why the time is taken on the network thread, but the stamp is made elsewhere.**

- The key is not known on the network thread. The transport never parses the payloads it carries, so
  the stream, the lane and the tick exist only once the payload is decoded. The network thread can
  record *when*, and nothing else; the time travels with the datagram to the drain.
- The probe takes stamps from two places only: its owner thread, which calls it directly, and the
  one producer of its `SpscMailbox` (§7). A network thread posting stamps of its own would be a
  second producer on the mailbox and race on its tail. So the drain stamps directly when it runs on
  the owner thread, and posts an `Event::stamp` when it runs on the thread that already produces
  into the mailbox. Neither case adds a crossing.

There is no per-message departure point: the network thread cannot key one either. Time spent
queued before a send is the transport's own measurement (its queue-delay statistics), not this
probe's.

**The masks stay off.** `kClientHops` and `kServerHops` do not contain the net hops, and a
`static_assert` beside each pins its exact set by name. A host that does not stamp `NetReceived`
therefore prints the same window lines as before. A host enables the hops by ORing `kServerNetHops`
or `kClientNetHops` into its role mask once its drain stamps `NetReceived` (og-input-latency part
P5); a window then lists them after `H6L` and before `WIRE`. While they are disabled no net hop
counts `noStart` or `noEnd` (§4), so a stray `NetReceived` stamp changes no reported line.

**Known error.** The first stamp of a point wins (G-03). A host that stamps both `NetReceived` and the
decode point for every redundant copy of a capture measures the first copy decoded, and counts two
duplicate stamps per later copy instead of one.
