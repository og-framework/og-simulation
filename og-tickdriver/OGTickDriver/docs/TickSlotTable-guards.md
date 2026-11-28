<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickSlotTable.h` — guards

Every prohibition that governs a line of `TickSlotTable.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `TickSlotTable.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `TickSlotTable-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

**Predecessor.** G-01 and G-02 together succeed G-05 of the brawler game's Jolt backend host guards
(TickDriverBackendUImpl-guards.md, then in the game module; OGSimulationUnreal's since og-tickdriver task 15e), "a client never picks a Jolt slot itself: it binds the
slot the authority chose, or waits". That guard had one tag, at its decision call; its "do not turn a
wait into a pick" edit is typed on another statement, so here it has its own id and tag (G-02). The
game retires its G-05 when its slot acquire is replaced by this table, naming G-01 and G-02.

---

## G-01 — a client never picks a slot itself: the decision is `decideBodySlot`'s, from the caller's role and replicated slot

**Tag site:** `TickSlotTable.h`, in `acquire`, immediately above
`const BodySlotDecision decision = decideBodySlot(isAuthority, replicatedSlot, taken);`.

**The prohibition.** ⛔ Do not bypass `decideBodySlot`, and do not pass it anything but the caller's
`isAuthority` and `replicatedSlot`: in particular not `true` for `isAuthority`, and not a slot this
table computes in place of `replicatedSlot` when none has been replicated. Never fall back to the
lowest free slot, to a slot derived from the owner key, or to any other slot this peer computes.

**The consequence.** A slot's bodies have fixed physics body ids, so a character's body ids are its
slot's. A client that picks its own slot puts the character in a different slot from the server
whenever the two have seen joins and leaves in a different order: a client that joins late binds its
own pawn first and takes slot 0, while the server gave it the slot after everyone already there. A
physics engine that orders contact solving by body id then solves a contact between two characters
in a different order on the client than on the server, the results differ by rounding, and an exact
correction compare reports a disagreement the netcode did not cause. Nothing crashes, and this table
logs nothing different: its `assigned role=Client` line shows the wrong slot only beside the
server's line for the same owner.

**What breaks if it moves.** Both substitutions are caught by tests:
`TickSlotTable.Differential.EqualsTheLegacyJoltSlotAcquireOverRandomOrders` (the slots differ from the
reference model's) and `TickSlotTable.Client.NeverAssignedWithoutTheReplicatedSlot`. A caller that
passes the wrong role is outside this header: it is typed where the caller computes `isAuthority`.

---

## G-02 — a wait returns no slot

**Tag site:** `TickSlotTable.h`, in `acquire`, immediately above the `return std::nullopt;` that ends
the branch for `BodySlotOutcome::WaitForReplication` and `BodySlotOutcome::WaitForRelease`.

**The prohibition.** ⛔ Do not turn either wait into a pick. When `decideBodySlot` answers
`BodySlotOutcome::WaitForReplication` (the authority's slot has not arrived) or
`BodySlotOutcome::WaitForRelease` (the replicated slot is still held here by an owner that has left the
authority), return no slot and let the caller retry. Do not return `decision.slot`, the lowest free
slot, or any other slot from this branch, and do not record a holder in it.

**The consequence.** The same as G-01's: the client binds a slot the authority did not choose for this
owner, and two peers solve the same contacts in a different order. For `WaitForRelease` it is worse:
`decision.slot` is held by another owner here, so two owners would share one slot's bodies.

**What breaks if it moves.** Returning `decision.slot` here fails
`TickSlotTable.Differential.EqualsTheLegacyJoltSlotAcquireOverRandomOrders` and
`TickSlotTable.Lines.MatchTheGoldens` (measured 2026-10-16). A caller that does not wait on the empty
answer, and binds some slot anyway, is outside this header.
