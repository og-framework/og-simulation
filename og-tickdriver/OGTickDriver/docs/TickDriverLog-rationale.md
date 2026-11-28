<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverLog.h` — rationale

The narrative behind `TickDriverLog.h`. The header keeps its licence, its docs pointer and one include;
everything that explains it lives here. Its retired guard id is in `TickDriverLog-guards.md`.

**If this file and `TickDriverLog.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction. Everything below is true to a reader with no game engine and no other repository.

---

## §1 What it is for

`TickDriverLog.h` includes `HostLog.h` and declares nothing of its own. `HostLogLevel`, `BasicHostLine`,
`HostLine` and `formatHostLine` are declared in `HostLog.h`, and their narrative and guard are in
`HostLog-rationale.md` and `HostLog-guards.md`. Every file that includes this header sees the same
declarations as before, so none of them had to change.

## §2 Provenance

Written on 2026-10-16 for the og-tickdriver migration, holding `HostLogLevel`, `BasicHostLine`,
`HostLine` and `formatHostLine` and guard G-01. On 2026-11-24 og-session-runtime task 18 moved all four,
unchanged, to `HostLog.h` (with G-01, now `HostLog-guards.md` G-01), because og-simulation headers outside
the tick driver use them: they now include `HostLog.h` instead of a tick-driver header. Nothing in this
file depends on any document outside this submodule.
