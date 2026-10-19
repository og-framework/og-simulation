<!-- SPDX-License-Identifier: MPL-2.0 -->
# `PhysicsEngineTraits.h` — guards

Every prohibition that governs a line of `PhysicsEngineTraits.h`. Each entry has an **opaque, stable id**. In the
source a single line `// ⛔G-nn  docs/PhysicsEngineTraits-guards.md` sits directly above the statement where the
forbidden edit would be typed.

**There is no live entry.** What an engine's traits and a game's binding must provide are the two concepts, and the
self-check `static_assert`s pin that each part of them can fail. Why each requirement is what it is lives in
`PhysicsEngineTraits-rationale.md`.

**If this file and the source disagree, the source is authoritative and this file is stale.** Fix this file; do not
soften the source to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time or run-time check, moves to §R
and its number is spent forever. A retired id may be **named** in prose; it may never again appear as a `⛔G-nn`
tag.

---

## R. Retired ids

None.
