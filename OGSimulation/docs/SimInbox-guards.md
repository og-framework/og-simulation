<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimInbox.h` — guards

Every prohibition that governs a line of `SimInbox.h`. Each entry has an **opaque, stable id**. In the
header a single line `// ⛔G-nn  docs/SimInbox-guards.md` sits exactly where the forbidden edit would be
typed.

**If this file and `SimInbox.h` disagree, the header is authoritative and this file is stale.** Fix this
file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved to
§R and its number is spent forever. A retired id may be **named** in prose; it may never again appear as
a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** The narrative lives in `SimInbox-rationale.md`. A guard is a
prohibition plus the consequence of ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is true
and actionable to a reader with no game engine, no other repository and no other file open.

---

## Why G-01 to G-04 are guards and not tests

All four forbid **weakening a memory order**. No test run on an x86-64 machine can catch that edit:
x86-64 is strongly ordered, so an acquire load, a release store and a relaxed load or store are all
plain moves, and every read-modify-write is a locked instruction whatever order it names. Measured on
this header (2026-11-14, a Win64 Development build): with all four orders set to
`std::memory_order_relaxed` at once, the inbox's five default test cases passed in 10 runs out of 10,
the one-producer, one-consumer stress case and the case whose consumers alternate between two threads
included. The defect appears only on a weakly ordered CPU (ARM64), where the simulation also runs.
`Mailbox-guards.md` makes the same argument for `SpscRing`, whose index discipline this header copies.

G-10 forbids both: weakening the give-back's release, and moving the give-back before the read index's
publish. The weaker order is invisible on x86-64 for the reason above. The reorder is in the source, so it
is wrong on every CPU, but no test catches it either: the window is the few instructions between the two
stores. Measured (2026-11-15, MSVC Release on x86-64): with the give-back moved above the publish, the
two-thread test case over both rings, whose Control ring is exactly its four tags' Control quotas, passed
10 runs out of 10.

G-08 forbids a reorder, not a weaker order, and the same holds for it: on one thread the reorder changes
nothing, and the merged drain it protects is only wrong when the consumer reads between the two stores.
Measured (2026-11-15, MSVC Release on x86-64): with the two stores swapped, a test case with one producer
and one consumer on two threads over both rings (400,000 records) passed 10 runs out of 10.

## Why G-05 to G-07, G-09 and G-11 are guards and not tests

Each forbids deleting a fallback that follows an `OG_CHECK`. The fallback runs only where the check is
compiled out: a UE build with checks disabled (a Shipping client, by default) and the standalone build
with `NDEBUG`, where `OG_CHECK` is `assert`. Where the check is live it stops the program, or, in a test
runner that reports a failed check as a failed case, ends the case before the fallback is reached. So a
test build cannot reach the fallback, and its deletion passes every test.

---

## G-01 — `observe` is an ACQUIRE load. Never relaxed.

**Tag site:** `SimInbox.h`, in `SimInbox::observe`, immediately above
`return otherSidesIndex.load(std::memory_order_acquire);`.

**The prohibition.** Do not change `std::memory_order_acquire` to `std::memory_order_relaxed` (or to
`consume`), and do not replace a call to `observe` with a direct relaxed load of the other side's index.

**The consequence.** `observe` is how each side reads the index the *other* side publishes.
- In `drain` it reads `pushedSequence_`, `writeIndex_`, `controlWrite_` and `eventWrite_`. Without acquire, the consumer
  may see the new sequence or index and then read a record's fields and bytes from *before* the producer
  wrote them: a stale or half-written record, handed to the callback as valid.
- In `push` and `pushEvent` it reads `readIndex_`, in `pushControl` `controlRead_`, and in
  `pushRingEvent` `eventRead_`. Without acquire, the producer may see a slot as free and overwrite it while
  the consumer's callback is still reading it.
- In the quota tests of `push` and `pushControl` it reads a tag's held counts, which the drain lowers. That
  acquire is G-10's other half.

Neither failure crashes; both deliver wrong bytes across a thread hand-off, on ARM64 only.

**What breaks if it moves.** Every cross-thread read (three write indices, three read indices, the sequence
and the tags' held counts) goes through this one function template. Each side reads its *own* indices relaxed, directly and
correctly.

---

## G-02 — `publish` is a RELEASE store. Never relaxed.

**Tag site:** `SimInbox.h`, in `SimInbox::publish`, immediately above
`ownIndex.store(next, std::memory_order_release);`.

**The prohibition.** Do not change `std::memory_order_release` to `std::memory_order_relaxed`, and do not
replace a call to `publish` with a direct relaxed store.

**The consequence.** `publish` is how each side hands a slot to the other.
- After a push (`commit`) it advances `writeIndex_`, `controlWrite_` or `eventWrite_`, and then `pushedSequence_`, after
  the record is written. Without release, the index or the sequence may become visible before the record's
  bytes; G-01's acquire then has nothing to pair with, and the consumer reads an unwritten record.
- In `drain` it advances `readIndex_`, `controlRead_` or `eventRead_` after the callback returns. Without release, the
  producer may overwrite the slot while the callback's reads of it are still in flight.

**What breaks if it moves.** As G-01: every index and sequence write goes through this one function
template, and this line is the release half of every hand-off. An acquire without its release orders
nothing.

---

## G-03 — `enterDrain` is an ACQUIRE compare-exchange. Never relaxed.

**Tag site:** `SimInbox.h`, in `SimInbox::enterDrain`, immediately above
`return draining_.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed);`.

**The prohibition.** Do not change the success order `std::memory_order_acquire` to
`std::memory_order_relaxed`, and do not split the compare-exchange into a load and a store.

**The consequence.** The consumer may be a different thread on each drain. The previous consumer's
`leaveDrain` (G-04) is a release store of `false`; this acquire, reading it, is what makes everything the
previous consumer did visible to the next one: its last `publish` of `readIndex_`, its write of the
deepest occupancy, and whatever its callback wrote. Without it, on ARM64, the next consumer may read an
older `readIndex_`. It then hands records to the callback a second time and publishes `readIndex_`
*backwards*, after which the producer counts slots the consumer has already freed as full, and the next
drain delivers the same records again.

A host that orders its consumers by other means (a task chain, a thread join) loses nothing from this
acquire; a host that does not is correct only because of it. The inbox does not ask which kind of host it
has.

Splitting the compare-exchange breaks the overlap check on every CPU: two consumers that both load
`false` before either stores `true` both enter, and run at once.

**What breaks if it moves.** This is the only line that takes the consumer role.

---

## G-04 — `leaveDrain` is a RELEASE store. Never relaxed.

**Tag site:** `SimInbox.h`, in `SimInbox::leaveDrain`, immediately above
`draining_.store(false, std::memory_order_release);`.

**The prohibition.** Do not change `std::memory_order_release` to `std::memory_order_relaxed`, and do not
move the call to `leaveDrain` in `drain` above the loop that hands out the records.

**The consequence.** This is the release half of G-03. Without it, the next consumer's acquire has nothing
to pair with, with G-03's consequence on ARM64. Moving the call above the loop lets the next consumer enter
while this one is still handing out records, on every CPU: both read the same `readIndex_` and deliver the
same records.

**What breaks if it moves.** This is the only line that gives the consumer role up.

---

## G-05 — `pushEvent` returns when no slot is left. Never delete the return.

**Tag site:** `SimInbox.h`, in `SimInbox::pushEvent`, immediately above `if (freeSlots == 0u)`, the
statement after the `OG_CHECK` on `freeSlots > 0u`.

**The prohibition.** Do not delete this `if` and its `return` as dead code behind the check, and do not
move the record writes above it.

**The consequence.** Where the check is compiled out, an event pushed into a full inbox would be written
into the slot of the oldest unread record (a full ring's write slot is its read slot), and `writeIndex_`
would move to `N + 1` records past `readIndex_`. The consumer then loses that record and is handed the
event twice in its next drain. Until that drain hands out its first record, the producer's free-slot
count (`N` minus the occupancy, in unsigned arithmetic) wraps to a huge number, so every message pushed
in the meantime is accepted and written over records the consumer has not read yet. With the return, the event alone is
lost, as the check's message says.

**What breaks if it moves.** This is the only path that writes an event into the shared ring, and so the
only shared-ring write without the reserve test that `pushMessage` makes. An inbox with an event ring never
reaches it: its events go to that ring.

---

## G-06 — `drain` returns when it could not take the consumer role. Never delete the return.

**Tag site:** `SimInbox.h`, in `SimInbox::drain`, immediately above `if (!entered)`, the statement after
the `OG_CHECK` on `entered`.

**The prohibition.** Do not delete this `if` and its `return` as dead code behind the check.

**The consequence.** Where the check is compiled out, an overlapping drain would run as a second
consumer. A drain called from inside another drain's callback, or from a second thread, reads the same
`readIndex_`, which does not yet count the record the first drain is handling. It hands out that record
and every one after it, and calls `leaveDrain`, which gives up the role while the first drain still holds
it. The first drain then continues from its own copy of the index, hands out those records a second time
and publishes `readIndex_` backwards (G-03's consequence, on every CPU). On two threads it is also a data race on the records.

**What breaks if it moves.** The overlap check exists to stop exactly this; the return is its effect in
builds without checks.

---

## G-07 — `indexOf` clamps the lane. Never index the counters with the raw lane.

**Tag site:** `SimInbox.h`, in `SimInbox::indexOf`, immediately above
`return std::min(index, kSimLaneCount - 1u);`, the statement after the `OG_CHECK` on the lane.

**The prohibition.** Do not return `index` unclamped, and do not index `pushed_` or `dropped_` with
`static_cast<size_t>(lane)` anywhere else in the class.

**The consequence.** `SimLane` is an enumeration of five values, but a transport that maps its own lane
numbers by a cast can hand over any value of its underlying `uint8_t`. Where the check is compiled out, an
unclamped value of 5 or more makes `fetch_add` write past the per-lane arrays: into the next members, and
for larger values past the object, a write into whatever the heap holds there. Clamped, a bad lane is
counted under the last lane.

**What breaks if it moves.** Every per-lane counter is reached through this function.

---

## G-08 — the sequence is published after the ring index. Never before it.

**Tag site:** `SimInbox.h`, in `SimInbox::commit`, immediately above
`publish(pushedSequence_, nextSequence_);`.

**The prohibition.** Do not move this line, or the `++nextSequence_` above it, before
`publish(ringWriteIndex, next)`, and do not publish the sequence anywhere else.

**The consequence.** The drain reads the sequence first and hands out every record below it, in sequence
order, from every ring (rationale §3, §8). It finds a record only through its ring's write index, which it
reads after the sequence. Published the other way round, the drain can see a record's sequence before its
ring's index covers it. It then finds that ring's oldest record "not yet there", hands out another ring's
later record first, and the missing one in the next drain: a Control message before the `Connected` of its
connection, or a message after its connection's `Disconnected`. A consumer that frees a connection's state
on `Disconnected` then meets a message for a connection it does not know. On every CPU, because the
reorder is in the source.

**What breaks if it moves.** This is the only line that publishes the sequence; the three write indices are
published by the `publish` call above it.

---

## G-09 — a Control message into a full Control ring returns. Never delete the return.

**Tag site:** `SimInbox.h`, in `SimInbox::pushControl`, immediately above `if (!hasSlot)`, the statement
after the `OG_CHECK` on `hasSlot`.

**The prohibition.** Do not delete this `if` and its `return` as dead code behind the check, and do not
move the record writes above it.

**The consequence.** The tags' Control quotas fit the ring (a constructor `OG_CHECK`), so only an untagged
Control push reaches the check. Where the check is compiled out, a Control message pushed into a full Control ring
would be written into the slot of the oldest unread Control record (a full ring's write slot is its read
slot), and `controlWrite_` would move past a full ring. Every Control message pushed before the next drain
does the same to the next-oldest record. That drain then walks more than one lap of the ring: each
overwritten record is lost without a trace, and the record that overwrote it is handed out twice, once in
the lost record's place, out of order. With the return, the message alone is refused and counted in
`dropped[Control]`.

**What breaks if it moves.** This is the only path that writes a Control record into its own ring.

---

## G-10 — `giveBack` is a RELEASE subtraction after the read index's publish. Never relaxed, never before it.

**Tag site:** `SimInbox.h`, in `SimInbox::giveBack`, immediately above
`held.fetch_sub(1u, std::memory_order_release);`.

**The prohibition.** Do not change `std::memory_order_release` to `std::memory_order_relaxed`, and do not
move the `giveBack` call in `drain` above the `publish` of the ring's read index, nor lower a tag's count
anywhere else.

**The consequence.** The producer's quota test reads the count with acquire (G-01) and then the ring's read
index. If it can see the count lowered before it can see the read index past that record, it finds room in
the tag's quota while the ring still shows the record's slot taken. With the Control quotas summing to the
Control ring's size, all tags at their quota leave the ring exactly full: the next Control push then finds
the ring full though no quota was exceeded, and fails G-09's `OG_CHECK`, or, where it is compiled out, is
dropped for good. Moved above the publish, this happens on every CPU; with a relaxed subtraction, on ARM64.

**What breaks if it moves.** This is the only place a held count goes down; `drain` calls it once per
tagged record.

---

## G-11 — a paired Disconnected with no pair open does not lower the open count. Never decrement it unconditionally.

**Tag site:** `SimInbox.h`, in `SimInbox::pushPairedDisconnected`, immediately above `if (paired)`, the
statement after the `OG_CHECK` on `paired`.

**The prohibition.** Do not delete this `if` and decrement `eventsPaired_` unconditionally, as dead code
behind the check.

**The consequence.** Where the check is compiled out, a `Disconnected` paired by a producer that opened no
pair would wrap the open count, an unsigned 64-bit integer, from 0 to 2^64 - 1. The event ring's room test
adds the open count to the records it holds, and the sum wraps too: every later event would see room that is
not there, and be written into the slot of the oldest unread event (a full ring's write slot is its read
slot). The consumer then loses that event, a `Connected` or a `Disconnected`, and is handed the new one twice.
With the `if`, the `Disconnected` is pushed as an unpaired event and the open count stays true.

**What breaks if it moves.** This is the only place the open count goes down.

---

## §R Retired ids

**None.**

The capacity rules (a power of two, more than the 64 reserved slots), the payload limit fitting the record's
`uint16_t` size, the record's 536 B bound and the lock-free atomics were never guards: each is a
`static_assert` in the header from the start (rationale §10).

---

## Provenance

G-01 to G-07 were written with the header on 2026-11-14 and describe the header as first shipped. The
x86-64 measurement above was made the same day, on that header. G-08 and G-09 were added on 2026-11-15
with the Control ring and the sequence, and G-01 and G-02 were widened to the new indices. G-10 was added later
the same day with the per-tag Control quota, G-01 widened to the held counts, and G-09's consequence
narrowed to the untagged push that alone can reach it. G-11 was added later the same day with the event ring
and its paired events; G-01 and G-02 now also cover that ring's indices, which go through the same `observe`
and `publish`. Nothing in this file depends on any document outside this submodule.
