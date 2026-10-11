<!-- SPDX-License-Identifier: MPL-2.0 -->
# `BodySlotAssignment.h` — rationale

The narrative behind `BodySlotOutcome`, `BodySlotDecision` and `decideBodySlot`. The header keeps its
licence, its docs pointer, its code and one `OG_CHECK` message; everything that explains it lives here.

**If this file and `BodySlotAssignment.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: STL only, no logging. Everything below is true to a reader with no game engine, no physics
engine and no other repository.

---

## §1 What it is for

A physics backend that keeps a fixed body set (`BodySlotOccupancy.h` §1) gives each character slot its
own bodies, created once with fixed body ids. A character's bodies are therefore its slot's, and so are
their ids. Some physics engines order their contact solving by body id, so two peers that put the same
character in different slots can solve the same contact in a different order and drift apart by rounding.
A rollback-networked game cannot afford that: the client compares its prediction with the server's
state exactly, so a rounding difference is a misprediction.

The cure is that every peer puts a character in the same slot. Only the authority can choose it (a
client does not know which slots the authority has used, nor in which order characters joined there),
so the authority chooses and replicates the choice, and every client waits for it. `decideBodySlot` is
that rule, as a pure function a host calls when a character asks for a slot.

## §2 The rule

`decideBodySlot(isAuthority, replicatedSlot, takenLocally)` answers with a `BodySlotDecision`: an outcome
and a slot.

- **Authority.** The lowest slot not in `takenLocally` gives `BodySlotOutcome::Assigned` with that slot.
  None free gives `BodySlotOutcome::Full`. The replicated slot is not an input on the authority: its own
  table is the only truth, and the host writes the result where the clients can read it.
- **Client, nothing replicated yet** (`replicatedSlot` empty): `BodySlotOutcome::WaitForReplication`.
  The client waits even when every slot is free locally, because a free slot it picked itself could be one
  the authority gives to someone else.
- **Client, the replicated slot is still taken locally:** `BodySlotOutcome::WaitForRelease`, with the
  replicated slot. This happens when the authority has reused a slot that a character which has already
  left the authority still holds on this client, because the client has not yet heard of that departure.
  The wait ends when the departed character releases its slot on the client.
- **Client otherwise:** `BodySlotOutcome::Assigned` with the replicated slot, **even when a lower slot is
  free locally.** A client's free slots and the authority's need not agree at any moment, so a client
  never picks a slot of its own.

The slot field is meaningful for `Assigned` (the slot to bind) and `WaitForRelease` (the slot being
waited on, for the host's log). It is 0 for `WaitForReplication` and `Full`.

## §3 An out-of-range replicated slot

A replicated slot of `kMaxSimulatableSlots` or more is not a slot of this world. That can only come from
a corrupt or mismatched value on the wire, so it is an `OG_CHECK`. In a build where the check is compiled
out the function answers `BodySlotOutcome::WaitForReplication`, so the value is never used as an index
and the caller keeps waiting (a host normally bounds that wait with its own retry budget).

## §4 The inputs are plain values

`takenLocally` is a `std::array` of `kMaxSimulatableSlots` flags, not the host's table: the host builds it
from whatever it keys slots by. The replicated slot is a `std::optional` because "not replicated yet" is a
state of its own, distinct from slot 0. A host that carries the slot in one byte typically stores slot + 1
with 0 meaning none, and converts at its edge.

## §5 Guards

**None.** Each branch's behaviour is pinned by the tests (§6), so a wrong edit to a branch fails a test,
and the out-of-range check is an `OG_CHECK`. The prohibition that matters, "a client never picks a slot
itself", is about the CALLER: a host that bypasses this function, or tells it a client is the authority,
breaks it where this header cannot see. That guard belongs to the host.

## §6 What the tests show

Four cases in the physics-world test file:

- "BodySlotAssignment authority takes the lowest free slot and reports Full when none is free": spot
  values, then every one of the 256 taken sets; the replicated slot is ignored on the authority.
- "BodySlotAssignment client waits for the replicated slot before it binds": every taken set, all free
  and all taken included.
- "BodySlotAssignment client waits while the replicated slot is still held locally": every replicated
  slot against every taken set that holds it; the grid counts the witnesses where a lower slot is free, and
  asserts there are some.
- "BodySlotAssignment client is assigned the replicated slot even when a lower slot is free locally":
  every replicated slot against every taken set that leaves it free, with the same witness count. A client
  that takes the lowest free slot fails this case.

## §7 Provenance

Added 2026-10-11. The design work behind it is private initiative material and is **not distributed with
this submodule**.
