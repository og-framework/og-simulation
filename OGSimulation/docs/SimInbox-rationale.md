<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimInbox.h` — rationale

The narrative behind `SimInbox`, the bounded single-producer, single-consumer inbox that carries inbound
sim traffic from a transport's receive path to the sim's owner. The header keeps its licence, its docs
pointer, its code and one `⛔G-nn` tag per guard; everything that explains it lives here.

**If this file and `SimInbox.h` disagree, the header is authoritative and this file is stale.** Fix this
file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in `SimInbox-guards.md`,
reachable from the `⛔G-nn` tag at that site.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free and socket-free
by construction (the STL, `SimTransport.h` and `OGAssert.h`). Where a transport or a host appears below it
is "the transport" (whatever receives the bytes) and "the host" (whatever program owns the sim and calls
`drain`).

---

## §1 What it is

The inbound half of the transport port. `SimTransport.h` declares how the sim *sends*; `SimInbox` is how
it *receives*. The transport's receive path is the producer: for each sim message it receives it calls
`pushMessage`, and for each connection that opens or closes it calls `pushEvent`. The sim's owner is the
consumer: at its receive point it calls `drain`, which hands every waiting record to a callback, oldest
first.

The inbox decodes nothing. A record carries the lane it arrived on, the connection it came from, the
time it was received and the message bytes, copied; what the bytes mean is the consumer's business.

`DefaultSimInbox` is `SimInbox<2048>`. Constructed with no `SimInboxConfig`, an inbox is one ring of `N`
slots shared by every lane and by the connection events, exactly as the header first shipped. A
`SimInboxConfig` adds three things a transport with several connections needs (§12, §13, §14):
- **a Control ring** (`controlSlots`): Control messages get a ring of their own, so a flood on the other
  lanes can never take their slots;
- **an event ring** (`eventSlots`): connection events get a ring of their own, and a producer that pairs each
  connection's `Connected` with its `Disconnected` can never have a `Disconnected` refused;
- **tags and their quotas** (`tags`, `tagQuota`, `tagControlQuota`): a producer may tag each message with
  the source it came from. A tagged message on any lane but Control is refused while its tag already holds
  `tagQuota` undrained records in the shared ring, and a tagged Control message while its tag holds
  `tagControlQuota` in the Control ring.

## §2 The record

`InboundSimRecord` holds `kind`, `lane`, `size`, `conn`, `recvSeconds`, `sequence`, `tag`, `cause` and a fixed
512 B `bytes` array, so every record has the same size and lives in a slot of a preallocated array: no allocation
per message. `payload()` is the used prefix, `bytes[0, size)`.

- `kind` is `Message`, `Connected` or `Disconnected`. An event record has `size` 0 and `lane` `Control`;
  its `lane` carries nothing, `Control` is only a fixed value.
- `recvSeconds` is the producer's receive time, in seconds, on whatever clock the host gives the
  transport. The inbox stores it and never reads it.
- `sequence` is the record's place in the producer's push order (§3). The inbox writes it; a consumer has
  no use for it.
- `tag` is the tag the record counts against (§13), or `kNoInboxTag` when it counts against none: an
  untagged message, a tag outside the configured range, and every event, in either ring.
- `cause` says why a connection closed, on a `Disconnected` event: `Unstated` (the default, and on every
  other record) or `ControlQuota`, which the producer passes to `pushEvent` or `pushPairedDisconnected` when
  it closed the connection for going over its Control quota (§12). The inbox stores it and never reads it.
- `kMaxSimMessageBytes` (512) is the largest message the inbox accepts; a larger one is refused and
  counted as `tooLarge` (§5). Keeping each message within it is the sender's job: the inbox has no way to
  split one.

The header bounds the record at 536 B (512 of payload and 24 of header). The fields as declared take
exactly 536 B: 16 B of header, then the 4 B `sequence`, the 2 B `tag` and the 1 B `cause`, then the
payload, and 1 B of tail padding to the `double`'s 8 B alignment. Measured with MSVC on Win64, 2026-11-15
(`cause` at offset 22, the payload at 23). The first layout, without `sequence` and `tag`, took 528 B; with
them and before `cause`, the record was also 536 B, with 2 B of tail padding.

## §3 One order for messages and events

Messages and connection events reach the consumer in the order the producer pushed them, whichever ring
holds them. A transport that pushes a connection's `Connected` before its first message, and its
`Disconnected` after its last, gives the consumer that order with no rule of its own to merge queues. A
consumer can therefore create a connection's state when it sees `Connected`, use it for every message,
and free it on `Disconnected`, without ever seeing a message for a connection it does not know.

- **Without a Control ring or an event ring** there is one ring, and its order is the push order.
- **With either** the producer gives every record it pushes, in any ring, the next value of one 32-bit
  counter (`sequence`). The drain merges the rings by it: of the oldest record of each ring, it hands out
  the one with the lowest sequence. Each ring is in push order already, so the merge is the push order
  across all three. A Control message pushed after a connection's `Connected` and before its next SimInput
  message is handed out between them, and a `Disconnected` after its connection's last message, exactly
  where a one-ring inbox puts them.
- **The comparison is the counter's signed difference**, so it survives the counter's wrap as long as fewer
  than 2^31 records wait at once. Each of the three rings holds at most 2^29 slots: a `static_assert` on
  `N`, an `OG_CHECK` on `controlSlots` and `eventSlots`, and a `static_assert` that three rings of that size
  stay below 2^31 (a record is 536 B, so 2^29 slots would be over 250 GiB). The bound was 2^30 per ring
  while there were two rings.

## §4 The reserve

The last `kReservedEventSlots` (64) slots of the shared ring are for events only. `pushMessage` refuses a
message for the shared ring when no more than 64 of its slots are free; `pushEvent` may use them. Of
`DefaultSimInbox`'s 2,048 slots, messages can fill 1,984, and the last 64 are always there for connection
events. A Control ring does not change this. An event ring does (§14): events then go to it and never take
a shared slot, and the 64 reserved slots stay free; messages still stop at them, so the shared ring's
message capacity is the same with or without one.

An event must not be lost. A lost `Disconnected` leaves the consumer holding the connection's state
forever (a leak), and a lost `Connected` leaves it without state for messages that follow. Most messages
are different: an input or a relayed input is repeated by later messages (they carry recent history), and
a correction or a clock publish is superseded by the next one. So under overload the inbox drops messages
and keeps events.

⚠ A message on the reliable `Control` lane is the exception. The transport has already delivered it, so
nothing repeats it.
- **Without a Control ring** the inbox drops a Control message like any other once only the reserve is
  left. Its `dropped[Control]` counter is the only trace. A consumer that cannot afford to lose one must
  drain before the 1,984 message slots fill.
- **With a Control ring** a Control message never competes with the other lanes for a slot (§12).

64 events is 32 connections each connecting and disconnecting between two drains. An event that finds
even the reserve full means the consumer has not drained for at least 1,984 messages' worth of traffic
plus 64 events. Without an event ring the header treats that as a defect, with an `OG_CHECK`, not as load:
the message says a lost connection event leaks a connection. Where the check is compiled out the event is
dropped (`SimInbox-guards.md` G-05). The reserve is no bound a remote peer cannot pass: with every source
at its quota (§13) the shared ring's messages fill its 1,984 message slots, and more than 32 connections
that open and close between two drains then reach the check. A producer whose sources can connect at
will needs an event ring (§14).

## §5 Refusals, and why the producer never waits

`push` returns a `SimInboxPush`; `pushMessage` is `push(...) == SimInboxPush::Pushed`. A refused message is
counted, and the result names the reason:
- `TooLarge`: the message is over 512 B, on any lane and in either ring;
- `QuotaRefused`: it is tagged, not Control, and its tag already holds `tagQuota` undrained records in the
  shared ring: `quotaRefused[lane]`, and the tag's own count per lane (§13);
- `ControlQuotaRefused`: it is a tagged Control message for the Control ring and its tag already holds
  `tagControlQuota` Control records there: `quotaRefused[Control]`, and the tag's own count (§12);
- `Dropped`: only the reserve is left in the shared ring (`dropped[lane]`), or the Control ring is full
  (`dropped[Control]`). The tags' Control quotas fit the Control ring, so only an untagged Control push can
  fill it, and that is also an `OG_CHECK` (§12).
- `EventRefused`: an event for the event ring found no room for itself (or, for a paired `Connected`, for
  itself and the slot it keeps). It is counted in `eventsRefused` (§14).

`pushEvent`, `pushPairedConnected` and `pushPairedDisconnected` return a `SimInboxPush` too: `Pushed`,
`EventRefused`, or, for an event into a full shared ring where the `OG_CHECK` is compiled out, `Dropped`.

The producer is a transport's receive path. It must keep receiving, and a wait for the consumer would
stall the transport, so every push returns at once. The result lets the producer act on what it lost: a
transport closes a connection whose Control message came back `ControlQuotaRefused`, and does not announce a
connection whose paired `Connected` came back `EventRefused`. The inbox counts every refusal too (§9).

## §6 Size and memory

`DefaultSimInbox` holds 2,048 records of 536 B: 1,097,728 B (about 1.05 MiB) in its shared ring, in one heap
array. A Control ring of `C` slots is a second heap array of `C` × 536 B: a server transport for 16
connections, each with a Control quota of 456, gives it 7,296 slots, 3,910,656 B (about 3.73 MiB), and a
client transport for its one connection 456 slots, 244,416 B. An event ring of `E` slots is a third
heap array of `E` × 536 B: 816 slots, 437,376 B, for a server transport with 16 connections; 9 slots,
4,824 B, for a client transport. Each tag adds two held counts (shared ring and Control ring) and five
refusal counts. The constructor value-initialises every array, which runs each record's member initialisers
and so writes every byte once: the memory is committed and touched before the first message, and nothing
in the inbox allocates after the constructor.

The 1,984 message slots hold 1,984 / R seconds of traffic at R messages per second: 0.62 s at 3,200 per
second, 2.3 s at 860 per second. For a host that drains once per frame or step, a stall of a few hundred
milliseconds fits with room to spare.

The shared ring's capacity is a power of two, so a slot is `index & (N - 1)` and the free-running indices
stay valid across their wrap (the argument is `Mailbox-rationale.md` §2's, for `SpscRing`). The Control
and event rings' capacities are whatever the producer needs (7,296 and 816 are not powers of two), so their
slot is `index % C`, on 64-bit indices: at 10^6 records a second they wrap after more than 500,000 years.

## §7 Threads: one producer, one consumer at a time

**The producer is one thread**, fixed for the inbox's lifetime. This is a usage contract the code does not
check, as with `SpscRing`: two threads that push at once race on the write indices, on the sequence, on the
event ring's paired count and on the slot. A second producer needs its own inbox.

**The consumer is one thread at a time, not one thread.** A host may call `drain` from a different thread
each time: a host that runs its sim on a chain of worker tasks, for example, drains on whichever worker
runs that frame's task. So the inbox never checks a thread id. It checks *overlap*: `drain` takes an
atomic in-drain flag with a compare-exchange at entry and clears it at exit, and an `OG_CHECK` fires if
the flag was already taken. The check's message is the contract: one consumer at a time; the owner may
change between drains, never during one.

**The memory orders.** The ring indices, all three rings', follow `SpscRing`'s discipline
(`Mailbox-rationale.md`): each side reads the other side's index with acquire and publishes its own with
release, and reads its own index relaxed. The event ring's count of open pairs (§14) is the producer's
alone: a plain integer that only the push functions read and write. The sequence adds one pairing: after each push the producer publishes the next sequence with
release, *after* the ring index (G-08), and the drain reads it with acquire *before* it reads the ring
indices. Every record below that sequence is then visible in its ring. The in-drain flag adds another: the
release that clears it at the end of one drain and the acquire that takes it at the start of the next.
That pairing is what makes one consumer's last read indices visible to the next consumer when the two are
different threads and the host orders them by nothing else. Each of these orders is a guard
(`SimInbox-guards.md` G-01 to G-04, G-08), because x86-64 cannot tell an acquire or release from relaxed.

**The tag counters.** The producer adds one (relaxed: only it adds) before it publishes the record. The
drain subtracts one after the callback returns and *after* it has published the ring's read index past the
record, with release; the producer's quota tests read the count with acquire. So a producer that sees a
count lowered also sees the slot that record held as free (G-10). The other way round, the producer could
see a tag with room while the ring's read index still shows the slot taken: with the tags' Control quotas
summing to exactly the Control ring, that is a full Control ring with every quota respected, the `OG_CHECK`
of §12. A quota test that reads a count the consumer is about to lower refuses at most what the quota
would have refused a moment earlier.

## §8 `drain`

```
template <class Fn> size_t drain(Fn&& fn);   // fn(const InboundSimRecord&)
```

- **Bounded at entry.** `drain` reads the producer's sequence once, at entry, and hands out the records
  below it. Records pushed while a drain runs wait for the next drain, so a producer that keeps pushing
  cannot keep one drain running forever.
- **In push order,** across all three rings (§3).
- **One slot freed per record.** After each callback returns, `drain` publishes its ring's read index past
  the record and then gives the record's tag count back (§7, §13), so a long drain gives slots and quota
  back to the producer as it goes.
- **The record is valid only inside the callback.** Once the callback returns, its slot is the producer's
  to overwrite. A consumer that needs the bytes later copies them.
- **The callback must not call `drain`.** A nested drain overlaps the running one and fails the overlap
  check (`SimInbox-guards.md` G-06 is what happens where the check is compiled out).
- It returns the number of records handed out.

## §9 Statistics

`stats()` returns a `SimInboxStats` snapshot of relaxed counters:

| counter | counts |
|---|---|
| `pushed[lane]` | messages accepted, into either ring |
| `dropped[lane]` | messages refused because only the shared ring's reserve was left, or (`Control`) because the Control ring was full |
| `quotaRefused[lane]` | tagged messages refused because their tag held its quota: the shared ring's (§13), or on `Control` the Control ring's (§12) |
| `tooLarge` | messages refused for being over 512 B, on any lane |
| `events` | connection events accepted, into the shared ring or the event ring |
| `maxOccupancy` | the most shared-ring records one drain handed out: the records waiting there at its entry |
| `controlHighWater` | the most records the Control ring ever held, counted at each push |
| `eventsRefused` | events refused by the event ring for want of room (§14) |
| `eventHighWater` | the most records the event ring ever held, counted at each push |

So the messages offered to `pushMessage` are `pushed + dropped + quotaRefused` per lane plus `tooLarge` in
all, and once the consumer has drained everything, the messages it received are the sum of `pushed`.
`quotaRefusedFor(tag)` gives one tag's refusals per lane, `undrained(tag)` the records the tag holds in the
shared ring now, `controlUndrained(tag)` those it holds in the Control ring, and `resetTagCounters(tag)`
zeroes the tag's refusal counts when the producer gives the tag to a new source (only the producer may call
it). Nothing zeroes a held count: only the drain lowers it.

The counters are written by their own side only (the producer writes all but `maxOccupancy` and the tag
counts' subtractions; the consumer writes those) and read from any thread. They are counts for logs, not a
hand-off: a snapshot taken on another thread is not consistent across counters, and nothing should decide
on one.

## §10 What the compiler enforces

These are `static_assert`s in the header, not guards or prose:
- the capacity `N` is a power of two;
- `N` is more than the 64 reserved slots, so messages have at least one slot;
- `N` is at most 2^29, and three rings of that size stay below 2^31, so the sequence's signed difference
  orders every waiting record (§3);
- `kMaxSimMessageBytes` fits the record's `uint16_t` `size` field;
- `InboundSimRecord` is at most 536 B;
- the atomics the inbox uses (`size_t`, `uint64_t`, `uint32_t` and `bool`) are lock-free.

`SimLane`'s values and count are pinned in `SimTransport.h`.

## §11 Why not `SpscRing<InboundSimRecord, N>`

`SpscRing` (`Mailbox.h`) is the generic ring, and the inbox keeps its index discipline. It is not used
directly because:
- `SpscRing` stores its slots inside the object. At 2,048 records that is a 1 MiB object, too large for a
  stack and awkward as a member; the inbox puts its records in heap arrays.
- `SpscRing::tryPop` moves the item out into a `std::optional`, a 536 B copy per record. `drain` hands the
  record to the callback in place.
- The reserve for events, the overlap check on the consumer, the second and third rings with their merged
  order, the paired events and the tags are not `SpscRing` features, and adding them there would change a
  type other code already uses.

## §12 The Control ring

A message on the reliable Control lane is lost for good if the inbox refuses it (§4). In one shared ring
that happens whenever the other lanes have filled the 1,984 message slots, which a peer on the other end
of a transport can arrange: a flood that stays within the transport's own rate limits still fills the
shared ring between two drains. With `controlSlots` > 0, every Control message goes to a ring of its own,
which only Control messages fill.

- **Bounded per tag, so that tagged pushes cannot fill it.** How many Control messages arrive between two
  drains has no bound the inbox could size for: a source can keep sending, and a reconnecting source
  starts afresh. So each tag may hold at most `tagControlQuota` undrained Control records, and the
  constructor's `OG_CHECK` requires a tagged inbox with a Control ring to have a Control quota of at least
  one and `tags × tagControlQuota ≤ controlSlots`. Every tagged Control push then finds a slot. A
  transport for 16 connections with a quota of 456 has a ring of exactly 16 × 456 = 7,296.
- **Over its quota, a Control message is refused and the producer is told.** It is counted in
  `quotaRefused[Control]` and in the tag's `quotaRefusedFor(tag)`, and `push` returns
  `ControlQuotaRefused`. The message is lost for good, so this is not a refusal a correct source should
  ever meet: a transport answers it by closing the source's connection, and passes `ControlQuota` as the
  `Disconnected` event's `cause` so the consumer can say why.
- **Full is a defect.** Only an untagged Control push (an inbox with no tags, a tag outside the range, or a
  caller that passes none) counts against no quota and so can fill the ring. A Control message that finds
  the ring full fails an `OG_CHECK` whose message says so and that a refused message is lost for good.
  Where the check is compiled out, the message is refused and counted in `dropped[Control]`
  (`SimInbox-guards.md` G-09).
- **Too large is still refused.** A Control message over 512 B is `tooLarge`, as in the shared ring.
- **Its high-water mark** (`controlHighWater`) is counted at each push, so a log can show how close a ring
  came to its size.

## §13 Tags and the quota

One shared ring is first come, first served: a source that sends faster than the consumer drains takes
slots the others need. A tag gives each source its own share.

- **The producer names the source.** `pushMessage`'s last argument is a tag, 0 to `tags - 1`; the inbox
  does not know what a tag stands for (a transport with several connections gives one to each). The default,
  `kNoInboxTag`, and any tag outside the range mean "no source": the message counts against no quota.
- **The count is what the tag holds now.** A tagged message accepted into the shared ring adds one to its
  tag's count, and a tagged Control message accepted into the Control ring to its tag's Control count; the
  drain subtracts one after its callback (§7). `undrained(tag)` and `controlUndrained(tag)` read them.
- **A count belongs to the tag, not to the source.** When the producer gives a tag to a new source, the
  records the old source left undrained still count against the tag until the drain hands them out. The
  new source starts with that residue against its quotas. That is what keeps the bound: a source that
  leaves and comes back under a new tag cannot bring fresh room with it while its old records still
  wait.
- **The quota.** A tagged message on any lane but Control is refused while its tag already holds
  `tagQuota` records: counted in `quotaRefused[lane]` and in the tag's own `quotaRefusedFor(tag)`. The test
  comes before the reserve test, so a message over its quota is counted as a quota refusal even when the
  ring is also full. A `tagQuota` of 0 means no quota on these lanes: a producer with a single source has
  no one to isolate it from, and its refusals then stay `dropped`.
- **Control.** With a Control ring, a tagged Control message counts against its tag's Control quota (§12)
  and takes no shared slot. Without one, it takes a shared slot like any other, counts against its tag's
  shared count, and is never refused for the quota.
- **Isolation by arithmetic.** When `tags × tagQuota` is at most the 1,984 message slots, every tagged
  source together cannot fill the shared ring, so no source can be refused because of another's traffic,
  only by its own quota; and `tags × tagControlQuota` within the Control ring does the same for Control.
  Untagged messages are outside both bounds.

## §14 The event ring and paired events

A lost `Disconnected` leaks the consumer's state for its connection, and a lost `Connected` leaves its
messages without one (§4). In the shared ring an event has only the reserve, which a producer whose sources
can connect and disconnect at will can exhaust between two drains. With `eventSlots` > 0 every event goes to
a ring of its own, which only events fill, and the producer can pair them:

- **`pushPairedConnected(conn, t)`** pushes a `Connected` and *keeps a slot* for that connection's
  `Disconnected`. It is accepted only if the ring has room for both: the records it holds now, plus one
  slot for every pair still open, plus two. Otherwise it returns `EventRefused`, pushes nothing and keeps
  nothing.
- **`pushPairedDisconnected(conn, t, cause)`** pushes the `Disconnected` of a connection whose paired
  `Connected` was accepted, into the slot that was kept for it. It always finds that slot (below). A call
  with no pair open is a producer defect: it fails an `OG_CHECK`, and where the check is compiled out it
  is pushed like an unpaired event (`SimInbox-guards.md` G-11). A paired `Disconnected` that finds no room
  fails a second `OG_CHECK`, which names the only way to reach it: a `Disconnected` paired by a producer
  whose `Connected` was not.
- **`pushEvent(kind, conn, t, cause)`** with an event ring pushes an *unpaired* event: one that keeps
  nothing and is accepted only while one slot is free beyond the records held and the slots kept. It
  serves events that have no partner to keep a slot for, such as a failed connect's `Disconnected`, or a
  `Connected` that repeats one already paired. Refused, it returns `EventRefused`.

**Why a paired `Disconnected` always fits.** Let `held` be the records in the event ring as the producer
sees them and `open` the pairs it has open. Every accepted push keeps `held + open` at or below the ring's
size: a paired `Connected` raises `held` and `open` by one each and is accepted only if the sum before it
is at most the size minus two; an unpaired event raises `held` by one and is accepted only if the sum is
at most the size minus one; a paired `Disconnected` raises `held` by one and lowers `open` by one, so the
sum does not move, and since `open` was at least one, `held` was at most the size minus one: a slot was
free. A drain only lowers the true `held`, and the producer sees it lowered late or not at all, so its
`held` is never below the truth and the bound is conservative.

**The bound is the ring's size, whatever the sources do.** However many connections open between two
drains, and however long the drain gap, the event ring never overflows, no `Disconnected` of a paired
connection is ever refused, and no event `OG_CHECK` can be reached by anything a source sends. What a full
ring costs is a refused `Connected`: the producer learns it from `EventRefused`, before it has pushed
anything for that connection, so it can keep the connection's records out of the inbox altogether (a
transport closes the connection). The consumer then sees neither of its events and none of its messages.

**Order.** The event ring takes its sequence from the same counter as the other rings (§3), so a
`Connected` comes before every message pushed after it, and a `Disconnected` after every message pushed
before it, as in one ring.

Without an event ring, `pushPairedConnected` and `pushPairedDisconnected` are `pushEvent`: the shared ring
and its reserve (§4), with no pairing. The default `SimInboxConfig` keeps that.

## Provenance

Written with the header on 2026-11-14. The record size was measured the same day; the reserve, the
refusals, the order and both two-thread contracts are covered by the inbox's tests, and the two
`OG_CHECK`s were seen to fire with their messages.

2026-11-15: the Control ring, the sequence and its merged drain, and the tags and their quota were added,
so that a transport with several connections can keep them from taking each other's slots. With no
`SimInboxConfig` the inbox behaves as before, and the inbox's own test cases pass unchanged against the new
header. The new parts are covered by the tests of the transport that configures them, including one
producer and one consumer on two threads over both rings; the new `OG_CHECK` was seen to fire with its
message.

2026-11-15, later: the Control ring's bound became a per-tag Control quota (`tagControlQuota`), with the
`ControlQuotaRefused` result, the record's `cause`, `controlUndrained`, a `tagQuota` of 0 meaning none, and
the drain's give-back moved after the read index's publish (§7). Before, the ring was sized for one burst of
each source, and sources that kept sending between two drains, or reconnected, could fill it and reach its
`OG_CHECK`. The default `SimInboxConfig` still behaves as the header first shipped.

2026-11-15, later still: the event ring (`eventSlots`), the paired events and their two `OG_CHECK`s, the
`EventRefused` result, `pushEvent`'s result, the `eventsRefused` and `eventHighWater` counts, and the bound
of 2^29 slots per ring for three rings. The shared ring's reserve was the only room for events, and with
every source at its quota more than 32 connections opening and closing between two drains reached its
`OG_CHECK`. Without an event ring the inbox behaves as before; its own test cases pass unchanged against the
new header, and the new parts are covered by the tests of the transport that configures them, where each
new `OG_CHECK` was seen to fire with its message.
