<!-- SPDX-License-Identifier: MPL-2.0 -->
# `BodySlotOccupancy.h` — rationale

Which character slots are live at a given sim tick. The header holds the licence, a docs pointer and
the code; this file carries the why.

**If this file and `BodySlotOccupancy.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** It names no physics
engine and no game engine.

---

## 1. What it is

`BodySlotOccupancy` is one bit per character slot, `kMaxSimulatableSlots` (8) bits in all. A set bit
means the slot holds a live character at that tick; a clear bit means the slot is parked.

It is **og-simulation state, not physics-engine state.** A physics backend whose snapshot does not
restore its body set (`PhysicsWorldAdapter.h` :: `SnapshotCoverage`, `bodySetAndIds`) keeps one body
per slot in its world at all times and is told, through `applyOccupancy`, which of those bodies are
live. The caller (the step driver) is meant to keep the occupancy per sim tick and re-apply it after
every restore and before every step, so that a rollback across a join or a leave sees the slot as it
was at that tick. This header does not enforce that; it only defines the record.

## 2. Why 8

Eight matches the spawn-point count of the game this core was first built for (that game's own
code is not part of this submodule). The record is eight bits of information per tick; its in-memory
size is whatever `std::bitset` uses on the platform, which is larger than one byte on common standard
libraries. Raising the count is a deliberate change: a backend that keeps a fixed body set allocates
one slot body per slot, so the count also sizes that body set. The test suite pins the value at 8, so
a change is noticed.

`kMaxSimulatableSlots` is a plain constant, not a template parameter, so the occupancy type is the
same for every world and can be stored in a tick-keyed timeline without templating the timeline.

## 3. Equality

`operator==` is defaulted, so two occupancies compare by their bits. Callers and tests can
compare occupancies directly instead of comparing the `std::bitset` members by hand.

## 4. Guards

**None.** The header holds a constant and a one-member struct; there is no line in it where a wrong
edit would be typed that the compiler or the test suite does not already catch.

## 5. Provenance

Added 2026-10-06 together with `PhysicsWorldAdapter.h`. The design work behind it is private
initiative material and is **not distributed with this submodule**.
