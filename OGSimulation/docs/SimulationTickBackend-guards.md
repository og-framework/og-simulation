<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationTickBackend.h` — guards

Every prohibition that governs a line of `SimulationTickBackend.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `SimulationTickBackend.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `SimulationTickBackend-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

**There are no live entries.** Every ordering rule of the header is held by the compiler or a run-time check, so none
needs a tag (rationale §3, §4): the member order (render source < presenter < driver) is a `static_assert` in the
constructor; the presenter built before the driver, a door reached before `start`, and a registration whose owner holds
no slot are `OG_CHECK`s; the world host's members and `build` are the two concepts. The rest of the order (BL1, the
stop before teardown, the release's occupancy push, the restore note to the driver) is pinned by the tests in rationale
§6, each seen failing, and the two halves an adapter types around the calls (its tick functions stopped before
`stop()`, its world host outliving the backend) are typed in the adapter, where its own guards cover them.

---

## Retired

None.
