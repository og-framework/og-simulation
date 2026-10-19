<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverLog.h` — rationale

The narrative behind `HostLogLevel`, `BasicHostLine`, `HostLine` and `formatHostLine`. The header keeps
its licence, its docs pointer, its code and one guard tag; everything that explains it lives here.
Prohibitions are in `TickDriverLog-guards.md`.

**If this file and `TickDriverLog.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: STL only. Everything below is true to a reader with no game engine and no other
repository.

---

## §1 What it is for

A tick driver and its world host write host log lines (frame and step telemetry, world and contact
reports) without an engine string type. They format each line with `printf` conventions into a
`char` buffer they own and hand it to the engine's log sink with a `HostLogLevel` (`Verbose`, `Log`,
`Warning`). Some of those lines are written on the step thread, every step, where nothing may
allocate. `formatHostLine` is the one formatter for all of them:
- it **never truncates**: the whole line is formatted, whatever its length;
- it **does not allocate** while the line fits the buffer's reserved capacity, and grows the buffer
  only when a line is longer than any before it.

## §2 How it formats

`formatHostLine(out, fmt, ...)` is a two-pass `std::vsnprintf`:
1. it copies the argument list (`va_copy`), sets the line's size to its capacity (no allocation: a
   `std::basic_string` never allocates to grow its size within its capacity), and formats into it. The
   size limit passed is the size plus one, because a `std::basic_string` keeps one more element, the
   terminator, after its last character;
2. if the formatted length fits, it shrinks the size to that length, which never allocates, and
   returns;
3. otherwise it sets the size to the length (one allocation, the only one) and formats again, from the
   copy (guard G-01).

It returns `out.c_str()`, valid until the next call or the next change to `out`. A negative
`std::vsnprintf` result (an encoding error) leaves an empty line.

Cost: step 1 fills the bytes between the previous line's length and the capacity with zeros on every
call, a `memset` of at most the capacity. A C++23 `resize_and_overwrite` would avoid it; this header is
C++20.

On GCC and Clang the function carries the `printf` format attribute, so a call whose arguments do not
match its format string is a compiler warning. MSVC has no equivalent outside its static analyser.

## §3 The buffer

The buffer is a `std::basic_string` of `char` templated on its allocator, `BasicHostLine<Allocator>`;
`HostLine` is the one with the standard allocator. The caller owns it and reserves it once, at its own
construction, with `reserve`. The allocator parameter is there so a test can count allocations:
`TickDriverLog.FormatHostLine.EqualsSnprintfWithoutTruncationOrAllocation` formats 100-, 600- and
5,000-character lines into a line reserved at 1,024 characters with a counting allocator, compares each
with `std::snprintf`, and checks that only the 5,000-character line allocates.

## §4 Provenance

Written on 2026-10-16 for the og-tickdriver migration, to replace the engine string formatting the
brawler's Unreal frame host and Jolt backend host use for their host lines. Nothing in this file
depends on any document outside this submodule.
