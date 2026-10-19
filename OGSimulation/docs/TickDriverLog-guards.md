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

## G-01 — The second `vsnprintf` reads `retry`, the copy. Never pass it `args`.

**Tag site:** `TickDriverLog.h`, in `formatHostLine`, immediately above the second `std::vsnprintf`
call, the one that formats into the grown line.

**The prohibition.** Do not pass `args` to the second pass, and do not replace the `va_copy` with a
second `va_start` over the same list or a plain assignment of one `va_list` to another.

**The consequence.** The first `std::vsnprintf` consumes `args`. The C standard leaves a `va_list`
indeterminate after a `v…printf` call has read from it, so a second pass over `args` may format garbage
or read past the arguments. On an ABI whose `va_list` is an array type (x86-64 System V: Linux, and
macOS on Intel) the callee advances the caller's list, and the second pass then reads the arguments
after the last one. With MSVC on Windows x64 the reuse happens to format the same line, so a Windows
test does not catch it: with the second pass reading `args`,
`TickDriverLog.FormatHostLine.EqualsSnprintfWithoutTruncationOrAllocation` passed (measured
2026-10-16). Only lines longer than the reservation take the second pass, so the defect also stays
hidden on every short line.

**What breaks if it moves.** The `va_copy` must be taken before the first pass reads `args`. Deleting it
alone does not compile, because `retry` is then undeclared.
