<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationLogRoutes.h` — rationale

The narrative behind `SimulationLogRoute`, `SimulationLogRouteEntry`, `kSimulationLogRoutes` and the
`simulationLogRoutes` checks. The header keeps its licence, its docs pointer and its code; everything that
explains it lives here. Prohibitions are in `SimulationLogRoutes-guards.md` (there are none: both of
the header's rules are `static_assert`s).

**If this file and `SimulationLogRoutes.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: plain `char` prefixes, `HostLogLevel` from `TickDriverLog.h`, no engine type. Everything
below is true to a reader with no game engine and no other repository.

---

## §1 What it is for

og-simulation writes its diagnostic lines as plain strings through the `SIMLOG` and `SIMLOG_G` sinks
(`SimulationLog.h`). Each line starts with a tag such as `[CollectInput]` or `[Resim.Post]`, optionally
behind a severity token, `[Verbose]` or `[Warning]`. The core does not decide which log channel a line
goes to or at what level a token-less line is printed: the host's sink does. A host does that with a
first-match table of prefixes.

`kSimulationLogRoutes` is the part of that table that belongs to the core: **one row per tag that
og-simulation emits and no host emits.** A row is:
- `prefix`: the start of the tag. A host compares it with the line after the severity token,
  ignoring case;
- `route`: the channel the line belongs to (§2);
- `plainLevel`: the level of a line of that row that carries no severity token (§3).

A host puts these rows into its own table, ahead of or behind its own rows, and maps each
`SimulationLogRoute` to one of its log channels and each `HostLogLevel` to one of its levels. The host's
rows are the tags only it emits, the tags both it and the core emit, and rows it keeps for tags nothing
emits. Which tags those are is the host's business; this header names only the core's.

## §2 The routes

| route | what goes there |
|---|---|
| `SimTick` | per-tick simulation lines: the authority and prediction steps, input collection, resim checks per tick, the per-character resim input table |
| `Sim` | rare simulation lifecycle lines: resim prepare and finish, time resyncs, divergence and restore events |
| `Net` | replication lines: input and correction sends and receipts, the server's reception coordinator (park, release, gaps, drops, delay shifts, stats) |
| `DivergenceProbe` | the correction-verdict probe family: every `[DivergenceProbe.*]` line |

A host may send two routes to one channel. Each probe family has its own route so a host can switch it on
or off without touching any other line.

## §3 Levels — `plainLevel`

A line's level is decided in this order: a leading `[Verbose]` or `[Warning]` token wins; otherwise the
matching row's `plainLevel` applies. So the rows whose lines all carry a token (`[InputGap]`, `[InputDrop]`,
`[DelayShift]`, `[InputStats]`, `[InputDomain]`, `[RelaySkip]`, `[DivergenceProbe`, and `[Resim.Input]`,
which carries `[Verbose]`) have a `plainLevel` that applies to no line today.

The rule that classified the rows: a row whose token-less lines arrive per tick, per packet or per relay
is `Verbose`; a row for rare events is `Log`. `plainLevelsAreLogOrVerbose` admits only those two values. A
line that must reach `Warning` says so with its own `[Warning]` token, so a `Warning` row could only
hide that choice from the emitting site.

`[Resim.Input]` is `Verbose` although its lines carry a token: it is a per-replayed-tick line, and a future
token-less line there should not be printed by default. The token rows are `Log` for the opposite reason:
a future token-less line in a warning family should not be hidden by accident.

## §4 Order and the two checks

A host takes the **first** row whose prefix begins the line. Two pairs of rows overlap, and in each the
more specific row must come first:
- `[Resim.Input]` ahead of the `[Resim.` catch-all. `[Resim.Input]` is a per-character, per-resim-tick line
  (the volume of `[CollectInput]`); below the catch-all it would inherit the rare-lifecycle `Sim` route and
  the `Log` level, and the highest-frequency line of a resim would print by default;
- `[ReceiveLocalInput] DISCARD` ahead of `[ReceiveLocalInput]`. Both are emitted from one receive callback
  (`SimulationNetSync.h`): the first is a rare anomaly (`Log`), the second is per input (`Verbose`).

`allReachable(rows)` returns false if any row's prefix begins with an earlier row's prefix, compared
ignoring case with `startsWithIgnoringCase`; such a later row could never be taken. `plainLevelsAreLogOrVerbose(rows)`
returns false for a row at `Warning`. The header `static_assert`s both over `kSimulationLogRoutes`. Both are
templates over a row array, so a test can show each one rejecting a bad table without breaking the build.

Seen failing (a copy of the header with the change, compiled in a standalone CMake build of og-simulation
and the test; 2026-10-16):

| change | result |
|---|---|
| `[Resim.` moved above `[Resim.Input]` | `error C2338: static_assert failed: 'kSimulationLogRoutes: a route's prefix BEGINS WITH an earlier route's prefix, …'` |
| the `[Park]` row's `plainLevel` set to `Warning` | `error C2338: static_assert failed: 'kSimulationLogRoutes: a row's plainLevel is the level of a line that carries no [Verbose] or [Warning] token, …'` |

`[ResimProbe` is not a core row (§5), but the reason it is safe beside `[Resim.` holds here too:
`[Resim.` needs the dot, so it never takes a `[ResimProbe.*]` line. Widening the catch-all to `[Resim`
would take every `[ResimCheck.` row's lines and fail `allReachable`.

## §5 The rows and their emitters

Each row's tag is emitted by these core files and by no host (a search of every string literal in the
core, with the severity token removed, 2026-10-16):

| row | route | `plainLevel` | emitting file |
|---|---|---|---|
| `[Resim.Input]` | `SimTick` | `Verbose` | `InputResolutionTelemetry.h` |
| `[TimeResync.` | `Sim` | `Log` | `SimulationInputResolution.h`, `SimulationManager.h`, `SimulationReconciliation.h` |
| `[Resim.` | `Sim` | `Log` | `SimulationManager.h`, `SimulationReconciliation.h` |
| `[ResimCheck.Divergence]` | `Sim` | `Log` | `SimulationManager.h` |
| `[ResimCheck.PrepareRestore]` | `Sim` | `Log` | `SimulationReconciliation.h` |
| `[ResimCheck.Check]` | `SimTick` | `Verbose` | `SimulationReconciliation.h` |
| `[ResimCheck.IsSimilar]` | `SimTick` | `Verbose` | `SimulationManager.h` |
| `[AuthoritySimulation]` | `SimTick` | `Verbose` | `SimulationManager.h` |
| `[PredictionSimulation]` | `SimTick` | `Verbose` | `SimulationManager.h` |
| `[PostPrediction]` | `SimTick` | `Verbose` | `SimulationReconciliation.h` |
| `[CollectInput]` | `SimTick` | `Verbose` | `InputResolutionTelemetry.h` |
| `[ReceiveLocalInput] DISCARD` | `Net` | `Log` | `SimulationNetSync.h` |
| `[ReceiveLocalInput]` | `Net` | `Verbose` | `SimulationNetSync.h` |
| `[SendCorrectionStateToClients]` | `Net` | `Verbose` | `SimulationNetSync.h` |
| `[SendLocalInputToServer]` | `Net` | `Verbose` | `SimulationNetSync.h` |
| `[InjectCorrectionState]` | `Net` | `Verbose` | `SimulationReconciliation.h` |
| `[InputGap]` | `Net` | `Log` | `Network/ServerReceptionCoordinator.h` |
| `[InputDrop]` | `Net` | `Log` | `Network/ServerReceptionCoordinator.h` |
| `[DelayShift]` | `Net` | `Log` | `Network/ServerReceptionCoordinator.h` |
| `[InputStats]` | `Net` | `Log` | `Network/ServerReceptionCoordinator.h` |
| `[Park]` | `Net` | `Verbose` | `Network/ServerReceptionCoordinator.h` |
| `[Release]` | `Net` | `Verbose` | `Network/ServerReceptionCoordinator.h` |
| `[InputDomain]` | `Net` | `Log` | `Network/ServerReceptionCoordinator.h` |
| `[RelaySkip]` | `Net` | `Log` | `Network/ServerReceptionCoordinator.h` |
| `[DivergenceProbe` | `DivergenceProbe` | `Log` | `NetSyncTelemetry.h` |

**Not here, and why.** Three tag families are emitted by the core **and** by the brawler game's adapter:
`[ResimProbe`, `[ServerReceive]` and `[RelayProbe`. Their rows stay in the host's table, so the host keeps
one row for lines from both sources. The `[SendRemoteInputToClients]` row also stays with the host: the
core's write of that line is retired (the absence marker in `SimulationNetSync.h`), and the tag has no
emitter.

The search cannot see a tag assembled at run time (a `"%s"` that prints a tag); none is known.

## §6 What a host must keep

- **Its own rows must not overlap these.** If a host prefix and a core prefix begin with one another, one
  line can match a row in each part of the host's table, and the order of the two parts then decides its
  route. The brawler game's adapter checks this with a `static_assert` over its concatenated table.
- **Matching ignores case** and is done against the line after the severity token. The checks here fold
  ASCII `A`–`Z` only; every prefix is printable ASCII.
- **A new tag in the core gets a row here**, in the place that keeps `allReachable` true, with a
  `plainLevel` by the rule of §3. A tag without a row falls to the host's fallback channel.

## §7 Tests

`TickDriver/SimulationLogRoutesTest.cpp` (tag `[TickDriver]`):
- `SimulationLogRoutes.Rows.PrefixRouteAndPlainLevelArePinnedInOrder` pins all 25 rows, in order;
- `SimulationLogRoutes.Checks.HoldForTheTableAndRejectAShadowedRowOrAWarningLevel` checks both functions on
  the table and on four bad tables (a catch-all above its specific row, `DISCARD` below its catch-all, a
  lower-case duplicate, a `Warning` row) and one good one;
- `SimulationLogRoutes.FirstMatch.TakesTheSpecificRowIgnoresCaseAndLeavesHostTagsUnrouted` resolves sample
  lines by first match: the specific rows win, case is ignored, and the host's tags (`[ResimProbe.Gate]`,
  `[ServerReceive]`, `[ReleaseBatch]`, …) match no core row.

## §8 Provenance

og-tickdriver task 12 (2026-10-16). The rows, their order, routes and levels, and both checks' messages
were moved from the brawler game's adapter table, where all 39 rows of the host's table were kept together.
The adapter now puts this block first and its own 14 rows after it; the routing of every line is unchanged
(the adapter's rationale records the proof).
