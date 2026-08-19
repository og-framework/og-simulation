#pragma once
// SPDX-License-Identifier: MPL-2.0

// OGSim-core compiler-control switch (og-netcode-v2-input-relay item 77).
//
// Every OGSim-core header/TU (og-simulation, og-brawler, and the UE adapter
// layer that includes both) used to carry its own hardcoded
// `#pragma optimize("", off/on)` pair. That is a single design decision
// repeated 43 times by hand, so it is collected here, once, behind two macros:
//
//     OGSIM_OPTIMIZE_OFF   /   OGSIM_OPTIMIZE_ON
//
// Each call site keeps its own pair (open exactly once, close exactly once,
// same file) — this header only decides what the pair EXPANDS TO.
//
// - DEFAULT (no switch defined): expands to the MSVC `#pragma optimize("",
//   off/on)` pair — EXACTLY today's behaviour. Active in Debug/Development;
//   daily debugging (breakpoints hit, locals visible, call stack intact) is
//   untouched by this header's existence.
// - Define the compile definition `OGSIM_FORCE_OPTIMIZED=1` (e.g. on a
//   measurement build's module rules), or build UE_BUILD_TEST /
//   UE_BUILD_SHIPPING, and BOTH macros expand to nothing — the whole core
//   compiles at full command-line optimization, in every TU that includes it.
// - `#pragma optimize` is an MSVC extension, and a plain `#define` cannot
//   expand to a `#pragma` directive — MSVC's `__pragma()` form is what makes
//   that expansion possible from inside a macro. Under any other compiler
//   (e.g. the standalone CMake/Catch2 build of the header-only core) there is
//   no equivalent, and none is needed for behaviour: the macros are no-ops
//   there regardless of OGSIM_FORCE_OPTIMIZED, so the portable build never
//   sees an MSVC-only pragma.
//
// *** THE MEASUREMENT RULE — read this before quoting a resim/gate cost. ***
// No gate-family cost number (items 43, 46, 78, and anything measured on this
// hot path) is quotable unless the log or record that produced it NAMES the
// optimize setting it was taken under. A number taken under the default
// (OGSIM_OPTIMIZE_OFF active — debugger-friendly, as every archived resim
// number to date was measured) and a number taken with
// OGSIM_FORCE_OPTIMIZED=1 are not the same measurement, and are not
// comparable without saying so. Silently treating them as interchangeable is
// this initiative's signature defect — an instrument measuring the wrong
// quantity under the right name — applied to build configuration. State the
// setting the number was taken under, every time, in the record itself.

#if (defined(OGSIM_FORCE_OPTIMIZED) && OGSIM_FORCE_OPTIMIZED) \
	|| (defined(UE_BUILD_SHIPPING) && UE_BUILD_SHIPPING)       \
	|| (defined(UE_BUILD_TEST) && UE_BUILD_TEST)

	// Optimized configuration: no-op everywhere, MSVC or not.
	#define OGSIM_OPTIMIZE_OFF
	#define OGSIM_OPTIMIZE_ON

#elif defined(_MSC_VER)

	// Default configuration, MSVC: exactly today's pragma pair, expressed via
	// __pragma() so it can be produced by a macro at the call site.
	#define OGSIM_OPTIMIZE_OFF   __pragma(optimize("", off))
	#define OGSIM_OPTIMIZE_ON    __pragma(optimize("", on))

#else

	// Default configuration, non-MSVC: no `#pragma optimize` equivalent
	// exists (and standalone/portable builds do not need one) — no-op.
	#define OGSIM_OPTIMIZE_OFF
	#define OGSIM_OPTIMIZE_ON

#endif
