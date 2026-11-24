#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// ---------------------------------------------------------------------------
// RelayWriteProbe -- the SERVER-side relay telemetry.
// Companion: Network/RelayReadProbe.h (the CLIENT-side read and arrival probes,
// plus FrameHealthProbe). Tests: Network/RelayWritePathProbeTest.cpp.
// Every `§N` mark below resolves in docs/RelayProbes-rationale.md.
//
// ---------------------------------------------------------------------------
// ORIENTATION
//
//   object                 thread        window closes on
//   ---------------------  ------------  -----------------------------
//   RelayWriteProbe        GAME, SERVER  120 COMPLETED RUNS, per owner
//
//   PROBE 5 (RelayWriteProbe) asks how many relay writes for ONE owner share
//   one server game-thread frame, and so how many of them a once-per-poll
//   publish could ever carry. §10
//   PROBE 6 (ConnectionBudgetProbe, a connection's rate, payload and saturation
//   state) was removed by og-session-runtime task 14 with its one adapter call
//   site, the Unreal session's in-frame server. §12
//
// ⛔ INSTRUMENTATION ONLY, GAME THREAD ONLY, server role -- no lock, no logging, no feedback into any simulated value. §13
// ⛔ STL ONLY, no engine types. §13
// ⚠ It does not log; it hands back a summary struct and the caller owns the logger. Global namespace, like the rest of the core. §13
//
// ---------------------------------------------------------------------------
// ONE ADAPTER'S BINDING FOR THE REPLICATION NAMES. This header names ROLES --
// replication system, replication poll, replication writer, game-thread frame
// identity. For one adapter (Unreal) those were Iris, its once-per-frame property
// poll, `FReplicationWriter` and `GFrameCounter`, until og-input-latency P5-7 moved
// the sim traffic to og-sim-net; since then no adapter feeds `RelayWriteProbe` (the
// Unreal session's PROBE 5 is gone, and only og-simulation-tests drive it). Another
// adapter substitutes its own; nothing here depends on them.
//
// ⭐ WHY IT EXISTS AND WHAT IT FOUND: an elimination chain blamed the replication send path without testing whether two relay writes can share one server frame. They can: the answer was COALESCING, ~11.6 % of relayed inputs. §10
// ⛔ THAT WRITE PATH IS GONE: flush-on-poll staging replaced replace-latest, so the probe now measures the STAGE-OVERFLOW ceiling and `observableX1000` is that ceiling's acceptance gate. §11
// ⚠ A RUN LENGTH OF N DOES NOT PROVE the N-1 older writes would have arrived -- a send-path drop stacks on top. This is a CEILING, never a loss total. §10
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PROBE 5 — RELAY WRITES PER SERVER GAME-THREAD FRAME.
// ---------------------------------------------------------------------------

// A run is the relay writes for ONE owner inside ONE game-thread frame, so 0 is
// not observable and the histogram starts at 1; 16 is past the measured max of 8. §10
// ⛔ INDEX-ALIGNED WITH RelayArrivalProbe's gap histogram, deliberately -- the two are read side by side, so do not shift either. §13
inline constexpr std::uint32_t kRelayWriteMaxTrackedRun = 16u;
inline constexpr std::size_t   kRelayWriteOverflowBucket =
    static_cast<std::size_t>(kRelayWriteMaxTrackedRun) + 1u;

// One summary per this many COMPLETED RUNS (writing frames), per owner -- runs,
// not writes, so the sample count means what RelayArrivalProbe's does. §10
inline constexpr std::uint32_t kRelayWriteProbeWindowRuns = 120u;

// A capture-tick jump larger than this is a DISCONTINUITY (registration, a
// re-join, the tick domain being re-established), not a coverage hole. §10
// ⛔ NEVER ALLOWED INTO `missedCaptureTicks`, where one five-digit outlier swamps every real number. Same guard as `kFrameHealthDiscontinuityTicks`. §7
inline constexpr std::uint32_t kRelayWriteDiscontinuityTicks = 600u;

struct RelayWriteWindowSummary
{
    // --- identity -----------------------------------------------------------
    unsigned int ownerId = 0u;

    // --- THE HEADLINE: writes per writing frame -----------------------------
    // `runs` is the game-thread frames this owner's ring was written in, `writes`
    // the relay writes those frames carried. Their ratio is the coalescing factor. §10
    std::uint32_t runs         = 0u;
    std::uint32_t writes       = 0u;
    std::uint32_t p50          = 0u;
    std::uint32_t p99          = 0u;
    std::uint32_t maxRun       = 0u;
    bool          p99Saturated = false;

    // --- THE CAPTURE-TICK DENOMINATOR ---------------------------------------
    // `lastCaptureTick - firstCaptureTick + 1`: the capture ticks the sending
    // client PRODUCED over this window. §10
    // ⛔ THE ONLY DENOMINATOR comparable to the client's arrival rate, which is itself measured against 60 captures/s. §10
    std::uint32_t firstCaptureTick = 0u;
    std::uint32_t lastCaptureTick  = 0u;
    std::uint32_t captureSpan      = 0u;

    // --- FOUR FRACTIONS, PARTS PER THOUSAND, DELIBERATELY NOT COLLAPSED -----
    // ⛔ EACH NAMES A DIFFERENT STAGE -- one merged loss number would decide the remedy on merged evidence. §10
    //
    //   receivedX1000                 `writes*1000/captureSpan`  UPSTREAM completeness
    //   observableX1000               `observableWrites*1000/writes`  THE COALESCING CEILING; 1000 = none
    //   replaceLatestObservableX1000  `runs*1000/writes`  the RETIRED replace-latest path's ceiling
    //   deliverableX1000              `observableWrites*1000/captureSpan`  the product; COMPARE AGAINST THE CLIENT
    //
    // ⛔ `observableX1000`'s ARITHMETIC MOVED WITH THE MECHANISM, ITS DEFINITION DID NOT: the old `runs/writes` formula makes the `>= 990` gate unpassable by construction. §11
    // ⛔ `replaceLatestObservableX1000` MEASURES NOTHING LIVE -- no code path has that ceiling any more; it exists only so archived windows stay comparable. §11
    std::uint32_t receivedX1000    = 0u;
    std::uint32_t observableX1000  = 0u;
    std::uint32_t deliverableX1000 = 0u;
    std::uint32_t replaceLatestObservableX1000 = 0u;

    // The numerator behind `observableX1000`: Sum min(runLength, stageCapacity). §11
    // ⚠ REPORTED RAW so a ceiling can be re-derived rather than trusted, and a stage overflow shows as `writes - observableWrites`. §11
    std::uint32_t observableWrites = 0u;

    // --- the shape of the arrival pattern -----------------------------------
    // Frames between writing frames carrying NO write for this owner. High with
    // runs > 1 is CLUMPING; zero with runs == 1 is the healthy steady state.
    std::uint32_t emptyFrames = 0u;

    // --- upstream loss, kept STRICTLY SEPARATE ------------------------------
    // ⛔ A WRITE WHOSE captureTick IS NOT `prev + 1` MEANS THE SERVER NEVER RECEIVED THAT TICK -- `missedCaptureTicks` feeds `receivedX1000`, never a coalescing measure. §10
    std::uint32_t nonConsecutiveWrites = 0u;
    std::uint32_t missedCaptureTicks   = 0u;

    // --- hygiene ------------------------------------------------------------
    // ⛔ NON-ZERO MEANS DISCARD THIS WINDOW, do not average it in: a capture-tick discontinuity restarted it mid-flight, so it covers less wall clock than it appears to. §10
    std::uint32_t discontinuities = 0u;
};

// How many of one frame's writes the publish step can carry: `relayedInputRing::
// kMaxDepth` under flush-on-poll, 1 under the retired replace-latest path. §11
// ⛔ INJECTED RATHER THAN INCLUDED, so this header stays STL-only and a test can drive both regimes without a build flag. §11
// ⛔ A NAMED TYPE RATHER THAN A SECOND uint32, deliberately: every pre-flush call site becomes a COMPILE error, not a runtime surprise. §11
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

    // Record one relay-ring write. Returns true -- filling `out`, resetting THAT
    // OWNER's window -- when this write completed a window. `ownerId` is the key
    // the client-side RelayArrivalProbe also uses, so the two summaries line up. §10
    //
    // ⛔ `frameCounter` MUST BE THE ENGINE'S OWN FRAME IDENTITY, NOT AN INVOCATION COUNT -- the frame is the unit the replication system polls on. §10
    // ⛔ `captureTick` IS MONOTONE PER OWNER BY CONSTRUCTION -- the relay tap sits in the coordinator's `acceptedNew` arm, so a regression is a bug and counts as a discontinuity. §10
    // ⛔ A WINDOW CLOSES ON A RUN BOUNDARY, NEVER ON A WRITE -- an open run's length is unknown, so closing on writes biases `maxRun` and the mean downward. §10
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

        // ⛔ A DISCONTINUITY RESTARTS THE WINDOW RATHER THAN POISONING IT: a `captureSpan` that no longer matches its writes makes every fraction meaningless. FrameHealthProbe re-seeds for the same reason. §10
        // ⛔ `discontinuities` SURVIVES THE RESTART so the NEXT emitted line still says the window was interrupted. §10
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

        // ⚠ ON EVERY WRITE, BEFORE ANY FRAME REASONING, so it stays correct whatever the frame pattern turns out to be. §10
        if (advance > 1u)
        {
            ++s.nonConsecutiveWrites;
            s.missedCaptureTicks += (advance - 1u);
        }

        if (frameCounter == s.currentFrame)
        {
            // ⛔ COALESCED, under the retired depth-1 ring: the capture tick still ends the run so the watermark advances, but the run length does not close yet. §11
            ++s.currentRun;
            s.lastCaptureTick = captureTick;
            return false;
        }

        // A new frame started, so the previous run is COMPLETE and countable, and
        // its LAST write closes the window's capture span. §10
        // ⚠ `captureTick` BELONGS TO THE RUN ONLY NOW OPENING, and therefore to the NEXT window. §10
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
        // ⛔ THE NEXT WINDOW'S SPAN STARTS AT THIS CAPTURE TICK -- anchoring it anywhere else leaves a one-write hole between consecutive windows. §10
        resetWindow(s, captureTick);
        return true;
    }

    // ⛔ CALLED FROM THE SAME UNREGISTER CONTRACT that reaps the reception coordinator's claim map -- without it nothing bounds this map by live ids. §13
    void forgetOwner(unsigned int ownerId) { m_owners.erase(ownerId); }

    // --- introspection; tests and diagnostics only -------------------------
    std::size_t   trackedOwnerCount() const { return m_owners.size(); }
    std::uint32_t openRunLength(unsigned int ownerId) const
    {
        const auto it = m_owners.find(ownerId);
        return (it == m_owners.end()) ? 0u : it->second.currentRun;
    }

    // The summary this owner's window WOULD report now, so a test can assert a
    // distribution without filling a 120-run window.
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

        // The window's capture-tick span; `windowStartCapture` is its first run's first write. §10
        // ⛔ `windowEndCapture` IS THE LAST WRITE OF THE MOST RECENTLY COMPLETED RUN, never the open one, whose extent is not known yet. §10
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

    // Anchor everything to this (frame, capture tick) and clear the window: the
    // first write, and a discontinuity -- the two cases with no usable prior state.
    static void restart(OwnerState& s,
                        std::uint64_t frameCounter,
                        std::uint32_t captureTick)
    {
        s.currentFrame    = frameCounter;
        s.currentRun      = 1u;
        s.lastCaptureTick = captureTick;
        resetWindow(s, captureTick);
    }

    // ⚠ NOT STATIC: the observable numerator needs the stage capacity, which is per-probe. Everything else is unchanged. §11
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

    // ⛔ NEAREST-RANK, the same definition RelayArrivalProbe::percentile uses, so a p99 here and a p99 there are comparable without a footnote. §13
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

    // ⛔ THE OPEN RUN, THE FRAME ANCHOR AND THE WATERMARK SURVIVE A WINDOW RESET -- resetting any of them injects a false run boundary and capture gap at every window edge. §10
    // ⚠ That is one per 120 runs, which is exactly the size of effect this probe is trying to resolve. §10
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
    // How many of one frame's staged writes the publish step can carry. §11
    std::uint32_t m_stageCapacity;
    std::unordered_map<unsigned int, OwnerState> m_owners;
};
