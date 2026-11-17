<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimTransport.h` — rationale

The narrative behind `SimTransport.h`, the transport port: the connection id, the lane names and the
two concepts a transport satisfies to carry sim traffic. The header keeps its licence, its docs pointer
and its code; everything that explains it lives here.

**If this file and `SimTransport.h` disagree, the header is authoritative and this file is stale.** Fix
this file; do not soften the header to match it.

`SimTransport.h` states no prohibition: every rule it has is a `static_assert`, so it has no guards
document and a one-path docs pointer.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free and
socket-free by construction (STL only: `<concepts>`, `<span>` and the fixed-width integers). Where a
transport appears below it is "the transport": whatever library moves the bytes between processes.

---

## §1 What it is

A port, in the ports-and-adapters sense. The simulation code that sends sim traffic is written against
the two concepts here. A transport layer outside `og-simulation` implements them over its sockets. The
dependency points one way: the transport layer includes this header, and no `og-simulation` file
includes a transport header. So the sim can be built, tested and shipped without any socket library,
and a test can stand in a fake transport that records what was sent.

The port is the **outbound** half only. Inbound traffic arrives through `SimInbox` (`SimInbox.h`): the
transport's receive path pushes records into it, and the sim's owner drains it. The two headers share
`SimConnectionId` and `SimLane`, which is why they live here.

## §2 `SimConnectionId` and `SimLane`

`SimConnectionId` is a `uint32_t` that names one remote process for the life of its connection. `0` is
`kInvalidSimConnection`, never a live connection, so a zero-initialised id is visibly "no connection".
The transport hands out the other values; the sim only compares and stores them.

`SimLane` names the five lanes sim traffic uses:

| lane | carries | direction |
|---|---|---|
| `Control` | reliable session messages: roster changes, the relay floor, the connection's tier | both |
| `SimClock` | the server clock publish, and clock pings and pongs | both |
| `SimInput` | a client's input bundles | client to server |
| `SimRelay` | other players' inputs, relayed by the server | server to client |
| `SimState` | state corrections | server to client |

**The values are the transport's own lane numbers.** The transport this port was written for numbers
its lanes `Control, SimClock, SimInput, SimRelay, SimState, …` from 0, with lanes after `SimState` that
carry no sim traffic. Keeping `SimLane` equal to that prefix lets the transport map a lane with a cast
and lets a log line print the same number on both sides. The header pins every value with a
`static_assert`. Only a layer that includes both this header and the transport's can check that the two
enums agree, so that check belongs in the transport layer, not here.
`kSimLaneCount` is one past the last lane, also pinned, and sizes the inbox's per-lane counters.

## §3 `ClientSimTransport`

```
t.sendInputBundle(std::span<const std::byte>) -> void
t.endInputSends()                             -> void
```

A client sends its local players' input bundles once per send point. `sendInputBundle` hands over one
bundle, already encoded by the input bundle codec. `endInputSends` closes that send: the transport may
batch every bundle handed over since the last `endInputSends` into one message per connection (a set of
bundles), so a client with several local players sends one message per send point, not one per player.
A transport that sends each bundle at once treats `endInputSends` as a no-op.

The bytes are `const`: the transport never writes them. The span is valid only for the duration of the
call, so a transport that keeps the bytes past it copies them.

## §4 `ServerSimTransport`

```
t.relay(SimConnectionId skipOwner, std::span<const std::byte>) -> void
t.sendState(SimConnectionId, std::span<const std::byte>)        -> void
t.sendControl(SimConnectionId, std::span<const std::byte>)      -> void
t.broadcastClock(std::span<const std::byte>)                    -> void
```

- `relay` sends one player's relayed inputs to every connection except `skipOwner`, the connection that
  owns that player. A client never receives its own inputs back: it predicted them.
- `sendState` sends a correction to one connection.
- `sendControl` sends one reliable session message (a roster change, the relay floor, a tier) to one
  connection.
- `broadcastClock` sends the server clock publish to every connection.

## §5 Every send returns `void`

A send is fire-and-forget. Unreliable lanes lose messages by design, and the sim already recovers from a
lost input, relay or correction through redundancy, re-sends and the next correction. A send that reports
a result invites a caller to branch on it, which would make the sim's behaviour depend on the transport's
queue state. So the concepts require `void`; the transport keeps its own counters for what it dropped.

## §6 The self-checks

Each concept has a reference shape that must satisfy it and near-misses that must not, as
namespace-scope `static_assert`s, so a change to the concept that loosens or breaks it fails the build
in this header:

| shape | rules out |
|---|---|
| `clientSimTransportSelfCheck::MissingEndInputSends` | a client transport with no end of send, which could not batch |
| `clientSimTransportSelfCheck::ReportsSendResult` | a send that returns a result (§5) |
| `clientSimTransportSelfCheck::TakesAMutableSpan` | a transport that may write the caller's bytes |
| `serverSimTransportSelfCheck::MissingSendControl` | a server transport with no reliable session messages |
| `serverSimTransportSelfCheck::MissingBroadcastClock` | a server transport with no clock publish |
| `serverSimTransportSelfCheck::RelayWithoutSkipOwner` | a relay that cannot skip the owner (§4) |
| `serverSimTransportSelfCheck::ReportsSendResult` | a send that returns a result (§5) |

The pattern is the one `TickDriverConcepts.h` uses for its engine and game concepts.

## Provenance

Written with the header on 2026-11-14. Each assertion in the header was seen to fail, with its own
message, on a poisoned copy before it was trusted. The two lane assertions fired when the lane order and
the lane count were changed. Each concept's reference-shape assertion fired when its concept was
tightened past the reference shape. Each near-miss assertion fired when its concept was loosened (a
requirement removed, a return type unchecked, the span made mutable, a one-argument `relay` accepted) so
that the near-miss satisfied it.
