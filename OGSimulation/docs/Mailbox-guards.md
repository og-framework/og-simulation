<!-- SPDX-License-Identifier: MPL-2.0 -->
# `Mailbox.h` — guards

Every prohibition that governs a line of `Mailbox.h`. Each entry has an **opaque, stable id**. In
the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `Mailbox.h` disagree, the header is authoritative and this file is stale.** Fix
this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is
moved to §R and its number is spent forever. A retired id may be **named** in prose; it may never
again appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** The full memory-ordering argument, per operation, lives
in `Mailbox-rationale.md` §3, §5 and §10. A guard is a prohibition plus the consequence of ignoring
it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine, no other repository and no other file open.

---

## Why these four are guards and not tests

All four guards forbid **weakening a memory order**. That is the one edit to this header that no
test run on an x86-64 machine can catch. x86-64 is strongly ordered: an acquire load, a release
store and a relaxed load or store all compile to the same plain `mov`, and every read-modify-write
is a locked instruction whatever order it names. Measured on this header (§6 of the rationale):
- with G-01 to G-03 set to `std::memory_order_relaxed`, clang emits the identical x86-64
  instruction listing and MSVC emits the same instruction sequence with different registers;
- with G-04 set to `std::memory_order_relaxed`, clang emits the same atomic instructions on x86-64
  (a locked compare-exchange, no fence) and every `[Mailbox]` test passes, the snapshot stress
  test included, 20 runs out of 20.

So the single-thread tests, all three two-thread stress tests and every Win64 run keep passing. The
defect appears only on a weakly ordered CPU (ARM64: Android, and Apple silicon if it is ever a
target), where the same edit turns the `ldar` or `stlr` that makes the hand-off safe into a plain
`ldr` or `str`, and the acquire-release swap or compare-exchange into a relaxed one. The
simulation thread runs on ARM64 Android, so the defect would ship.

---

## G-01 — `observe` is an ACQUIRE load. Never relaxed.

**Tag site:** `Mailbox.h`, in `SpscRing::observe`, immediately above
`return otherSidesIndex.load(std::memory_order_acquire);`.

**The prohibition.** Do not change `std::memory_order_acquire` to `std::memory_order_relaxed` (or
to `consume`), and do not replace the call to `observe` in `tryPush` or `tryPop` with a direct
relaxed load of the other side's index.

**The consequence.** `observe` is how each side reads the index the *other* thread publishes.
- In `tryPop` it reads `writeIndex_`. Without acquire, the consumer may see the new index and then
  read the slot's bytes from *before* the producer constructed them: a half-built or stale item,
  moved out and handed to the caller as valid.
- In `tryPush` it reads `readIndex_`. Without acquire, the producer may see the slot as free and
  construct into it while the consumer's move-out of the previous item there is still in flight:
  the consumer receives a mix of two items.

Neither failure crashes; both deliver wrong data across a thread hand-off, on ARM64 only.

**What breaks if it moves.** Both cross-thread index reads go through this one function, so this
one line is the acquire half of both hand-offs. The thread's *own* index is read relaxed, directly
and correctly: only the thread that writes an index may read it relaxed.

---

## G-02 — `publish` is a RELEASE store. Never relaxed.

**Tag site:** `Mailbox.h`, in `SpscRing::publish`, immediately above
`ownIndex.store(next, std::memory_order_release);`.

**The prohibition.** Do not change `std::memory_order_release` to `std::memory_order_relaxed`, and
do not replace the call to `publish` in `tryPush` or `tryPop` with a direct relaxed store.

**The consequence.** `publish` is how each side hands a slot to the other thread.
- In `tryPush` it advances `writeIndex_` after the item is constructed. Without release, the
  index may become visible to the consumer before the item's bytes do; the acquire in G-01 then
  has nothing to synchronise with, and the consumer reads an unbuilt item.
- In `tryPop` it advances `readIndex_` after the item is moved out and destroyed. Without
  release, the producer may reuse the slot while the consumer is still reading it.

**What breaks if it moves.** As G-01: both cross-thread index writes go through this one
function, and this line is the release half of both hand-offs. An acquire without its release, or
a release without its acquire, orders nothing.

---

## G-03 — `swapMiddle` is an ACQ_REL exchange. Never weaker.

**Tag site:** `Mailbox.h`, in `TripleBuffer::swapMiddle`, immediately above
`return middle_.exchange(handOver, std::memory_order_acq_rel);`.

**The prohibition.** Do not change `std::memory_order_acq_rel` to `std::memory_order_release`,
`std::memory_order_acquire` or `std::memory_order_relaxed`, and do not split the exchange into a
load and a store.

**The consequence.** Both sides call `swapMiddle`, and each call needs **both** halves.
- In `publish` (writer): the **release** half makes the writer's writes to the slot it hands over
  visible to the reader that later takes it. The **acquire** half matters too: the slot the writer
  gets back may be one the reader has just handed over, and without acquire the writer may start
  overwriting it while the reader's last reads of it are still in flight.
- In `acquireLatest` (reader): the **acquire** half makes the published slot's contents visible
  before the reader reads them. The **release** half orders the reader's reads of the slot it
  hands back before the writer's reuse of it.

Weakening either half yields a torn struct on ARM64: a reader that sees part of one publish and
part of another, which is the one property the triple buffer exists to rule out. Splitting the
exchange into a load and a store breaks it on every CPU, because a publish landing between the two
is overwritten and its slot is lost from the rotation (two sides then hold the same slot).

**What breaks if it moves.** The middle slot is the only shared state in the triple buffer, and
this is the only line that touches it apart from the relaxed `dirty` hint.

---

## G-04 — `exchangeState` is an ACQ_REL compare-exchange. Never weaker.

**Tag site:** `Mailbox.h`, in `SnapshotChannel::exchangeState`, immediately above
`return state_.compare_exchange_weak(expected, desired, std::memory_order_acq_rel, std::memory_order_relaxed);`.

**The prohibition.** Do not change the success order `std::memory_order_acq_rel` to
`std::memory_order_release`, `std::memory_order_acquire` or `std::memory_order_relaxed`. Do not
split the compare-exchange into a load and a store, and do not write `state_` anywhere except
through this function (in particular, never with a plain `store`).

**The consequence.** `state_` is the whole shared state of the snapshot channel: the published
slots, newest first, and the set of slots the reader holds. Every hand-off between the two threads
is one successful exchange here, and each needs one half of the order:
- `commit` (writer) needs the **release** half. It makes the snapshot the writer just filled
  visible to a reader whose peek reads the new order. Without it, the reader may read a
  half-written snapshot.
- `peekNewest` (reader) needs the **acquire** half. It pairs with that release.
- `release` (reader) needs the **release** half. It orders the reader's last reads of a snapshot
  before the writer's reuse of the slot.
- `beginWrite` (writer), when it recycles a slot, needs the **acquire** half. It pairs with that
  release. Without it, the writer may start overwriting a snapshot the reader is still reading.

Weakening either half yields a torn snapshot on ARM64 only. Splitting the exchange, or adding a
plain store, breaks the channel on every CPU: a peek or a release that lands between the load and
the store is overwritten. A lost hold lets the writer recycle a snapshot the reader is reading, and
a lost commit loses a slot from the pool. A plain store also ends the release sequences that the
argument in rationale §10 relies on. Measured: with the exchange split into a load and a store, the
snapshot stress test crashed in 4 runs out of 4.

**What breaks if it moves.** All four operations change `state_` through this one function, and
this is the only line that writes it. The **failure** order is `std::memory_order_relaxed`
correctly: no snapshot is touched after a failed exchange (rationale §10), so strengthening it is
harmless but not needed.

---

## §R Retired ids

**None.**

The capacity rule (a power of two), the lock-free requirement on every atomic the header uses, the
default-constructible element of `TripleBuffer` and the nothrow destructor of `SpscRing`'s element
were never guards: each is a `static_assert` in the header from the start (rationale §7). The same
holds for `SnapshotChannel`'s slot-count bounds (at least 2, at most 12) and its
default-constructible element.

---

## Provenance

G-01 to G-03 were written with the header on 2026-10-06 and describe the header as first shipped.
G-04 was added later the same day, with `SnapshotChannel`. Nothing in this file depends on any
document outside this submodule.
