<!-- SPDX-License-Identifier: MPL-2.0 -->
# `Mailbox.h` — rationale

The narrative behind the three thread-crossing mailboxes in `Mailbox.h`: `SpscRing`, a bounded
queue that delivers every item in order; `TripleBuffer`, a latest-value slot that is never torn; and
`SnapshotChannel`, a pool of the newest few snapshots that the reader reads in place and holds while
it uses them. The header keeps its licence, its code and one `⛔G-nn` tag per guard; everything that
explains it lives here.

**If this file and `Mailbox.h` disagree, the header is authoritative and this file is stale.** Fix
this file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in
`Mailbox-guards.md`, reachable from the `⛔G-nn` tag at that site.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction (STL only: `<atomic>`, `<bit>`, `<optional>` and friends). Everything below is true
to a reader with no game engine and no other repository.

---

## §1 What they are for, and which one to use

All three classes move data between exactly **two threads**: one producer and one consumer, fixed
for the object's lifetime. Typical pairs are a game or render thread and a simulation thread, in
either direction. No class locks, allocates after construction, or waits for the other thread.
`SpscRing` and `TripleBuffer` return at once from every call. `SnapshotChannel` may retry a
compare-exchange a few times when the other thread changed its state at the same instant (§9,
"Progress").

| need | use | on overflow |
|---|---|---|
| every item, in order (commands, received messages, outbound batches, acknowledgements) | `SpscRing<T, N>` | `tryPush` returns `false`, the item stays with the caller, `drops()` counts it |
| only the newest value (an input snapshot, a state the reader samples) | `TripleBuffer<T>` | cannot overflow: a newer publish replaces an unread one |
| the newest snapshot **and the ones before it**, read in place and held while in use (interpolating between the two newest, a large state read without a copy) | `SnapshotChannel<T, N>` | cannot overflow: the writer recycles the oldest snapshot the reader does not hold; only if the reader holds every slot does `beginWrite` return `nullptr`, and `drops()` counts it |

The roles are a **usage contract the code cannot check**: calling `tryPush` from two threads,
`acquireLatest` from two threads, or `peekNewest` from two threads, is a data race. A second
producer needs a second ring.

`SimulationQueues.h` already has a type-specific SPSC ring, `PendingInputQueue` (one slot left
empty to tell full from empty). `SpscRing` is the generic form for new crossings; the existing
queues are unchanged by it.

## §2 `SpscRing<T, N>`

**Indices.** Two free-running counters, `writeIndex_` (written only by the producer) and
`readIndex_` (written only by the consumer). They are never reduced modulo `N`; a slot is
`index & (N - 1)`. At every instant, in unbounded arithmetic,
`readIndex_ <= writeIndex_ <= readIndex_ + N`, and the slots of the indices in
`[readIndex_, writeIndex_)` hold live items.
`writeIndex_ - readIndex_` in unsigned arithmetic is the exact item count, so all `N` slots are
usable (no wasted slot), empty is a difference of 0 and full is a difference of `N`.

**Why `N` must be a power of two.** It is not only for the mask. A counter of `size_t` wraps after
2⁶⁴ increments (2³² where `size_t` is 32 bits), and slot numbering stays continuous across that
wrap only if `N` divides the counter's range, which for a power of two of bits means `N` is a
power of two. A non-power-of-two `N` with `index % N` would jump to a wrong slot at the wrap. The
header enforces it with `std::has_single_bit(N)`; `N = 1` is a valid one-slot mailbox.

**Lifetime of an item.** Slots are raw aligned storage, not `T` objects: `tryPush` constructs the
item in place and `tryPop` moves it out and **destroys it in the slot** before freeing the slot.
So an item's destructor runs when it is popped, not when its slot is next overwritten, which
matters when the item owns something (a strong reference that keeps another object alive must be
released when the consumer takes it). `T` needs no default constructor. Items still queued when
the ring is destroyed are destroyed with it; that destructor reads both indices relaxed, so the
ring must outlive both threads' use of it (join them, or otherwise synchronise, first).

**Full.** `tryPush` on a full ring returns `false`, increments `drops()` and leaves the argument
untouched: an rvalue passed to it is not moved from, so the caller can retry, keep, or discard it.
Retrying after a `false` is how the stress test delivers 10⁶ items with no loss through a
64-slot ring. `drops()` is cumulative and never reset; it counts rejected pushes, not lost items,
because the ring cannot know what the caller did next.

**`sizeApprox()`** is the count at some instant during the call, clamped to `N`. From the
producer or the consumer thread it errs only on the safe side for that thread: the producer may
see fewer free slots than there are, the consumer fewer items. From a third thread it is a
diagnostic only.

## §3 `SpscRing` — the memory-ordering argument, per operation

The argument is made in the C++ memory model, not for one CPU, so it holds on a weakly ordered CPU
(ARM64) as well as on x86-64. Each hand-off is one **release** store of an index that the other
thread reads with an **acquire** load; when the load reads the stored value, everything the
storing thread did before the store happens-before everything the loading thread does after the
load. Both cross-thread loads go through `observe` (guard G-01) and both cross-thread stores
through `publish` (guard G-02).

**`tryPush` (producer thread):**
1. `writeIndex_` is loaded **relaxed**. Only this thread writes it, so program order alone gives
   this thread its own latest value.
2. `readIndex_` is loaded **acquire** (`observe`, G-01). It synchronises with the consumer's
   release store in `tryPop` that wrote the value read. So the consumer's move-out and destruction
   of every item below that index happen-before this push. The slot about to be written last held
   item `writeIndex_ - N`, which is below the observed `readIndex_` whenever the ring is not full,
   so it is free and no consumer access to it can still be in flight.
3. If full: `drops_` is incremented **relaxed**. Only the producer writes it, and no other data is
   published through it, so no ordering is needed; a reader of `drops()` sees the count
   eventually and never sees it decrease.
4. The item is constructed in the slot: plain, non-atomic writes.
5. `writeIndex_ + 1` is stored **release** (`publish`, G-02). Step 4's writes are ordered before
   it, so a consumer whose acquire load reads the new index also sees the complete item.

A stale `readIndex_` in step 2 (the consumer freed a slot an instant ago) can only make the ring
look fuller than it is: the push is rejected and counted, which is correct for the instant
observed. It can never make a slot look free that is not.

**`tryPop` (consumer thread):** the mirror image.
1. `readIndex_` is loaded **relaxed** (own index).
2. `writeIndex_` is loaded **acquire** (G-01). It synchronises with the producer's release in
   `tryPush` step 5, so the item in slot `readIndex_` is fully constructed and visible.
3. If empty, `std::nullopt`. A stale `writeIndex_` can only make the ring look emptier; the item
   is returned by a later call.
4. The item is moved out and destroyed: plain accesses.
5. `readIndex_ + 1` is stored **release** (G-02). Step 4 is ordered before it, so a producer that
   reads the new index in its step 2 cannot overwrite the slot while it is still being read.

**`drops()`:** a relaxed load. It reads a counter only, never data, so it may lag by an instant
and needs no ordering.

**`sizeApprox()`:** `readIndex_` is loaded **acquire first**, then `writeIndex_`. The acquire is
what keeps the result non-negative: on ARM64 two relaxed loads of different variables may be
satisfied out of order, which could pair a new `readIndex_` with an older `writeIndex_`. With the
acquire, the consumer's own earlier observation of `writeIndex_` (made before it stored the
`readIndex_` value read) happens-before the second load, so the second load reads a
`writeIndex_` at least that large.

**Destructor:** relaxed loads; it relies on the caller having synchronised with both threads
(§2).

## §4 `TripleBuffer<T>`

**Three slots, three owners.** `back_` is the writer's slot, `front_` is the reader's slot, and
`middle_` (the only atomic) holds the third slot's index in bits 0–1 plus a **dirty** bit (bit 2).
At every instant `{back_, middle_ & 3, front_}` is a permutation of `{0, 1, 2}`: each side only
ever swaps its own index with the one in `middle_`, and a swap preserves the permutation. The
writer and the reader therefore never hold the same slot, which is why a reader can never see a
torn struct, whatever its size, without any lock.

- **`write()`** returns the writer's slot. ⚠ Its contents are **stale**: whatever that slot held
  when it came back to the writer, usually the publish before last or a value the reader has
  finished with, never reliably the latest one. A writer must write every field it means to
  publish (or keep its own copy of the last value and assign it). The stress test writes every
  field of every publish. This is a rule for the calling code, with no site in this header, so it
  is not a guard.
- **`publish()`** swaps the writer's slot, marked dirty, into `middle_` and takes the old middle
  slot as the new writer slot.
- **`acquireLatest()`** returns the newest published value. If `middle_` is dirty it swaps its own
  slot (clean) into `middle_` and takes the published one; otherwise it keeps the slot it has.
  It returns `nullptr` until the first publish, so a reader can tell "nothing yet" from a
  default-constructed value. The returned pointer stays valid, and its contents unchanged, until
  the reader's next `acquireLatest`.
- **`dirty()`** is true when a publish has happened that the reader has not taken yet.

**Latest wins.** Publishes the reader does not take in time are overwritten: three publishes
between two reads deliver only the third. The reader never sees an older publish after a newer
one: every swap on `middle_` is a read-modify-write, all of them fall in one total order, and the
reader always takes what the latest swap before its own left there. A consumer that needs every
item uses `SpscRing`.

`T` must be default constructible: the three slots exist from construction (a `static_assert`).

## §5 `TripleBuffer` — the memory-ordering argument, per operation

**`write()` (writer thread):** no atomic access. The slot is the writer's alone (§4) until it is
handed over by `publish`.

**`publish()` (writer thread):** one **acq_rel** exchange on `middle_` (`swapMiddle`, G-03).
- *Release:* every write the writer made to its slot is ordered before the exchange. The reader's
  acquire exchange that later takes this slot reads this exchange's value (or a later
  read-modify-write in the same release sequence), so it sees those writes.
- *Acquire:* the slot the writer receives may be the one the reader has just swapped in. The
  reader's release half orders its last reads of that slot before its exchange, and this acquire
  orders them before the writer's next writes to it. Without it the writer could overwrite a slot
  the reader is still reading.

**`acquireLatest()` (reader thread):**
1. A **relaxed** load of `middle_` (`dirty()`) decides whether to swap. It is a hint only: no data
   is read on its strength. Only the reader ever clears the dirty bit, so if the hint says dirty,
   the exchange that follows also finds it dirty and takes a published slot. If the hint is stale
   and says clean, the reader returns its current slot (an older but complete value) and sees the
   new publish on a later call.
2. One **acq_rel** exchange (G-03). *Acquire* pairs with the writer's release in `publish`: the
   published slot's contents are visible before the reader reads them. *Release* orders the
   reader's reads of the slot it hands back before the writer's reuse of it.
3. `front_` and the "has a value" flag are reader-only plain variables.

**`dirty()`:** a relaxed load. It reads one bit and publishes nothing.

## §6 Why the orders are guards, and what the tests can and cannot show

The orders above are the whole correctness argument, and **no test on an x86-64 machine can catch
a weakened one.** x86-64 keeps loads in order with other loads and stores in order with other
stores, so an acquire load or a release store needs no fence there and compiles to a plain `mov`,
the same as a relaxed one. Measured on this header (2026-10-06, a probe translation unit
instantiating `tryPush`, `tryPop`, `publish` and `acquireLatest`, built at `-O2` / `/O2`), with the
three guarded orders of the ring and the triple buffer (G-01 to G-03) set to
`std::memory_order_relaxed`:

| compiler, target | result |
|---|---|
| clang (Android NDK r27), x86-64 | identical instruction listing |
| MSVC 14.44, x64 | identical instruction sequence (237 instructions), different register choices |
| clang (Android NDK r27), ARM64 | `ldar` → `ldr`, `stlr` → `str`, `__aarch64_swp4_acq_rel` → `__aarch64_swp4_relax` |

A second probe, for `SnapshotChannel` (2026-10-06, instantiating `beginWrite`, `commit`,
`peekNewest` and `release` on a 4-slot channel), with the order in `exchangeState` (G-04) set to
`std::memory_order_relaxed`:

| compiler, target | result |
|---|---|
| clang (Android NDK r27), ARM64 | all 8 compare-exchange calls in the listing: `__aarch64_cas8_acq_rel` → `__aarch64_cas8_relax` |
| clang (Android NDK r27), x86-64 | the same atomic instructions in both (`lock cmpxchg`, no fence); the listings differ only in loop layout (the acq_rel build reloads the writer's `writing_` inside the commit loop) |
| clang 19, Windows x64, the real tests | the whole `[Mailbox]` set passes (20 cases), and the three stress tests pass 20 runs out of 20 |

So a weakened order passes every Win64 test, the stress tests included, and fails only on ARM64,
where the simulation thread also runs (Android). That is why each of the four sites carries a
guard (`Mailbox-guards.md`) rather than relying on a test.

What the tests do show, each witnessed by running them against a deliberately broken copy of the
header:

| broken copy | caught by |
|---|---|
| `publish` keeps the writer's slot instead of swapping (writer and reader share a slot) | `Mailbox.TripleBuffer.StressReaderNeverSeesATornStruct` (732,154 torn reads in one run), `Mailbox.TripleBuffer.WriterAndReaderSlotsNeverAlias`, `Mailbox.TripleBuffer.LatestWinsAndTheDirtyFlag` |
| the full test lets one more item in than the ring holds | `Mailbox.SpscRing.FullRingRejectsWithoutConsumingAndCountsTheDrop`, and `Mailbox.SpscRing.StressMillionItemsInOrderWithNoLoss` on its own (5,443 and 7,207 out-of-order items in two runs) |
| a rejected push is not counted | `Mailbox.SpscRing.FullRingRejectsWithoutConsumingAndCountsTheDrop`, `Mailbox.SpscRing.StressMillionItemsInOrderWithNoLoss` |
| `SnapshotChannel` recycles without looking at the held set | 7 cases, among them `Mailbox.SnapshotChannel.HeldSnapshotIsNeverRecycled` and `Mailbox.SnapshotChannel.StressPairedPeeksNeverTornAndAlwaysOrdered` (8,173 torn reads in one run) |
| `SnapshotChannel` recycles the newest unheld snapshot instead of the oldest | 7 cases, among them `Mailbox.SnapshotChannel.WriterRecyclesTheOldestUnheldSnapshot`, `Mailbox.SnapshotChannel.NewestFirstAndBackIndexing` and the snapshot stress test (the newest snapshot went backwards between reads) |
| a peek does not hold what it returns | 8 cases, the snapshot stress test among them (torn reads) |
| a recycle that finds every slot held is not counted | `Mailbox.SnapshotChannel.DropCountedWhenEverySlotIsHeld` |
| no per-read view: `back` is always counted from the current newest | `Mailbox.SnapshotChannel.OneReadSeesOneConsistentView`, and the snapshot stress test on its own (7,651 pairs whose `back = 1` was not older than `back = 0`) |
| `exchangeState` split into a load and a store | the snapshot stress test crashes (4 runs out of 4) |

The torn-struct check uses a probe of 32 words whose last word is a checksum of the others, so a
read that mixes two publishes fails the checksum. In a correct run the reader takes most of the
10⁶ publishes while the writer is running (about 0.87 million distinct values in one measured
run), so the check is exercised, not vacuous. The snapshot stress test uses the same probe for both
snapshots of each read, and also checks that the newest one does not change while it is held. In
one measured run it made 510,922 reads, 510,819 of them pairs, and saw 499,592 distinct newest
snapshots while the writer was running.

⚠ No thread sanitizer run backs this header: none was available on the machine it was written on.
The ordering argument in §3 and §5 is the evidence for weakly ordered CPUs.

## §7 Compile-time checks

Each of these replaced what would otherwise have been a comment, and each is a `static_assert` in
the header:

| check | why | witnessed |
|---|---|---|
| `std::has_single_bit(N)` | §2: the slot mapping across the counter wrap | `N = 3` and `N = 0` fail to compile |
| `std::is_nothrow_destructible_v<T>` (ring) | `tryPop` destroys the item after moving it out, and the destructor of the ring destroys the rest; a throwing destructor would leave an index unadvanced over a dead slot | a `T` with `noexcept(false)` destructor fails to compile |
| `std::is_default_constructible_v<T>` (triple buffer) | the three slots exist from construction (§4) | a `T` with only an `int` constructor fails to compile |
| `N >= 2` (snapshot channel) | §9 "Sizing": the smallest channel in which a reader holding one snapshot never stops the writer | `N = 1` fails to compile |
| `N <= 12` (snapshot channel) | §9 "The state word": twelve 4-bit slot entries fit beside the count and the held set in 64 bits | `N = 13` fails to compile; `N = 2` and `N = 12` compile and are tested |
| `std::is_default_constructible_v<T>` (snapshot channel) | the `N` slots are built with the channel (§9) | a `T` with only an `int` constructor fails to compile |
| `is_always_lock_free` for every atomic type used | a lock-based atomic would make a mailbox block, which defeats its purpose on a real-time thread | not witnessable on a target platform: all three atomic types are lock-free on x86-64 and ARM64 |

## §8 Layout

Data written by different threads lives on different 64-byte cache lines, so the producer's and
the consumer's writes do not slow each other down (false sharing). In `SpscRing`, `writeIndex_`
and `drops_` (both producer-written) share a line, `readIndex_` has its own, and the slot storage
starts on a fresh line. In `TripleBuffer`, each slot, `back_`, `middle_` and the reader's `front_`
start on their own lines. In `SnapshotChannel`, `state_` (written by both threads) has its own
line, the writer's `writing_` and `drops_` share one, the reader's `heldBack_` has one, and each
slot starts on a fresh line. This is for speed only; nothing in §3 or §5 depends on it.

The line size is the constant `kCacheLineBytes = 64`, the line size of x86-64 and of the ARM64
cores Android devices use. The standard library's interference-size constant is not used because
its value is allowed to change with compiler and tuning flags, which would change these classes'
layout between translation units built differently.

## §9 `SnapshotChannel<T, N>`

**What it is for.** `TripleBuffer` hands over only the newest value. A reader that needs the
snapshot before it as well (interpolation between the two newest simulation steps, for example)
uses `SnapshotChannel`: `N` snapshots of type `T`, built with the channel, which the writer fills in
place and the reader reads in place. Nothing is copied, and nothing is allocated after
construction. The channel never constructs, copies, moves or assigns a `T` after its constructor,
so the only allocations a snapshot can cause are the ones its own default constructor makes, while
the channel is being built. `Mailbox.SnapshotChannel.NoAllocationAfterConstruction` counts them
with a `T` that allocates and can be neither copied nor moved.

**The state word.** All state the two threads share is one `std::atomic<uint64_t>`, `state_`:

| bits | content |
|---|---|
| 0–11 | the held set: bit `s` is set while the reader holds slot `s` |
| 12–15 | how many slots are published |
| 16–63 | the published slots, newest first: 4 bits per slot index, position 0 is the newest |

At every instant:
- the held set is a subset of the published slots, because the reader can hold only a published
  slot and the writer never unpublishes a held one;
- the writer's open slot (`writing_`) is neither published nor held;
- every other slot is free, so the published slots and the open slot together are at most `N`.

Because the order and the held set share one word, every change of state is a single
compare-exchange, and neither thread can ever see one of them updated without the other. A pair of
SPSC rings over the slots (a ring of published slots and a ring of free ones) cannot give this
channel's behaviour. Only the reader can take from the published ring, so the writer could not take
back the oldest published snapshot when the reader falls behind. It would have to drop instead,
however few snapshots the reader actually holds. Here the published order plays the part of the
first ring, and the free slots, the complement of the published slots and the open one, play the
part of the second.

**Writer (one thread).**
- `beginWrite()` returns the open slot. If a slot is already open (no `commit` since the last
  `beginWrite`), it is returned again. Otherwise it opens:
  1. a free slot, if there is one (this happens only before the first `N` commits);
  2. otherwise, the **oldest published snapshot the reader does not hold**, which it unpublishes in
     the same step;
  3. otherwise, nothing: every slot is published and held. It returns `nullptr` and `drops()`
     counts one.

  ⚠ As with `TripleBuffer::write`, the slot's contents are **stale**: whatever it held last. The
  writer must write every field it means to publish.
- `commit()` publishes the open slot as the newest. With no open slot it does nothing, so a
  `commit` after a `nullptr` from `beginWrite` is harmless.
- The writer never waits for the reader. When the reader stops reading, the writer keeps recycling
  the slots the reader does not hold.

**Reader (one thread).**
- `peekNewest(back)` returns a published snapshot and **holds** it: `back = 0` is the newest,
  `back = 1` the one before, and so on. It returns `nullptr` when fewer than `back + 1` are
  published, and always for `back >= N`. A held snapshot is never recycled and never written: it
  stays valid and unchanged until `release`.
- A **read** runs from a peek made while the reader holds nothing to the `release` that leaves it
  holding nothing again. Within a read, the reader counts `back` from its **anchor**: the held
  snapshot with the smallest `back`, which is always the newest snapshot it holds. What a peek sees
  depends on its `back` against that smallest held `back`:
  - **`back` at or above the smallest held `back`:** the peek returns the anchor or an older
    snapshot, so no commit made after the anchor was peeked is visible to it. While no peek of the
    read has used a smaller `back` than the read's first peek, the anchor is that first peek's
    snapshot, so these peeks see the newest snapshot **as it was when the read began**. If the
    snapshot at that place has been recycled in the meantime, the peek returns the next older one,
    or `nullptr`: still older, never newer.
  - **`back` below every held `back`** (possible only when the read's first peek used
    `back >= 1`): the snapshots newer than the anchor are not held, so the writer may have
    recycled them and committed new snapshots into their slots. The peek may then return a
    snapshot **committed during the read**, or `nullptr` while the writer has a recycled slot
    open. Whatever it returns is newer than every held snapshot, and it becomes the anchor.
  - So the order between the peeks of one read holds whatever the writer does in between. A
    larger `back` never returns a newer snapshot than a smaller one, and `back = 1` after
    `back = 0` returns one strictly older, or `nullptr`. But only a read whose first peek is
    `back = 0` is guaranteed one fixed view, the one at the read's start.
  - How: for each slot it holds, the reader remembers the `back` it was peeked with (`heldBack_`).
    It counts from the held slot with the smallest one. That slot is still published, and the
    published slots stay in commit order, so offsets from it toward older snapshots stay valid.
    An offset toward newer snapshots counts over slots the reader does not hold, which is why the
    second case can land on a later commit.
  - `Mailbox.SnapshotChannel.OneReadSeesOneConsistentView` pins the first case.
    `Mailbox.SnapshotChannel.PeekBelowEveryHeldBackMaySeeACommitFromTheRead` pins the second at
    `N = 3`: after commits 1, 2, 3, a read peeks `back = 1` and gets 2; after commits 4 and 5,
    `back = 0` returns 4, which was committed during the read but is still newer than the held 2.
    The same case shows the `nullptr`, and that the same commits stay invisible to a read that
    peeks `back = 0` first.
- ⚠ **Usage rule: a reader that needs a fixed read-start view peeks `back = 0` first.** Its
  smallest held `back` is then 0, so no later peek of the read can go below it. This is a rule for
  the calling code, with no site in this header, so it is not a guard.
- Peeking a slot that is already held returns it again. Holds are per slot, not counted, so one
  `release` frees it.
- `release(snapshot)` ends the hold on `snapshot`. `nullptr`, a pointer that is not one of the
  channel's snapshots, and a snapshot that is not held are ignored.
- ⚠ **Release everything at the end of each read.** A reader that keeps a snapshot held from one
  read to the next keeps counting `back` from it, so a peek at or above its `back` never sees a
  newer commit. This is also a rule for the calling code, with no site in this header, so it is not
  a guard.

**Sizing: how many slots the reader may hold, and `N`.** The reader may hold any number of
snapshots at once, up to all `N`. What the writer can then do depends on `H`, the largest number
the reader holds at any one time:

| `N` against `H` | consequence |
|---|---|
| `N >= H + 2` | never drops, and the newest snapshot is never recycled: a read that starts while the writer is filling a slot still sees the last commit |
| `N == H + 1` | never drops, but when the reader holds every older snapshot the writer recycles the newest, so a read during that write sees the one before it |
| `N <= H` | the writer can find every slot held: `beginWrite` returns `nullptr` and counts a drop |

Why the first row holds: `beginWrite` recycles only when all `N` slots are published (no slot is
free and none is open). At least `N - H >= 2` of them are then unheld, and the oldest of two or more
unheld snapshots is not the newest.
`Mailbox.SnapshotChannel.SizingRuleKeepsTheNewestWhileTheReaderHoldsTwo` shows the first two rows
with `H = 2`.

**The sizing rule is `N = H + 2`.** `kMaxHeldKeepingNewest` (`N - 2`) is the largest `H` the first
row allows. A caller states its own `H` at compile time by asserting that its channel type's
`kMaxHeldKeepingNewest` is at least `H` (a `static_assert` at the declaration). A reader
that interpolates between the two newest snapshots holds `H = 2`, so `N = 4`. A reader that only
takes the newest holds `H = 1`, so `N = 3`. The header asserts `N >= 2`, the smallest channel in
which a reader holding one snapshot never stops the writer, and `N <= 12` (the state word).

**Progress.** Every operation is a short loop around one compare-exchange on `state_`. An exchange
fails only when the other thread changed `state_` after this one read it, which means the other
thread made progress. So the channel is lock-free but not wait-free, and neither thread ever waits
for the other to do something. In the stress test (10⁶ commits against a reader that peeks
`back = 0` and `back = 1` and releases, as fast as it can) the whole run takes about 0.05 s.

**ABA.** A compare-exchange succeeds when `state_` equals the value it read, even if `state_`
changed and changed back in between (a slot recycled and committed again into the same position).
That is harmless here, because `state_` is the whole state each decision is computed from. When the
exchange succeeds, the decision is right for the current state, whatever happened before. The
reader's private `heldBack_` describes only slots the reader holds, and those cannot change under
it.

## §10 `SnapshotChannel` — the memory-ordering argument, per operation

The argument is made in the C++ memory model, so it holds on ARM64 as well as on x86-64. After
construction, every change to `state_` is a successful compare-exchange in `exchangeState`, which is
**acq_rel** on success (guard G-04). Two consequences carry the whole argument:
1. All changes to `state_` fall in one modification order, and every compare-exchange reads the
   latest value in it. So each operation takes effect at its successful exchange, and the
   operations of the two threads are totally ordered.
2. Every change is a read-modify-write, so every later value of `state_` lies in the **release
   sequence** of every earlier exchange. An acquire that reads any later value synchronises with
   each earlier release, however many exchanges of either thread came in between. A plain store
   would end those release sequences, which is one reason G-04 forbids one.

**`beginWrite()` (writer thread):**
1. `writing_` is a plain variable: only the writer uses it.
2. `state_` is loaded **relaxed**. Only the writer changes the published order and count, so this
   load sees them exactly: a load never reads a value older than the thread's own last write. The
   held set it reads may be out of date. It is only a first guess, which the exchange checks.
3. **Free slot** (fewer than `N` published, so before the first `N` commits): no exchange. The slot
   has never been published, so the reader has never read it.
4. **Recycle:** one acq_rel exchange that removes the oldest unheld slot from the published order.
   - It succeeds only if that slot is still unheld in the latest value. If the reader's peek of
     that slot comes first in the modification order, the exchange fails and the writer computes a
     new victim. So the writer never takes a slot the reader holds.
   - Its **acquire** half synchronises with the reader's release exchange that last cleared the
     slot's held bit (or with any later value, by the release sequence). So every read the reader
     made of that snapshot happens-before the writer's next write into it.
5. **Drop:** the exchange writes back the value it read, unchanged. Its success confirms that at
   that instant every slot really was published and held, so a drop is counted only when it is
   real. `drops_` is incremented **relaxed**: only the writer writes it, and it publishes no data.

**`commit()` (writer thread):** the writer's writes into the open slot are plain, and they come
before one acq_rel exchange that puts the slot first in the published order. Its **release** half
makes them visible to any reader peek whose acquire reads this value or a later one. A retry
recomputes from the latest value, so a hold or release the reader made in between is kept, not
overwritten.

**`peekNewest()` (reader thread):**
1. `state_` is loaded **relaxed**. Only the reader changes the held set, so this load sees it
   exactly. The published order may be out of date and is checked by the exchange.
2. One acq_rel exchange sets the slot's held bit. If nothing is found, or the slot is already held,
   the exchange writes the value back unchanged, so `nullptr` and a repeated peek are also exact
   at the instant of the exchange.
   - Its **acquire** half synchronises with the `commit` that published the slot, by the release
     sequence, so the snapshot's contents are visible before the reader reads them.
   - The slot becomes held in the same step in which it is seen published. From then on the writer
     cannot recycle it (`beginWrite` step 4), so it is not written again until the reader releases
     it.
3. `heldBack_` is plain: only the reader uses it.

**`release()` (reader thread):** the reader's reads of the snapshot come before one acq_rel
exchange that clears its held bit. Its **release** half orders them before the writer's recycle
exchange (`beginWrite` step 4) that takes the slot. The early return for a snapshot that is not
held reads the held bit **relaxed**, which is exact: only the reader changes it.

**`drops()`:** a relaxed load of a counter. It reads no data.

**A failed exchange** is **relaxed**. After a failure no operation touches a snapshot: each loop
computes again from the value the failure returned, and only a successful exchange leads to a slot
being read or written (or to a return).

**Construction and destruction** happen while no other thread uses the channel. As for `SpscRing`,
join or otherwise synchronise both threads before the channel is destroyed.

## §11 Provenance

Written on 2026-10-06 as the engine-free mailbox for every thread crossing between a host's game
thread and a project-owned simulation thread. `SnapshotChannel` (§9, §10) was added later the same
day, for readers that need more than the newest snapshot. A game engine's own triple buffer was deliberately
not wrapped, so the core stays engine-free. ⚠ The design work that motivated it lives in private
working material that is **not distributed with this submodule**; nothing above depends on it.
