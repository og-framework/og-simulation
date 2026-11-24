<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverLog.h` — guards

Every prohibition that governs a line of `TickDriverLog.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `TickDriverLog.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `TickDriverLog-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

---

**There are no live guards.** The header includes `HostLog.h` and declares nothing of its own.

## §R Retired ids

⛔ **These ids are spent.** They may be named, but must never again appear as a `⛔G-nn` tag that
names this file.

### G-01 — RETIRED, moved with its site to `HostLog-guards.md` G-01

The second `vsnprintf` in `formatHostLine` reads `retry`, the copy, never `args`. On 2026-11-24
og-session-runtime task 18 moved `formatHostLine` unchanged from this header to `HostLog.h`, and the
guard went with it: its entry and its tag are now `HostLog-guards.md` G-01, under the same id.
