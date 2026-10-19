<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverConcepts.h` — guards

Every prohibition that governs a line of `TickDriverConcepts.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `TickDriverConcepts.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `TickDriverConcepts-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

---

**There are no live guards.** Every rule this header carries is a concept requirement checked by a
`static_assert` in one of its self-check namespaces, so the compiler enforces it and it needs no tag.
`TickDriverConcepts-rationale.md` §6 lists what each self-check pins.

No id has been issued or retired yet. The first guard this header ever needs is G-01.
