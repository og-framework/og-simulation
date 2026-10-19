<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickSlotTable.h` — rationale

The narrative behind `TickSlotLogTarget` and `TickSlotTable`. The header keeps its licence, its docs
pointer, its code, its `static_assert` and `OG_CHECK` messages, its log strings and two guard tags;
everything that explains it lives here. Prohibitions are in `TickSlotTable-guards.md`.

**If this file and `TickSlotTable.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free and
physics-free by construction: STL and other `og-simulation` headers only. Everything below is true to
a reader with no game engine, no physics engine and no other repository.

---

## §1 What it is for

A physics backend with a fixed body set (`BodySlotOccupancy.h`) binds each character to one slot,
and every peer must put a character in the same slot (`BodySlotAssignment-rationale.md` §1).
`decideBodySlot` is that rule as a pure function. `TickSlotTable` is the bookkeeping around it that a
world host needs:
- which owner holds which slot;
- the `taken` set it hands to `decideBodySlot`;
- a record of which waits it has already reported for each owner;
- the `[SimHost.Slot]` lines.

The table is physics-agnostic, so another physics backend can reuse it.

## §2 The interface and the boundary

- `acquire(key, name, isAuthority, replicatedSlot) -> std::optional<uint32_t>`:
  1. if `key` already holds a slot, it returns that slot, decides nothing and logs nothing;
  2. otherwise it builds `taken` from the slots held now, and the lowest slot free in it;
  3. it calls `decideBodySlot(isAuthority, replicatedSlot, taken)` (guard G-01);
  4. on a wait, it logs the wait if this owner has not yet logged that kind (§5) and returns no slot
     (guard G-02);
  5. on `BodySlotOutcome::Full` it fails an `OG_CHECK` and returns no slot (§6);
  6. on `BodySlotOutcome::Assigned` it records the holder, ends the owner's wait record, logs
     `assigned` and returns the slot.
- `release(key) -> std::optional<uint32_t>`: ends the owner's wait record, then removes its holder
  and returns its slot. An owner that holds none returns no slot. The holder is removed before the
  call returns, so a caller that logs `size()` afterwards logs the count without this owner.
- `slotOf(key)`: the slot `key` holds, or none. It changes nothing.
- `size()`: the number of owners holding a slot.

**What stays with the caller** (the world host and the game behind it):
- the owner key and its name;
- the peer's role (`isAuthority`) and the replicated slot (`replicatedSlot`), read from wherever the
  game replicates it;
- **publishing the authority's choice.** On the authority, the caller writes the returned slot to
  where its clients will read it. It must do so only for a **new** assignment: an `acquire` for an
  owner that already holds a slot returns the same slot again. A caller tells the two apart with
  `slotOf(key)` before the call, as the test caller in `TickSlotTableTest.cpp` does
  (`alreadyHeld`);
- binding the slot's bodies, the occupancy hand-over, any second (shadow) world's release, and their
  log lines.

The authority's publish therefore happens after the `assigned` line, where the code this table
replaces (§10) published it before. Both happen in one call on one thread, so a publish that writes
no line of its own leaves the order of the lines unchanged.

## §3 The lines

The table writes exactly two lines, at `HostLogLevel::Log`, with the code's `printf` format strings:
- `[SimHost.Slot] wait=%s owner=%s slot=%d heldBy=%s`: the kind (`WaitForReplication` or
  `WaitForRelease`), this owner's name, `-1` for a replication wait or the slot waited on, and the
  name of the owner holding that slot (`-` for a replication wait);
- `[SimHost.Slot] assigned role=%s owner=%s slot=%u lowestFreeLocally=%d`: `Authority` or `Client`,
  this owner's name, the slot, and the lowest slot that was free **before** this assignment (`-1` if
  none was, which cannot happen for an assignment). On the authority it equals the slot. On a client
  it can be lower than the slot, and a reader comparing it with the slot sees the replicated choice
  at work.

**The log target** is any type with `bool logEnabled(HostLogLevel) const` and
`void log(HostLogLevel, const char*)`: the concept `TickSlotLogTarget`, the two log members of the
tick driver's engine concept (`TickDriverEngine` in `TickDriverConcepts.h`). So a world host passes
its engine. The table keeps a pointer to the target and two function pointers built from its type,
so the table is not a template and a host holds it as a plain member. The target must outlive the
table. `logEnabled` is asked before each line is formatted, so a disabled level costs no formatting.

**The buffer.** Each line is formatted with `formatHostLine` (`TickDriverLog.h`) into one `HostLine`
reserved at construction with `kReservedLineChars` (256) characters. The line pointer handed to `log`
is valid only during that call. A longer line grows the buffer once and stays correct
(`TickSlotTable.Lines.AreFormattedIntoTheReservedLine`, with a 600-character name).

## §4 The owner key and name

The key is an opaque `uint64_t`: the table only compares keys. A host typically passes an object
address or a stable id. The name is the caller's, as a NUL-terminated string, for the lines only:
- a wait line and an `assigned` line print the name passed to that `acquire`;
- `acquire` copies the name into the holder's record when it assigns, and a later `WaitForRelease`
  line of another owner prints that copy as `heldBy`. A holder's name is therefore the one it had
  when it was assigned. A host whose names never change sees no difference from reading the name
  again.

## §5 Waits are logged once per owner and kind

A client can ask for a slot many times while it waits (each retry of a bind). Each owner has a record
with one bit per wait kind. The first wait of a kind sets its bit and logs; later waits of that kind
log nothing. A replication wait followed by a release wait logs both, once each. The record ends when
the owner is assigned or released, so a wait after that is logged again.

The bit is set even when the log level is disabled at that moment; that wait is then never logged
for this owner, even if the level is turned on later
(`TickSlotTable.Waits.EachKindIsLoggedOncePerOwner`, last section). The code this table replaces
behaved the same.

## §6 A full table

`decideBodySlot` answers `BodySlotOutcome::Full` only on the authority, when every slot is held. The
world has a fixed body set, so that is a configuration error: an `OG_CHECK`. In a build where the
check is compiled out, `acquire` returns no slot and logs nothing, and the caller treats it as a
wait. A client never gets `Full` (`BodySlotAssignment-rationale.md` §2).

## §7 Allocation and threads

Measured on 2026-10-16 with a counting global `operator new` in a scratch MSVC Debug build: a
repeated wait, an `acquire` for an owner that holds a slot, `slotOf`, `size` and `release` allocate
nothing. The first wait of an owner allocates its record, and an assignment allocates the holder's
record and its name copy. A line longer than the reservation grows the buffer once (§3).

Slot binding happens when a character registers, not on the step path. A world host's per-frame
reads use `slotOf`, which does not allocate.

The table has no synchronisation. A host calls it from one thread at a time.

## §8 Guards

- **G-01** sits on the `decideBodySlot` call: the decision is that function's, from the caller's role
  and replicated slot.
- **G-02** sits on the wait branch's `return std::nullopt;`: a wait returns no slot.

Both are also caught by tests (the guards doc names them), and both name their predecessor there.
`TickSlotLogTarget` carries no guard: its three `static_assert`s pin it.

## §9 What the tests show

`TickSlotTableTest.cpp`, tag `[TickSlotTable]`:
- `TickSlotTable.Differential.EqualsTheLegacyJoltSlotAcquireOverRandomOrders`: a reference model,
  transcribed literally from the code this table replaces (§10), with only its engine types and
  macros swapped for plain ones, against the table plus a test caller. Each of 24 fixed seeds runs 500
  random events on an authority peer and a client peer:
  - joins (the authority acquires);
  - repeated acquires;
  - the client learning of an owner;
  - replication of the authority's slot;
  - client acquires;
  - authority leaves and client leaves, in any order.

  After every event the returned slots, every owner's slot, the size, the lines (level and text), the
  published slots, the occupancy, bind and shadow calls and the `[SimHost.Bind] release` lines of the
  caller are equal. Before each acquire the test also checks that both `taken` sets, and so both
  decisions, are equal. Witness counters assert that every outcome occurred: authority and client
  assignments, a client assigned above a lower free slot, both waits, a repeated wait with no line,
  an acquire of a held slot, and releases with and without a slot.
- `TickSlotTable.Lines.MatchTheGoldens`: both lines, byte for byte, in all four variants (authority
  and client assignments, both waits), each at `HostLogLevel::Log`.
- `TickSlotTable.Waits.EachKindIsLoggedOncePerOwner`: §5, in five sections.
- `TickSlotTable.Client.NeverAssignedWithoutTheReplicatedSlot`: the client waits however often it
  asks, then binds the replicated slot above a lower free one.
- `TickSlotTable.Assigned.LowestFreeLocallyIsFromTheSlotsHeldBeforeTheAssignment`: §3.
- `TickSlotTable.Release.ErasesTheOwnerAndReturnsItsSlot`: §2.
- `TickSlotTable.Lines.AreFormattedIntoTheReservedLine`: §3.

Each of the following edits was made to the header and seen failing, on 2026-10-16:
- logging a wait twice;
- `lowestFreeLocally` from the slots held after the assignment;
- `true` for `isAuthority` at the decision;
- a client falling back to the lowest free slot;
- returning `decision.slot` from a wait;
- one bit for both wait kinds.

The differential failed on each of them. Keeping the wait record across a release fails only
`TickSlotTable.Waits.EachKindIsLoggedOncePerOwner`, because the random orders never re-acquire a
released owner.

## §10 Provenance

Written on 2026-10-16 for the og-tickdriver migration. It ports the slot acquire and release of the
brawler game's Unreal Jolt backend host (its Jolt slot acquire and release functions) without engine
types or physics. That code keyed its tables by actor pointer, read the replicated slot from the
pawn, and published the authority's slot itself. Here the caller does those (§2). The lines are the
same bytes. Nothing in this file depends on any document outside this submodule.
