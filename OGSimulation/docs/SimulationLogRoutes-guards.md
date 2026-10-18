<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationLogRoutes.h` — guards

Every prohibition that governs a line of `SimulationLogRoutes.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `SimulationLogRoutes.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `SimulationLogRoutes-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

---

**There are no live guards.** Both of the header's rules are enforced without a tag:
- no row may be shadowed by an earlier row (`allReachable`, a `static_assert` over `kSimulationLogRoutes`);
- no row's `plainLevel` may be `Warning` (`plainLevelsAreLogOrVerbose`, a `static_assert` over the same table).

Each was seen failing under a deliberate break (rationale §4). The rows themselves are pinned by the
`[TickDriver]` cases of `SimulationLogRoutesTest.cpp` (rationale §7).

No id has been issued or retired yet. The first guard this file ever needs is G-01. No guard of another
file names this file as its successor.
