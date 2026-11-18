<!-- SPDX-License-Identifier: MPL-2.0 -->
<!-- lint-external-ref: SimulationInputRelay.cpp -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: SimulationInputRelay.h -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: PreReplication -- Unreal Engine replication hook name, outside every scan root -->
<!-- lint-external-ref: emitFlushReport -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
# `RelayRingStager.h` — guards

Every prohibition that governs a line of `RelayRingStager.h`. Each entry has an **opaque, stable id**. In the header a
single line `// ⛔G-nn  docs/RelayRingStager-guards.md` sits exactly where the forbidden edit would be typed.

**If this file and `RelayRingStager.h` disagree, the header is authoritative and this file is stale.** Fix this file;
do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved to §R and its
number is spent forever. A retired id may be **named** in prose; it may never again appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** The narrative lives in `RelayRingStager-rationale.md`. A guard is a
prohibition plus the consequence of ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is true and
actionable to a reader with no game engine, no other repository and no other file open.

**Provenance.** The three prohibitions were carried from the Unreal host's relay, which keeps its own copies: G-01 from
`UESimulatableOwnerBinding-guards.md` G-01 (the stage write), G-02 and G-03 from the relay actor's comments in
`SimulationInputRelay.cpp` and `SimulationInputRelay.h` (old convention, not converted). **As shipped** quotes the text
each entry carries.

---

## G-01 — a stage overflow is counted, never absorbed

**Tag site:** `RelayRingStager.h`, in `stageArrival`, immediately above `if (outcome.droppedOldest)`.

**As shipped** (`UESimulatableOwnerBinding-guards.md` G-01):
> ⛔ COUNTED, NOT ABSORBED, and on the HOST -- the one input loss this side can see.

**The prohibition.** Do not delete this count, and do not add a condition to it: every outcome whose `droppedOldest`
is true adds one to `stageOverflowDrops`.

**Consequence.** An overflow means one frame staged more than `relayedInputRing::kMaxDepth` arrivals for this
character and the oldest staged entry was evicted before it was ever published. The server keeps no other record of
that lost input; without the count, `stageOverflowDrops` on the `[RelayFlush]` line reads 0 while relayed inputs are
being dropped.

**What breaks if it moves.** The count must stay on the outcome of this call: the codec reports an eviction only in
the outcome it returns, and no later state shows it.

---

## G-02 — the report window counts flushes, not published rounds

**Tag site:** `RelayRingStager.h`, in `flush`, immediately above `if (++m_flushesSinceReport >= kFlushReportRounds)`.

**As shipped** (`SimulationInputRelay.cpp`, in `PreReplication`):
> The window is counted in POLLS, not in published rounds: a host that is polled but always suppressed is exactly the
> state an operator most needs to see, and a rounds-driven window would go silent on it.

**The prohibition.** Do not make this count depend on what the flush published: do not add `published > 0` (or any
other condition) to it, do not move it into the `published > 0` branch, and do not substitute `flushedRounds` for it.
Every call of `flush` counts once, empty or not.

**Consequence.** A character that is flushed every frame but has no arrivals (a client that stopped sending, a
character nobody controls) suppresses every flush. A window counted in published rounds never reaches 120 for it and
prints no `[RelayFlush]` line, so the one state the line exists to show is the one it hides.

---

## G-03 — the window resets its own count only; the counters are session totals

**Tag site:** `RelayRingStager.h`, in `flush`, immediately above `m_flushesSinceReport = 0u;`.

**As shipped** (`SimulationInputRelay.cpp`, above `emitFlushReport`):
> SESSION TOTALS, CUMULATIVE — diff two consecutive lines for a window's delta.

**Correction (R0).** The shipped comment goes on: "three of the four counters are session FACTS", and "a single
overflow visible in exactly one line out of ~45". The line has carried five counters since `resent` was added, and the
"~45" depends on the session length; neither number is carried.

**The prohibition.** Do not widen this reset: it resets `m_flushesSinceReport` and nothing else. Do not reset
`m_counters`, or any field of it, with it.

**Consequence.** A reader gets a window's delta by subtracting two consecutive lines; per-window counters would make
that subtraction wrong. They would also show a stage overflow in a single line only, and would remove the session
facts the line carries: whether the flush ever published, and whether the stage ever overflowed.

---

## §R — retired ids

None.
