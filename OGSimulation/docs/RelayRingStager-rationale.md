<!-- SPDX-License-Identifier: MPL-2.0 -->
<!-- lint-external-ref: ASimulationInputRelay -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: noteStageOverflowDrop -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: SimulationInputRelay.h -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: ASimulationInputRelay::PreReplication -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: emitFlushReport -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: LogOGInputRelay -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: PreReplication -- Unreal Engine replication hook name, outside every scan root -->
<!-- lint-external-ref: m_onRingFlushedCallback -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
# `RelayRingStager.h` — rationale

The narrative behind `RelayRingStager` and `RelayRingStagerCounters`. The header keeps its licence, its docs pointer,
its code and three tags; everything that explains it lives here. Prohibitions are in `RelayRingStager-guards.md`.

**If this file and `RelayRingStager.h` disagree, the header is authoritative and this file is stale.** Fix this file;
do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free and socket-free: it includes
only og-simulation headers and the C++ standard library. The engine host it was moved from is named below as
provenance; nothing here needs it.

---

## §1 What it is

One `RelayRingStager` per relayed character, on the server. It owns two `RelayRingBuffer`s (`SimPayloadBuffers.h`):
- **the stage**, where every relayed arrival of the current frame is written (`stageArrival`);
- **the published ring**, the bytes a server sends to the other peers (`published()`).

`flush(resendEntries)` moves the stage into the published ring with `relayedInputRing::flushStagedInto` and keeps the
flush counters. A `[RelayFlush]` line reports the counters every `kFlushReportRounds` flushes and once at
`finalReport()`.

It was written as the transport-free half of the Unreal host's relay: the stage and the flush of the relay actor
(`ASimulationInputRelay`, in `OGSimulationUnreal`, deleted by og-input-latency task P5-9), and the stage write of the
owner binding's `stageRelayedInput` (deleted with the binding's net half by task P5-8). Since task P5-7 og-sim-net's
server holds one per relayed character (`SimNetServer.h`, `Character::stager`), stages into it from
`relayRemoteInput` and flushes it once per frame from `flushRelays` (§7).

## §2 Staging

`stageArrival<InputT>(captureTick, dA, input)` is `relayedInputRing::stageArrival` on the stage, unchanged:
- the stage holds up to `relayedInputRing::kMaxDepth` (8) entries. The capacity is a constant inside the codec; no
  depth is passed on this path. A `static_assert` after the class pins `stageArrival`'s signature to
  `(std::uint32_t, std::uint8_t, const InputT&)`, so a depth parameter cannot be added without failing the build;
- an arrival for a capture tick the stage already holds rewrites that entry in place;
- a new tick that finds the stage full evicts the oldest entry, if it is newer than that entry; an older tick is
  refused (`accepted` false);
- the codec's `StageArrivalOutcome` is returned as is. When `droppedOldest` is true the stager counts
  `stageOverflowDrops` (guard G-01).

An overflow means one frame carried more than `kMaxDepth` arrivals for one character. The evicted entry is never
published, and the server has no other record of that input loss: the relay sends carry no delivery signal, and the
receiving client counts its holes on its own side. The actor's comment on `noteStageOverflowDrop` said so
(`SimulationInputRelay.h`, beside `noteStageOverflowDrop`, deleted by og-input-latency task P5-9).

**Unlike the Unreal stage, the count cannot be skipped.** The binding counted an overflow on the relay actor only when
it had one (`if (outcome.droppedOldest && host != nullptr)`, until og-input-latency task P5-8 deleted it); the stager owns its
counter, so every overflow it reports is counted.

## §3 Flushing

`flush(resendEntries)`:
1. calls `relayedInputRing::flushStagedInto(published, stage, resendEntries)`, which copies the staged entries into the
   published ring behind up to `resendEntries` re-sent entries of the ring it last published (clamped to
   `relayedInputRing::kMaxResendEntries`, 2, inside the codec), empties the stage, and returns the number of staged
   entries it published;
2. counts, exactly as `ASimulationInputRelay::PreReplication` did (SimulationInputRelay.cpp:146-160 (deleted by og-input-latency task P5-9)):
   - published > 0: `flushedRounds` + 1, `entries` + published, `resent` + (the ring's entry count − published);
   - published = 0: `suppressed` + 1;
3. counts the flush toward the report window (§4);
4. returns the published count.

**An empty stage leaves the published ring byte-identical.** `flushStagedInto` returns 0 before it writes anything
when the stage is empty (`RelayedInputRingCodec.h:389-391`, that file's guard G-04). The stager adds no write of its
own. A sender that sends the ring only when `flush` returned more than 0 therefore sends nothing on a quiet frame, and
a sender that keeps sending the ring sends the same bytes. A `suppressed` count that rises is the normal idle state of
a character with no arrivals, not a fault.

The re-send count is a parameter, not a member. The actor stored it (`setRelayResendEntries`, clamped there); the
stager leaves that to its caller, which reads it from its session config, and the codec clamps it again. og-sim-net's
server reads `TimeConfig::relayResendEntries` and clamps it (`latencyKnobs::clampRelayResendEntries`) before every
`flush` (`SimNetServer::flushRelays`).

## §4 The `[RelayFlush]` line

The line, as the logger receives it:

```
[Warning][RelayFlush] <reason> owner=<debugName> flushedRounds=<u> entries=<u> entriesPerRoundX100=<u> suppressed=<u> stageOverflowDrops=<u> resent=<u>
```

- **From `[RelayFlush]` on, the text is the deleted actor's `emitFlushReport` format** (SimulationInputRelay.cpp:187-197 (deleted by og-input-latency task P5-9)),
  field for field and space for space. Log readers that search for `[RelayFlush]` and its fields find the same text.
- **`owner`** is `debugName`, given at construction. The actor printed `GetNameSafe(GetOwner())`, the character
  actor's name; the host that builds a stager passes the name it wants on the line. `kMaxDebugNameBytes` (128) bounds
  it (an `OG_CHECK` in the constructor), and a `static_assert` checks that the longest line fits `kReportLineBytes`.
- **`entriesPerRoundX100`** is `entries × 100 / flushedRounds`, computed in 64 bits, and 0 when nothing was published:
  the actor's formula. 100 means every published round carried one entry; higher values mean rounds that carried
  several arrivals.
- **The counters are session totals,** never reset by the window (guard G-03). Two consecutive lines give a window's
  delta by subtraction. A per-window reset would show a stage overflow in one line only, and the line would no longer
  say whether the flush ever ran or the stage ever overflowed in the session.
- **`reason`** is `window` for the periodic line and `teardown` for `finalReport()`. The actor's teardown line said
  `FINAL` (SimulationInputRelay.cpp:213 (deleted by og-input-latency task P5-9)); the task that specified this class named the reason `teardown`. No
  og-simulation file reads the reason.
- **The window is counted in flushes, not in published rounds** (guard G-02). A line is emitted on every
  `kFlushReportRounds`-th (120th) call of `flush`, empty ones included. A character that is flushed but never has
  arrivals is the state an operator most needs to see: a window counted in published rounds would never print for it.
  At 60 flushes per second (one per server network update at 60 Hz) a window is 2 s per character.
- **`finalReport()`** prints the line once more with `teardown`, whatever the window count. A session shorter than one
  window, and the flushes after the last window, are then still reported. The stager does not know its role: the
  actor printed its final line on the authority only (`HasAuthority()` in `EndPlay`); a host creates stagers only
  where it relays, on the server.
- **No logger, no line.** With an empty `Logger` the stager stages, flushes and counts, and prints nothing.

**Route and level.** The line begins with the `[Warning]` token, so a host that reads severity tokens prints it at
`Warning`. The row `{"[RelayFlush]", Net, Log}` in `SimulationLogRoutes.h` gives it the `Net` route. The row's
`plainLevel` is `Log`, not `Warning`: `plainLevelsAreLogOrVerbose` refuses a `Warning` row at compile time, and a line
that must reach `Warning` carries the token (`SimulationLogRoutes-rationale.md` §3). The task asked for the route
"Net, Warning"; the token and the row together give exactly that.

The level is the actor's: it logged the line at `Warning`, until og-input-latency task P5-9 deleted it. In the Unreal host, `RouteOGMessage` maps the `Net` route
to `LogOGNet` (`UESimulationLogRoutes.h:300`), and the game's engine config sets that category to `Warning`
(`LogOGNet=Warning`), so a line without the token would not be written. The line moved from `LogOGInputRelay` to
`LogOGNet` when og-sim-net's server bound this class (og-input-latency task P5-7; its host's `log` routes through
`RouteOGMessage`), and `RouteOGMessage` prints the whole string, token included (`UESimulationLogRoutes.h:281`).

**Volume.** Nothing is logged per stage or per flush; only the windowed line and the final one. A per-flush line would
be a line per character per frame.

## §5 Frame-locked and immediate modes

The stager does not know the session's send mode. A frame-locked caller stages every arrival of a frame and flushes
once per frame, as the actor did once per replication update. An immediate caller flushes after each receipt, so each
flush carries what arrived in one receipt. Both call the same two members. In immediate mode a window is 120 receipts,
not 120 frames, so its time span depends on the arrival rate.

## §6 Threads

One thread: the caller's. `stageArrival`, `flush`, `finalReport` and the getters must all be called from the context
that owns the stager; it has no atomics and no lock. The logger is called from inside `flush` and `finalReport`, on
that thread.

## §7 Equivalence and what is not here

**EQUIV, at the time of P2-5.** No product code included the header then; the actor's flush, its counters and its line
stayed the shipped path. Since og-input-latency task P5-7 og-sim-net's server (`SimNetServer.h`) includes it and is
the shipped path, and task P5-9 deleted the actor.

**Moved behaviour, not moved code.** The actor's own code and comments stayed as they were (old comment convention)
until P5-9 deleted them. What this class carries over:
- the flush call and its four counters (`PreReplication`);
- the overflow count (`stageRelayedInput`'s guard, G-01 of `UESimulatableOwnerBinding-guards.md`, retired with it by
  og-input-latency task P5-8);
- the line's format and its two reasons (`emitFlushReport`, `EndPlay`), with the reason renamed as §4 says;
- `kFlushReportRounds = 120` (SimulationInputRelay.h:301 (deleted by og-input-latency task P5-9)).

**Not carried over:**
- the flush callback (`m_onRingFlushedCallback`): the caller gets the published count from `flush` and reads the ring
  with `published()`;
- the replication setup (priorities, owner skip, update frequency): a transport's concern;
- the client side (ring arrival, unrouted counts, listener link): the receiver stores rings in its own buffer.

## §8 Compile-time checks

| check | what it replaces |
|---|---|
| `static_assert` on `&RelayRingStager::stageArrival<…>`'s type | the "no depth parameter" fence; the same check sat on the Unreal binding's `stageRelayedInput` until og-input-latency task P5-8 deleted it |
| `static_assert(kReportLineBytes >= …)` | a length bound the actor's line did not need (an `FString`) |
| `OG_CHECK(debugName.size() <= kMaxDebugNameBytes)` | the run-time half of the same bound |

The signature check takes the address of `stageArrival<SignatureProbeInput>` inside `decltype`, which does not
instantiate the body; `SignatureProbeInput` has no serialization fields and would not compile as a staged input.

## §9 Tests

`Network/RelayRingStagerTest.cpp` in og-simulation-tests (tag `[Network]`):
- `ScriptedStagingMatchesTheCodecReferenceAtEveryFlush`: for r = 0, 1 and 2, 250 flushes of a cycled script (bursts of
  12 and 17 over `kMaxDepth`, empty frames, a refused old tick, a same-tick rewrite). A reference drives the codec
  functions directly and counts as `PreReplication` did. After every flush the published bytes, the counters and the
  lines equal the reference's, and an empty flush leaves the bytes unchanged;
- `WindowLineOnFlush120And240AndTeardownFromFinalReport`: the window line appears on flush 120 and 240 and nowhere else,
  with the expected fields; `finalReport` prints `teardown`;
- `EmptyFramesCountTowardTheWindow`: one published round then 119 empty flushes, and 120 empty flushes, each print a
  line at flush 120;
- `StageOverflowIsCountedAndTheWindowKeepsSessionTotals`: 12 arrivals in one frame count 4 overflows; two windows show
  cumulative totals;
- `RouteTableResolvesRelayFlushToNetAtWarning`: the line carries the `[Warning]` token, and its body takes the
  `[RelayFlush]` row, route `Net`;
- `WithoutALoggerFlushesAndReportsNothing`.

## §10 Provenance

og-input-latency P2-5 (2026-11-14): the relay stager moves out of the Unreal actor into og-simulation. The task text
cited the actor's flush as SimulationInputRelay.cpp:142-190 (deleted by og-input-latency task P5-9); on the tree of that date `PreReplication` is 142-170 and
`emitFlushReport` 177-198.
