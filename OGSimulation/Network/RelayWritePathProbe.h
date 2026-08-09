#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// ---------------------------------------------------------------------------
// RelayWriteProbe / ConnectionBudgetProbe — the SERVER-side relay telemetry
// T22 needs, and the two numbers nobody has ever measured.
// (og-netcode-v2-input-relay T22. Companion to Network/RelayReadProbe.h, which
// holds the CLIENT-side arrival/read probes plus ServerFrameProbe.)
//
// ---------------------------------------------------------------------------
// WHY A SEPARATE FILE. This is a DIAGNOSTIC UNIT with a stated end date. T22 is a
// measurement task, not a fix: if its result is "the mechanism is elsewhere",
// deleting this header, its two call sites and its test file removes the whole
// instrument in one reviewable change. RelayReadProbe.h is shipped telemetry that
// three later tasks read; mixing a disposable probe into it would make that
// separation a diff-archaeology exercise later. Same STL-only, no-logging,
// caller-owns-the-window conventions as that file — see its banner.
//
// ---------------------------------------------------------------------------
// ⭐ THE GAP IN THE ELIMINATION CHAIN THIS EXISTS TO CLOSE.
//
// `RelayDepthCoverageHypothesis.md` §9.11a eliminates every upstream stage and
// concludes "the loss is in Iris's send path". Its elimination table has a hole,
// and it is a precise one:
//
//   | stage                        | evidence cited                     |
//   | server writes the ring       | writes on every accepted receipt,  |
//   |                              | and receipts are complete          |
//   | server frame rate            | 1.003 sim ticks/frame, p99 = 1     |
//
// Both statements are TRUE and neither covers this case. The ring is written from
// the RPC RECEIPT path (`ServerReceptionCoordinator::receiveRemoteInput`'s relay
// tap -> `relayRemoteInput` -> `writeLatest`), on the game thread, once per
// genuinely-new capture tick. It is NOT written from the sim tick. So:
//
//   * "receipts are complete" says no capture tick is LOST on the wire into the
//     server. It says nothing about how many of them land in the SAME game-thread
//     frame.
//   * "1.003 sim ticks per frame" measures the SIM's pacing. The relay writes are
//     paced by PACKET ARRIVAL, which is a different clock with different jitter.
//
// And the ring ships at `relayRedundancyDepthTicks = 1` — REPLACE-LATEST. Iris
// polls a replicated property ONCE PER SERVER GAME-THREAD FRAME and compares the
// live value against its shadow copy (T20 §4.4 for the cadence, §4.5 for the
// compare-against-latest semantics: "three writes produce one compare against the
// third"). Therefore:
//
//   ⇒ IF TWO RELAY WRITES LAND IN ONE GAME-THREAD FRAME, THE FIRST ONE IS
//     UNOBSERVABLE. It is not dropped by the network, not deferred by
//     FReplicationWriter, and not visible to any client-side probe. It is
//     overwritten in server memory before replication ever looks at it.
//
// That loss is INDISTINGUISHABLE, from the client, from a send-path drop: the
// client sees a relay ring whose newest capture tick advanced by more than 1.
// `RelayArrivalProbe` reports it as `gapCaptureTicks > 1`. Everything §9.11
// measured is consistent with either cause.
//
// THE ARITHMETIC THAT MAKES THIS TESTABLE. Over a window, writes arrive at the
// capture rate (60/s, since client->server receipt is measured complete) and are
// observable at most once per WRITING FRAME. So
//
//     observable fraction  <=  writingFrames / totalWrites
//                           =  1 / (mean writes per writing frame)
//
// The §9.11 clean-run baseline measured a delivered fraction of 59.3 %, i.e. a
// mean arrival gap of 60/35.6 = 1.685 capture ticks. If write coalescing is the
// mechanism, THIS PROBE MUST REPORT A MEAN OF ~1.69 WRITES PER WRITING FRAME AND A
// RUN-LENGTH HISTOGRAM THAT MATCHES THE CLIENT'S GAP HISTOGRAM SHAPE FOR SHAPE
// (p50 = 1, p99 = 3-7, max = 8). If instead it reports ~1.00, coalescing
// contributes nothing and the loss really is downstream — which is the outcome
// that keeps the send-path candidates alive.
//
// BOTH OUTCOMES ARE INTERPRETABLE IN ADVANCE, WHICH IS THE POINT. The interpretation
// table lives in `impl/pie_script_t22.md`; it is written before the run, not after.
//
// ---------------------------------------------------------------------------
// ⭐ [og-netcode-v2-input-relay T34] THE ANSWER CAME BACK "COALESCING", AND THE
// MECHANISM ABOVE HAS SINCE BEEN REMOVED. Read everything above as the QUESTION
// this probe was built to settle, not as a description of the shipped write path.
//
// The measurement said the write path loses ~11.6 % of relayed inputs to
// same-frame coalescing (observability ~884 per mille). Item 34 replaced
// replace-latest with BARE C1 FLUSH-ON-POLL: arrivals are staged and the whole
// stage is published once per Iris poll, so a second write in a frame no longer
// overwrites the first — it is published beside it (`relayedInputRing::
// stageArrival` / `flushStagedInto`). The paragraph beginning "IF TWO RELAY WRITES
// LAND IN ONE GAME-THREAD FRAME" is therefore HISTORICAL: it is true of the path
// this probe measured, and false of the path that ships.
//
// THE PROBE SURVIVES BECAUSE THE QUESTION DID NOT GO AWAY, IT SHRANK. A burst
// LONGER than the stage capacity still loses its oldest entries, and that ceiling
// is what `observableX1000` now reports (see its field comment). The probe is also
// item 34's own acceptance instrument: its pass condition is
// `observableX1000 >= 990`. It is no longer disposable in the sense the WHY A
// SEPARATE FILE note above intended.
//
// ---------------------------------------------------------------------------
// WHAT THIS DELIBERATELY DOES NOT CLAIM. A run length of N means N writes shared a
// frame. It does NOT prove the N-1 older ones would otherwise have arrived — a
// send-path drop could be stacked on top of coalescing, and the two are additive.
// The probe reports the CEILING on observability that the write pattern imposes;
// comparing that ceiling against the client's measured arrival rate is what
// separates "coalescing explains all of it" from "coalescing explains part of it".
//
// ---------------------------------------------------------------------------
// INSTRUMENTATION ONLY, and GAME THREAD ONLY (server). Neither object holds a
// lock, neither logs, neither feeds back into any simulated value. Removing this
// header changes no behaviour.
//
// NAMESPACE NOTE: global namespace, matching the rest of the OGSim core.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PROBE 5 — RELAY WRITES PER SERVER GAME-THREAD FRAME.
// ---------------------------------------------------------------------------

// Exact buckets for run lengths 1..16; anything longer saturates. A run length is
// the number of relay writes for ONE owner inside ONE game-thread frame, so 0 is
// not a possible observation (a run only exists because a write created it) and
// the histogram starts at 1 — the same convention RelayArrivalProbe's gap
// histogram uses, deliberately, so the two are read side by side without an
// index shift. 16 is well past the §9.11 max gap of 8.
inline constexpr std::uint32_t kRelayWriteMaxTrackedRun = 16u;
inline constexpr std::size_t   kRelayWriteOverflowBucket =
    static_cast<std::size_t>(kRelayWriteMaxTrackedRun) + 1u;

// One summary per this many COMPLETED RUNS (writing frames), per owner. Driven by
// runs and not by writes so the sample count means the same thing as
// RelayArrivalProbe's does: 120 observable events. At the predicted ~35 writing
// frames/s that is ~3.4 s; at a healthy 60 it is ~2 s.
inline constexpr std::uint32_t kRelayWriteProbeWindowRuns = 120u;

// A capture-tick jump larger than this is a DISCONTINUITY (registration, a client
// re-join, the tick domain being re-established), not a coverage hole. Counted
// separately and never allowed into `missedCaptureTicks`, where a five-digit
// outlier would swamp every real number — the same guard, for the same reason, as
// ServerFrameProbe's `kServerFrameDiscontinuityTicks`.
inline constexpr std::uint32_t kRelayWriteDiscontinuityTicks = 600u;

struct RelayWriteWindowSummary
{
    // --- identity -----------------------------------------------------------
    unsigned int ownerId = 0u;

    // --- THE HEADLINE: writes per writing frame -----------------------------
    // `runs` is the number of game-thread frames in which this owner's ring was
    // written at least once. `writes` is the number of relay writes those frames
    // contained. Their ratio is the coalescing factor.
    std::uint32_t runs         = 0u;
    std::uint32_t writes       = 0u;
    std::uint32_t p50          = 0u;
    std::uint32_t p99          = 0u;
    std::uint32_t maxRun       = 0u;
    bool          p99Saturated = false;

    // --- THE CAPTURE-TICK DENOMINATOR ---------------------------------------
    // The span of capture ticks this window covers, inclusive:
    // `lastCaptureTick - firstCaptureTick + 1`. This is the number of capture
    // ticks the sending client PRODUCED over the window, and it is the only
    // denominator that makes the server's numbers directly comparable to the
    // client's arrival rate — which is itself measured against 60 captures/s.
    std::uint32_t firstCaptureTick = 0u;
    std::uint32_t lastCaptureTick  = 0u;
    std::uint32_t captureSpan      = 0u;

    // --- THREE FRACTIONS, PARTS PER THOUSAND, DELIBERATELY NOT COLLAPSED -----
    // Each names a different stage, and a single "loss" number would make the
    // remedy decision on merged evidence.
    //
    //   receivedX1000    `writes * 1000 / captureSpan` — of the capture ticks the
    //                    client produced, how many the server actually received.
    //                    UPSTREAM completeness. Depth cannot improve this; only
    //                    input redundancy can. §9.11 measured this as ~1000
    //                    indirectly (`[InputStats] dropped 0/238`); this is the
    //                    direct reading.
    //   observableX1000  `observableWrites * 1000 / writes` — of what the server
    //                    received, how much a once-per-poll publish could ever see.
    //                    THE COALESCING CEILING. 1000 = no coalescing.
    //                    ⭐ [T34] ITS DEFINITION IS UNCHANGED; ITS ARITHMETIC MOVED
    //                    WITH THE MECHANISM. Under the retired replace-latest write
    //                    path a writing frame published exactly ONE entry, so the
    //                    ceiling was `runs / writes` (~884 measured). Under bare C1
    //                    flush-on-poll the whole staged burst is published, so the
    //                    ceiling is `Sum min(runLength, stageCapacity) / writes` —
    //                    only a burst LONGER than the stage loses anything, which at
    //                    a stage capacity of 8 against a measured max run of 8 is
    //                    ~1000. Item 34's acceptance gate reads this field and its
    //                    pass condition is `>= 990`; leaving the old arithmetic in
    //                    place would have made that gate unpassable by construction
    //                    while the mechanism worked perfectly.
    //   replaceLatestObservableX1000
    //                    `runs * 1000 / writes` — the SAME ceiling computed the way
    //                    the replace-latest path imposed it. Kept ONLY so archived
    //                    T22/T33/T39 windows stay directly comparable and so the
    //                    improvement flush-on-poll bought is visible as two numbers
    //                    on one line rather than as a claim. It measures nothing
    //                    live: no code path has that ceiling any more.
    //   deliverableX1000 `observableWrites * 1000 / captureSpan` — the product, and
    //                    ⭐ THE NUMBER TO COMPARE AGAINST THE CLIENT. §9.11
    //                    measured a delivered fraction of 593 (35.6 arrivals/s
    //                    against 60 captures/s). If this reads ~593 the server's
    //                    own write pattern already accounts for the entire loss
    //                    and the send path is exonerated. If it reads ~1000 the
    //                    loss is genuinely downstream.
    std::uint32_t receivedX1000    = 0u;
    std::uint32_t observableX1000  = 0u;
    std::uint32_t deliverableX1000 = 0u;
    std::uint32_t replaceLatestObservableX1000 = 0u;

    // [T34] The numerator behind `observableX1000`: Sum min(runLength,
    // stageCapacity) over the window's completed runs. Reported raw so a window's
    // ceiling can be re-derived rather than trusted, and so a burst that genuinely
    // overflowed the stage is visible as `writes - observableWrites` rather than
    // only as a rounded per-mille.
    std::uint32_t observableWrites = 0u;

    // --- the shape of the arrival pattern -----------------------------------
    // Frames between writing frames that carried NO write for this owner. High
    // `emptyFrames` with runs > 1 is CLUMPING (packets arriving in bursts); zero
    // empty frames with runs == 1 everywhere is the healthy steady state.
    std::uint32_t emptyFrames = 0u;

    // --- upstream loss, kept STRICTLY SEPARATE ------------------------------
    // A write whose captureTick is not exactly prevCaptureTick + 1 means the
    // server never received that capture tick at all. `missedCaptureTicks` is the
    // count of those ticks and is the numerator behind `receivedX1000`.
    std::uint32_t nonConsecutiveWrites = 0u;
    std::uint32_t missedCaptureTicks   = 0u;

    // --- hygiene ------------------------------------------------------------
    // Non-zero means the window was RESTARTED mid-flight by a capture-tick
    // discontinuity (registration, re-join, tick domain re-established). A window
    // reporting this covers less wall clock than it appears to. DISCARD IT rather
    // than averaging it in — that is what this field is for.
    std::uint32_t discontinuities = 0u;
};

// [T34] How many of one frame's writes the publish step can carry —
// `relayedInputRing::kMaxDepth` under bare C1 flush-on-poll, 1 under the retired
// replace-latest path. INJECTED rather than included, so this header stays
// STL-only and so a test can drive both regimes without a build flag.
//
// A NAMED TYPE RATHER THAN A SECOND uint32, deliberately: the probe's other
// constructor argument is also a plain count, and `RelayWriteProbe probe(4u)`
// silently meaning something new is precisely the class of defect this initiative
// keeps paying for. With this wrapper every pre-T34 call site is a COMPILE error
// that has to be read, not a runtime surprise.
struct RelayStageCapacity
{
    std::uint32_t value = 1u;
};

class RelayWriteProbe
{
public:
    explicit RelayWriteProbe(RelayStageCapacity stageCapacity,
                             std::uint32_t windowRuns = kRelayWriteProbeWindowRuns)
        : m_windowRuns(windowRuns == 0u ? kRelayWriteProbeWindowRuns : windowRuns)
        , m_stageCapacity(stageCapacity.value == 0u ? 1u : stageCapacity.value)
    {
    }

    // Record one relay-ring write. Returns true — filling `out` and resetting THAT
    // OWNER's window — when this write completed a window.
    //
    //   ownerId       the relayed character's id (the key the client-side
    //                 RelayArrivalProbe also uses, so the two summaries line up).
    //   frameCounter  a GLOBAL, monotonic game-thread frame number (UE's
    //                 GFrameCounter). NOT an invocation count: the whole claim
    //                 rests on this being the engine's own frame identity, because
    //                 the frame is the unit Iris polls on.
    //   captureTick   the capture tick being written. Monotone per owner by
    //                 construction (the relay tap sits inside the coordinator's
    //                 `acceptedNew` arm), so a regression here is a bug and is
    //                 counted as a discontinuity rather than silently absorbed.
    //
    // A WINDOW CLOSES ON A RUN BOUNDARY, NOT ON A WRITE. The run that is still
    // open cannot be counted — its length is not known until a later frame's first
    // write proves it ended. Closing on writes would put a truncated run in the
    // histogram once per window and bias `maxRun` and the mean downward.
    bool noteWrite(unsigned int ownerId,
                   std::uint64_t frameCounter,
                   std::uint32_t captureTick,
                   RelayWriteWindowSummary& out)
    {
        OwnerState& s = m_owners[ownerId];

        if (!s.seeded)
        {
            s.seeded = true;
            restart(s, frameCounter, captureTick);
            return false;
        }

        // A DISCONTINUITY RESTARTS THE WINDOW RATHER THAN POISONING IT. A capture
        // tick that jumped by more than a plausible stall — or went backwards,
        // which the monotonic relay gate makes unreachable today — breaks the
        // `captureSpan` denominator, and a span that no longer matches the writes
        // it spans makes every fraction below meaningless. ServerFrameProbe
        // re-seeds for the same reason. `discontinuities` survives the restart so
        // the NEXT emitted line still says the window was interrupted.
        const bool backwards   = (captureTick <= s.lastCaptureTick);
        const std::uint32_t advance = backwards ? 0u : (captureTick - s.lastCaptureTick);
        if (backwards || advance > kRelayWriteDiscontinuityTicks
            || frameCounter < s.currentFrame)
        {
            const std::uint32_t carried = s.discontinuities + 1u;
            restart(s, frameCounter, captureTick);
            s.discontinuities = carried;
            return false;
        }

        // Capture-tick accounting runs on EVERY write, before any frame reasoning,
        // so it stays correct whatever the frame pattern turns out to be.
        if (advance > 1u)
        {
            ++s.nonConsecutiveWrites;
            s.missedCaptureTicks += (advance - 1u);
        }

        if (frameCounter == s.currentFrame)
        {
            // Coalesced: this write overwrites the last one in a depth-1 ring
            // before Iris ever polls. The capture tick it carries still ends the
            // run, so the watermark advances; the run length does not close yet.
            ++s.currentRun;
            s.lastCaptureTick = captureTick;
            return false;
        }

        // A new frame started, so the previous run is now COMPLETE and countable —
        // and its LAST write is the previous one, which is what closes the window's
        // capture span. `captureTick` belongs to the run that is only now opening,
        // and therefore to the NEXT window.
        s.emptyFrames += static_cast<std::uint32_t>(
            (frameCounter - s.currentFrame) - 1u);
        recordRun(s, s.currentRun);
        s.windowEndCapture = s.lastCaptureTick;

        s.currentFrame    = frameCounter;
        s.currentRun      = 1u;
        s.lastCaptureTick = captureTick;

        if (s.runs < m_windowRuns)
        {
            return false;
        }

        fillSummary(out, ownerId, s);
        // The next window opens on the run this write just started, so its capture
        // span starts at THIS capture tick. Anchoring it anywhere else would leave
        // a one-write hole between consecutive windows.
        resetWindow(s, captureTick);
        return true;
    }

    // Drop an owner's state. Called from the same unregister contract that reaps
    // the reception coordinator's claim map, so the map stays bounded by live ids.
    void forgetOwner(unsigned int ownerId) { m_owners.erase(ownerId); }

    // --- introspection; tests and diagnostics only -------------------------
    std::size_t   trackedOwnerCount() const { return m_owners.size(); }
    std::uint32_t openRunLength(unsigned int ownerId) const
    {
        const auto it = m_owners.find(ownerId);
        return (it == m_owners.end()) ? 0u : it->second.currentRun;
    }

    // The summary this owner's window WOULD report right now. Exposed so a test can
    // assert a distribution without having to fill a 120-run window.
    bool peekSummary(unsigned int ownerId, RelayWriteWindowSummary& out) const
    {
        const auto it = m_owners.find(ownerId);
        if (it == m_owners.end())
        {
            return false;
        }
        fillSummary(out, ownerId, it->second);
        return true;
    }

private:
    struct OwnerState
    {
        bool          seeded          = false;
        std::uint64_t currentFrame    = 0u;
        std::uint32_t currentRun      = 0u;
        std::uint32_t lastCaptureTick = 0u;

        // The window's capture-tick span. `windowStartCapture` is the first write
        // of the window's first run; `windowEndCapture` is the LAST write of the
        // most recently COMPLETED run — never the open one, whose extent is not
        // known yet.
        std::uint32_t windowStartCapture = 0u;
        std::uint32_t windowEndCapture   = 0u;

        std::array<std::uint32_t, kRelayWriteOverflowBucket + 1u> buckets{};
        std::uint32_t runs                 = 0u;
        std::uint32_t writes               = 0u;
        std::uint32_t observableWrites     = 0u;
        std::uint32_t maxRun               = 0u;
        std::uint32_t emptyFrames          = 0u;
        std::uint32_t nonConsecutiveWrites = 0u;
        std::uint32_t missedCaptureTicks   = 0u;
        std::uint32_t discontinuities      = 0u;
    };

    // Anchor everything to this (frame, capture tick) and clear the window. Used
    // for the first write and for a discontinuity — the two cases where no earlier
    // state is usable.
    static void restart(OwnerState& s,
                        std::uint64_t frameCounter,
                        std::uint32_t captureTick)
    {
        s.currentFrame    = frameCounter;
        s.currentRun      = 1u;
        s.lastCaptureTick = captureTick;
        resetWindow(s, captureTick);
    }

    // [T34] NOT static any more: the observable numerator needs the stage capacity,
    // which is per-probe. Everything else is unchanged.
    void recordRun(OwnerState& s, std::uint32_t runLength) const
    {
        const std::size_t bucket = (runLength > kRelayWriteMaxTrackedRun)
            ? kRelayWriteOverflowBucket
            : static_cast<std::size_t>(runLength);
        ++s.buckets[bucket];
        ++s.runs;
        s.writes += runLength;
        s.observableWrites += (runLength > m_stageCapacity) ? m_stageCapacity : runLength;
        if (runLength > s.maxRun) { s.maxRun = runLength; }
    }

    // NEAREST-RANK over the run-length histogram — the same definition
    // RelayArrivalProbe::percentile uses, so a p99 here and a p99 there are
    // comparable without a footnote.
    static std::uint32_t percentile(const OwnerState& s,
                                    std::uint32_t numeratorPercent,
                                    bool& outSaturated)
    {
        outSaturated = false;
        if (s.runs == 0u)
        {
            return 0u;
        }

        const std::uint64_t scaled = static_cast<std::uint64_t>(numeratorPercent)
                                   * static_cast<std::uint64_t>(s.runs);
        const std::uint64_t rank = (scaled + 99u) / 100u;

        std::uint64_t cumulative = 0u;
        for (std::size_t bucket = 1u; bucket <= kRelayWriteOverflowBucket; ++bucket)
        {
            cumulative += s.buckets[bucket];
            if (cumulative >= rank)
            {
                outSaturated = (bucket == kRelayWriteOverflowBucket);
                return static_cast<std::uint32_t>(bucket);
            }
        }
        outSaturated = false;
        return s.maxRun;
    }

    void fillSummary(RelayWriteWindowSummary& out,
                     unsigned int ownerId,
                     const OwnerState& s) const
    {
        bool p50Saturated = false;
        out.ownerId          = ownerId;
        out.runs             = s.runs;
        out.writes           = s.writes;
        out.observableWrites = s.observableWrites;
        out.p50          = percentile(s, 50u, p50Saturated);
        out.p99          = percentile(s, 99u, out.p99Saturated);
        out.maxRun       = s.maxRun;

        out.firstCaptureTick = s.windowStartCapture;
        out.lastCaptureTick  = s.windowEndCapture;
        out.captureSpan      = (s.runs == 0u
                                || s.windowEndCapture < s.windowStartCapture)
            ? 0u
            : (s.windowEndCapture - s.windowStartCapture + 1u);

        out.observableX1000 = (s.writes == 0u)
            ? 0u
            : static_cast<std::uint32_t>(
                  (static_cast<std::uint64_t>(s.observableWrites) * 1000u) / s.writes);
        out.replaceLatestObservableX1000 = (s.writes == 0u)
            ? 0u
            : static_cast<std::uint32_t>(
                  (static_cast<std::uint64_t>(s.runs) * 1000u) / s.writes);
        out.receivedX1000 = (out.captureSpan == 0u)
            ? 0u
            : static_cast<std::uint32_t>(
                  (static_cast<std::uint64_t>(s.writes) * 1000u) / out.captureSpan);
        out.deliverableX1000 = (out.captureSpan == 0u)
            ? 0u
            : static_cast<std::uint32_t>(
                  (static_cast<std::uint64_t>(s.observableWrites) * 1000u) / out.captureSpan);

        out.emptyFrames          = s.emptyFrames;
        out.nonConsecutiveWrites = s.nonConsecutiveWrites;
        out.missedCaptureTicks   = s.missedCaptureTicks;
        out.discontinuities      = s.discontinuities;
    }

    // The OPEN run, the frame anchor and the capture-tick watermark survive a
    // window reset — only the window's own accumulators are cleared. Resetting the
    // run or the watermark would inject a false run boundary and a false capture
    // gap at every window edge: one per 120 runs, which is exactly the size of
    // effect this probe is trying to resolve.
    static void resetWindow(OwnerState& s, std::uint32_t startCaptureTick)
    {
        s.buckets.fill(0u);
        s.runs                 = 0u;
        s.writes               = 0u;
        s.observableWrites     = 0u;
        s.maxRun               = 0u;
        s.emptyFrames          = 0u;
        s.nonConsecutiveWrites = 0u;
        s.missedCaptureTicks   = 0u;
        s.discontinuities      = 0u;
        s.windowStartCapture   = startCaptureTick;
        s.windowEndCapture     = startCaptureTick;
    }

    std::uint32_t m_windowRuns;
    // [T34] How many of one frame's staged writes the publish step can carry.
    std::uint32_t m_stageCapacity;
    std::unordered_map<unsigned int, OwnerState> m_owners;
};

// ---------------------------------------------------------------------------
// PROBE 6 — PER-CONNECTION SEND BUDGET AND THROUGHPUT.
//
// WHAT IT SETTLES, AND WHY THE ARITHMETIC ALONE WAS NOT ENOUGH. The task's budget
// model is: the server's per-tick send allowance is
// `CurrentNetSpeed / DesiredTickRate` bytes, and a connection may bank at most two
// ticks of unused allowance. Both halves are read from engine source
// (`UNetConnection::Tick`: `DeltaBits = CurrentNetSpeed * clamp(DeltaTime, 0,
// 1/DesiredTickRate) * 8`, then `QueuedBits` is floored at `-2 * DeltaBits`), and
// at `MaxClientRate = 250000` / 60 Hz that is 4,166.67 B/tick with 8,333 B of
// bankable credit. Against a modelled 1,413.75 B/round for three characters that
// is ~34 % occupancy — nowhere near saturation.
//
// EVERY NUMBER IN THAT PARAGRAPH IS DERIVED, NOT OBSERVED. `CurrentNetSpeed` is
// what the SERVER clamped the client's request to, at runtime, on a code path
// (`UWorld::NotifyControlMessage`) nobody on this initiative has watched execute;
// and the modelled payload counts two properties out of an unknown total. This
// probe replaces every derived term with a measured one:
//
//   * `netSpeedBps` — the connection's ACTUAL `CurrentNetSpeed`. If this is not
//     250000, the whole 34 % reading is wrong and Protocol B's prediction inverts.
//   * `outBytes` / `outPackets` deltas — the REAL per-tick payload, all properties,
//     all framing, all headers. This is Protocol C's answer, at connection
//     resolution: the two properties T29 measured are a known 401 B/char/round, so
//     the residual is everything else.
//   * `queuedBits` min/max and `notReadyFrames` — the saturation state itself.
//     `QueuedBits + SendBuffer <= 0` IS `UNetConnection::IsNetReady()`, and Iris's
//     `UDataStreamChannel::Tick` returns WITHOUT WRITING ANYTHING when it is false.
//     A non-zero `notReadyFrames` is the send-path deferral candidate's smoking
//     gun; a zero one with `queuedBits` pinned near its floor is that candidate's
//     refutation, measured rather than argued.
//   * `outPacketsLost` delta — ack-derived, so it is the emulation's REAL outgoing
//     loss rate rather than the configured `PktLoss` percentage. That is candidate
//     1's evidence without changing a single config value.
//
// TAKES PLAIN NUMBERS, exactly like ServerFrameProbe, so it is engine-free and
// unit-testable; the caller reads the UNetConnection fields.
// ---------------------------------------------------------------------------

// One summary per this many samples, per connection. At the intended once-per-
// server-frame cadence that is ~2 s — the same feel as every other window here.
inline constexpr std::uint32_t kConnectionBudgetWindowSamples = 120u;

struct ConnectionBudgetWindowSummary
{
    std::uint32_t connectionId = 0u;

    // --- the window itself --------------------------------------------------
    std::uint32_t samples      = 0u;
    std::uint32_t elapsedMs    = 0u;

    // --- the allowance, as the server actually negotiated it -----------------
    std::int32_t  netSpeedBps         = 0;
    std::uint32_t allowanceBytesPerTick = 0u;   // netSpeedBps / tickRate

    // --- what was actually sent ---------------------------------------------
    std::uint32_t outBytes          = 0u;   // delta over the window
    std::uint32_t outPackets        = 0u;
    std::uint32_t outPacketsLost    = 0u;
    std::uint32_t bytesPerSample    = 0u;   // outBytes / samples
    std::uint32_t bytesPerPacket    = 0u;
    // Occupancy of the per-tick allowance, in tenths of a percent. 340 = 34.0 %.
    std::uint32_t occupancyPctX10   = 0u;

    // --- the saturation state ------------------------------------------------
    // QueuedBits is NEGATIVE when there is headroom (it is a debt counter that the
    // per-tick allowance pays down), so `min` is the MOST headroom seen and `max`
    // is the CLOSEST to saturation. `notReadySamples` counts samples at or above
    // zero — the state in which Iris writes nothing at all this frame.
    std::int32_t  queuedBitsMin  = 0;
    std::int32_t  queuedBitsMax  = 0;
    std::int32_t  queuedBitsMean = 0;
    std::uint32_t notReadySamples = 0u;
};

class ConnectionBudgetProbe
{
public:
    explicit ConnectionBudgetProbe(
        std::uint32_t windowSamples = kConnectionBudgetWindowSamples)
        : m_windowSamples(windowSamples == 0u ? kConnectionBudgetWindowSamples
                                              : windowSamples)
    {
    }

    // One sample, once per server game-thread frame, per client connection.
    //
    //   outTotalBytes / outTotalPackets / outTotalPacketsLost
    //       the connection's SESSION-CUMULATIVE counters (UNetConnection::
    //       OutTotalBytes etc.), not the StatPeriod ones. Cumulative counters are
    //       used deliberately: the StatPeriod accumulators are reset by the engine
    //       on its own schedule, so differencing them across our window would drop
    //       whatever the engine zeroed mid-window.
    //   tickRateHz
    //       the rate the per-tick allowance is computed against — the server's
    //       DesiredTickRate. Passed in rather than assumed so that a run on a
    //       differently-configured server still reports a correct allowance.
    bool noteSample(std::uint32_t connectionId,
                    std::int32_t netSpeedBps,
                    std::int32_t queuedBits,
                    std::uint64_t outTotalBytes,
                    std::uint64_t outTotalPackets,
                    std::uint64_t outTotalPacketsLost,
                    std::uint32_t tickRateHz,
                    std::uint64_t nowMicros,
                    ConnectionBudgetWindowSummary& out)
    {
        ConnState& s = m_conns[connectionId];

        if (!s.seeded)
        {
            s.seeded = true;
            reseed(s, outTotalBytes, outTotalPackets, outTotalPacketsLost, nowMicros);
            return false;
        }

        // A counter that went backwards means the connection was replaced under the
        // same id (reconnect). Re-anchor rather than reporting a negative delta as
        // a gigantic unsigned one.
        if (outTotalBytes < s.startBytes || outTotalPackets < s.startPackets
            || outTotalPacketsLost < s.startPacketsLost || nowMicros < s.startMicros)
        {
            reseed(s, outTotalBytes, outTotalPackets, outTotalPacketsLost, nowMicros);
            return false;
        }

        if (s.samples == 0u)
        {
            s.queuedBitsMin = queuedBits;
            s.queuedBitsMax = queuedBits;
        }
        else
        {
            if (queuedBits < s.queuedBitsMin) { s.queuedBitsMin = queuedBits; }
            if (queuedBits > s.queuedBitsMax) { s.queuedBitsMax = queuedBits; }
        }
        s.queuedBitsSum += static_cast<std::int64_t>(queuedBits);
        if (queuedBits >= 0) { ++s.notReadySamples; }
        s.netSpeedBps = netSpeedBps;
        s.tickRateHz  = tickRateHz;
        ++s.samples;

        if (s.samples < m_windowSamples)
        {
            return false;
        }

        fillSummary(out, connectionId, s,
                    outTotalBytes, outTotalPackets, outTotalPacketsLost, nowMicros);
        reseed(s, outTotalBytes, outTotalPackets, outTotalPacketsLost, nowMicros);
        return true;
    }

    void forgetConnection(std::uint32_t connectionId) { m_conns.erase(connectionId); }

    std::size_t trackedConnectionCount() const { return m_conns.size(); }

private:
    struct ConnState
    {
        bool          seeded           = false;
        std::uint64_t startBytes       = 0u;
        std::uint64_t startPackets     = 0u;
        std::uint64_t startPacketsLost = 0u;
        std::uint64_t startMicros      = 0u;

        std::uint32_t samples         = 0u;
        std::int32_t  queuedBitsMin   = 0;
        std::int32_t  queuedBitsMax   = 0;
        std::int64_t  queuedBitsSum   = 0;
        std::uint32_t notReadySamples = 0u;
        std::int32_t  netSpeedBps     = 0;
        std::uint32_t tickRateHz      = 0u;
    };

    static void reseed(ConnState& s,
                       std::uint64_t outTotalBytes,
                       std::uint64_t outTotalPackets,
                       std::uint64_t outTotalPacketsLost,
                       std::uint64_t nowMicros)
    {
        s.startBytes       = outTotalBytes;
        s.startPackets     = outTotalPackets;
        s.startPacketsLost = outTotalPacketsLost;
        s.startMicros      = nowMicros;
        s.samples          = 0u;
        s.queuedBitsMin    = 0;
        s.queuedBitsMax    = 0;
        s.queuedBitsSum    = 0;
        s.notReadySamples  = 0u;
    }

    static void fillSummary(ConnectionBudgetWindowSummary& out,
                            std::uint32_t connectionId,
                            const ConnState& s,
                            std::uint64_t outTotalBytes,
                            std::uint64_t outTotalPackets,
                            std::uint64_t outTotalPacketsLost,
                            std::uint64_t nowMicros)
    {
        out.connectionId = connectionId;
        out.samples      = s.samples;
        out.elapsedMs    = static_cast<std::uint32_t>((nowMicros - s.startMicros) / 1000u);

        out.netSpeedBps = s.netSpeedBps;
        out.allowanceBytesPerTick = (s.tickRateHz == 0u || s.netSpeedBps <= 0)
            ? 0u
            : static_cast<std::uint32_t>(
                  static_cast<std::uint32_t>(s.netSpeedBps) / s.tickRateHz);

        out.outBytes       = static_cast<std::uint32_t>(outTotalBytes - s.startBytes);
        out.outPackets     = static_cast<std::uint32_t>(outTotalPackets - s.startPackets);
        out.outPacketsLost =
            static_cast<std::uint32_t>(outTotalPacketsLost - s.startPacketsLost);

        out.bytesPerSample = (s.samples == 0u) ? 0u : (out.outBytes / s.samples);
        out.bytesPerPacket = (out.outPackets == 0u) ? 0u : (out.outBytes / out.outPackets);
        out.occupancyPctX10 = (out.allowanceBytesPerTick == 0u)
            ? 0u
            : static_cast<std::uint32_t>(
                  (static_cast<std::uint64_t>(out.bytesPerSample) * 1000u)
                  / out.allowanceBytesPerTick);

        out.queuedBitsMin  = s.queuedBitsMin;
        out.queuedBitsMax  = s.queuedBitsMax;
        out.queuedBitsMean = (s.samples == 0u)
            ? 0
            : static_cast<std::int32_t>(s.queuedBitsSum / static_cast<std::int64_t>(s.samples));
        out.notReadySamples = s.notReadySamples;
    }

    std::uint32_t m_windowSamples;
    std::unordered_map<std::uint32_t, ConnState> m_conns;
};
