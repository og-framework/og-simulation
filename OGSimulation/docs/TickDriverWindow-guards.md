<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverWindow.h` — guards

Every prohibition that governs a line of `TickDriverWindow.h` or `TickDriverWindow.cpp`. Each entry has
an **opaque, stable id**. In the code a single line `// ⛔G-nn` sits exactly where the forbidden edit
would be typed.

**If this file and `TickDriverWindow.h` or `TickDriverWindow.cpp` disagree, the code is authoritative
and this file is stale.** Fix this file; do not soften the code to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `TickDriverWindow-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

---

**There are no live guards.** Each rule this file's code carries is enforced without a tag:
- the eight series are pinned by a `static_assert` in `tickDriverWindow`;
- the first-step line, the only text a step thread formats, is a separate class with no reference to a
  window, so it cannot reach window state (rationale §6);
- the format strings, the decimals of each field, the `-` for an empty series, the pre-step role rule,
  the window deltas, the nearest-rank percentiles, the overflow count and the reservations are pinned by
  the `[TickDriverWindow]` cases, each seen failing under a deliberate break (rationale §3–§5).

No id has been issued or retired yet. The first guard this file ever needs is G-01. No guard of another
file names this file as its successor.
