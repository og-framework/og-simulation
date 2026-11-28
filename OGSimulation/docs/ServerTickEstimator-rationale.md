<!-- SPDX-License-Identifier: MPL-2.0 -->
# `ServerTickEstimator.h` — rationale

The narrative behind `ServerTickEstimator`, the client's estimate of the server's current continuous tick, built from the
clock ping/pong samples the client already receives. The header keeps its licence, its docs pointer and its code; everything
that explains it lives here.

**If this file and `ServerTickEstimator.h` disagree, the header is authoritative and this file is stale.** Fix this file; do
not soften the header to match it.

This header has no guards document: every prohibition found while writing it is either pinned by a test in
`ServerTickEstimatorTest.cpp` (og-simulation-tests) or is typed in another file (§9).

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free and allocation-free (the STL and
`TimeConfig.h`). Where a host appears below it is "the host": whatever program owns the connection, the clock and the log.

---

## §1 What it is

A sensor. From each clock sample it forms one **offset**, "the server's tick minus the client's clock, in ticks", filters the
offsets, and answers `serverTickAt(nowSeconds)` = now · f + ô: the server's continuous tick at client time now, on the
clock the samples are stamped with. f is `tickFrequency`.

Added by og-time-dilation task 3 (2026-11-20) as the input of the client's rate controller (a later task). `NetworkTimeEstimator`
owns one and feeds it (§9). **No production code reads it yet**, and until a host copies the pong's server tick into
`SimClockRttSample` every sample carries the none value and is ignored, so its arrival changes no runtime behaviour.

"Continuous tick" is defined by the server's step starts: the server's continuous tick is n at the instant its step to n
started, and a seconds later it is n + a · f. The server publishes n once it has finished that step, together with the
step's start time, and measures the pong's age from that start (og-sim-net's `SimNetClock-rationale.md` §5, since
og-time-dilation task 2b; until then from the publication, which placed n a step's duration later).

## §2 The offset formula, and its NTP basis

A sample carries:
- `serverTick` n: the tick the server had last published when it answered the ping;
- `serverTickAgeSeconds` a: the server's time from the start of its step to n to sending the pong, on the server's clock;
- `rttSeconds`: the round trip with the server's turnaround removed, (t3 − t0) − (t2 − t1), NTP's delay (RFC 5905 §8);
- `t3Seconds`: the client's receive time.

When the pong left, the server's continuous tick was n + a · f. The pong then travelled one downlink delay d, so at t3 the
server's tick was n + (a + d) · f. d is not observable; NTP's assumption is that it is half the round trip. So one sample gives

    o = n + (a + rtt/2) · f − t3 · f

and the estimate at any later client time is now · f + ô. Using the full RTT instead of half adds rtt/2 · f to every offset,
about 2 ticks at a 65 ms RTT (the test's seen-failing probe measured 2.05).

## §3 Why the pong's tick age is needed, and the phase-blind second filter

Without a, the tick's phase inside its step at the pong's send is unknown in [0, 1). On a jittery link the phase is uniform
and averages to 0.5. But a client pinging at 20 Hz against a 60 Hz server sends every 3 ticks exactly; on a link with
little jitter (loopback, one machine, one clock) every pong then leaves at the same phase, and each client would carry its
own constant bias of up to a tick. The age removes it, for 4 bytes on the pong.

**The phase-blind filter** runs the same rules on the same samples with a replaced by its mean, 0.5 / f, and is read by
`phaseBlindServerTickAt`. It exists only to measure, on real captures, what the age buys. The test
`PhaseLockedPingsNeedTheAge` puts every pong at phase 0.9: the estimate's error stays under 2e-12 tick while the phase-blind
one reads −0.4 tick (0.5 − 0.9) on average and is off by at least 0.3 tick on every sample. It has its own seed, run and count; its re-seeds are not counted.

The phase-blind filter never reads a, so og-time-dilation task 2b (the age measured from the step's start) does not move it.
A tick is published a step's duration D after its step starts, so the pongs that carry n leave while the server's continuous
tick runs from n + D · f to n + 1 + D · f: on a jittery link the phase-blind estimate reads about D · f low, and the
estimate with a does not.

## §4 What it reads on an asymmetric link: (u − d)/2

With uplink u and downlink d, rtt/2 − d = (u − d)/2, so the estimate is too high by (u − d)/2 · f: NTP's offset error,
by construction. The test `AnAsymmetricLinkReadsHalfTheAsymmetry` pins it: u = 50 ms, d = 20 ms reads +0.90 tick.

This is the intended semantics, not a defect to correct. A controller that holds the client's lead over this estimate at a
target of the form (R/2 + margin) · f gets a true lead of u · f + margin: the lead is the uplink plus a margin, the same for
every client, which is what makes every client's inputs reach the server with the same slack.

## §5 The filter, sample by sample

1. **Ignored, no count:** a `serverTick` of `kNoServerTick` (0xFFFFFFFF, also `SimClockRttSample::kNoServerTick`, which is
   defined from it), any non-finite input, a negative age, or an age of `kMaxServerTickAgeSeconds` (2³¹ µs, 2147.48 s) or
   more. One host's binding (og-sim-net's SimClock pong) carries the age as a u32 of microseconds, the unsigned difference of
   two clock readings, so a server clock that steps backwards yields an age near 2³² µs rather than a negative one; an age in
   the upper half of that range is read as the negative age it is. An honest age is a fraction of a second.
2. **Rejected, counted in `rejectedSamples()`:** when the smoothed RTT is above 0 and
   `rttSeconds > rttGateMultiplier · smoothedRttSeconds + rttGateMarginSeconds`. A sample that waited in a queue has a
   long RTT and, being queued on one side, an asymmetric one: its offset would be wrong by about half the queueing. A rejected sample moves nothing else, not even the last-sample
   time, so a link that delivers only queued samples goes stale (§6) instead of looking fresh with an old estimate
   (`ValidAfterTwentySamplesAndStaleOneSecondAfterTheLast` pins that).
3. **The offset** o of §2 is formed.
4. **The first accepted sample seeds** ô = o, and the count since the seed is 1.
5. **Otherwise**, with Δt = max(0, t3 − tLast) and gain a = 1 − e^(−Δt/τ):
   - if |o − ô| > `reseedJumpTicks`, the jump run grows and ô stays; at `reseedRunSamples` in a row ô = o (a re-seed: count
     1, `reseeds()` + 1). An in-band sample resets the run (`OnlyAConsecutiveRunOfJumpsReSeeds`);
   - else the run resets, ô += a · (o − ô), and the count grows.
   The gain follows the time since the last sample, so the filter's time constant is τ seconds whatever the ping rate and
   whatever is lost. A sample whose t3 is not after the last one has Δt = 0 and no weight (`AnOutOfOrderSampleHasNoWeight`).
6. tLast = t3. The code stores it once Δt has been taken from the old value and before either filter runs (§7); nothing
   reads the new value in between. `acceptedSamples()` counts every sample that reached step 3 (accepted plus rejected is
   every sample not ignored), so a window can take its delta; the count since the seed is internal and feeds only `isValid`.

`isValid(nowSeconds)` is `samplesSinceSeed ≥ samplesToValid` and `nowSeconds − lastSampleSeconds() ≤ staleAfterSeconds`,
`samplesSinceSeed` being the count since the seed. A re-seed resets the count, so the estimate is not valid again until
`samplesToValid` samples after it.

## §6 The constants

| field | default | why |
|---|---|---|
| `tickFrequency` | `TimeConfig{}.tickFrequency` (60) | the server's tick rate; the owner passes its `TimeConfig`'s |
| `filterTauSeconds` | 5.0 | noise against lag. On a link with u and d each uniform over 15–50 ms (og-net's shaper b0), one sample's (u − d)/2 has sd 7.1 ms = 0.43 tick; an exponential filter with gain a per sample leaves sd·√(a/(2 − a)), a = 1 − e^(−0.05/5) at 20 Hz: 0.030 tick. Under a clock-rate mismatch δ the offset ramps at δ · f per second and the filter lags it by δ · f · τ: 0.030 tick at 100 ppm (measured 0.0300) |
| `rttGateMultiplier`, `rttGateMarginSeconds` | 1.5, 0.010 | a sample 50 % plus 10 ms above the smoothed RTT is queued; on b0 (RTT 30–100 ms, mean 65) the bound sits near 107 ms, above every honest sample |
| `samplesToValid` | 20 | one second of samples at 20 Hz since the (re-)seed |
| `staleAfterSeconds` | 1.0 | twenty missing pings at 20 Hz |
| `reseedJumpTicks` | 3.0 | 50 ms at 60 Hz: far above one honest sample's error on b0 (at most 17.5 ms = 1.05 tick) |
| `reseedRunSamples` | 5 | a quarter of a second at 20 Hz: a server step loss, or a clock step, re-seeds within it; a stray outlier does not |

## §7 Threads

One writer: the thread that calls `noteSample`, which for the owner in §9 is `NetworkTimeEstimator`'s writer thread (the
thread its `noteSimClockSample` runs on). Every reader-visible value (both offsets, each filter's count since the seed, the
last-sample time and the three counters) is a `std::atomic`, and a `static_assert` requires lock-free 8- and 4-byte
atomics, so a read from another thread is never torn and never takes a lock. The jump runs and the configuration are
touched only by the writer (the configuration is fixed at construction).

**The publication contract: one writer; readers acquire the count, then read the offset.**
- The writer: `noteSample` stores the last-sample time first (relaxed), then runs each filter. A filter that moves its
  offset stores the offset (relaxed) and then its count `samplesSinceSeed` with `std::memory_order_release`; the count is
  the last value each call writes for that filter.
- The readers: `isValid` loads the main filter's count with `std::memory_order_acquire` before the last-sample time;
  `serverTickAt` and `phaseBlindServerTickAt` load their filter's count with acquire before its offset
  (`OffsetFilter::publishedTicks`). Every other load is relaxed.
- What it buys: a reader that sees a count sees the offset and the last-sample time of the call that stored it, or later
  ones. So `isValid` is never true beside an offset that a seed or a re-seed has replaced, such as the initial 0.0. Under
  relaxed ordering alone the C++ memory model allows that, and a weakly ordered CPU (ARM, the initiative's Android
  target) can show it: a count of 20 with an offset about a thousand ticks off. On x86-64 (TSO) an acquire load and a
  release store compile to the same plain moves as relaxed ones, so nothing changes there.

Beyond that, as in `NetworkTimeEstimator`, the values are **not a snapshot**: a reader may combine values from two writer
calls (an offset or a last-sample time newer than the count it acquired; or, after a jump-run sample, which stores no
count, an older last-sample time). The worst outcome is one call's answer from that mix, superseded by the next.

## §8 What the tests measured, and three limits

On the test's seeds (`ServerTickEstimatorTest.cpp`, 2026-11-20): b0-like symmetric jitter with the true age, 600 s, p95 of
|error| 0.063 tick, mean −0.002; 5 % loss plus 10 % single-sample 200 ms bursts, p95 0.074, every burst sample rejected;
±100 ppm for 10 minutes, at most 0.030; a 12-tick step loss re-seeds on the 5th sample after it.

1. **p95 is about 2 σ.** The 0.030-tick figure of §6 is a standard deviation; the p95 of |error| is 1.96 σ ≈ 0.059 tick in
   theory and 0.063 on the test's trace. A bound below that needs a longer τ (the bound scales as 1/√τ).
2. **A re-seed lands on one sample.** ô = o takes that one sample's error, 0.43 tick sd on b0, and τ then takes seconds to
   average it out. The step-loss test's "0.1 tick 0.5 s later" holds on a link without jitter, which is what it uses.
3. **The gate reads the owner's smoothed RTT, which absorbs a burst.** `NetworkTimeEstimator`'s own RTT gate is 4 · R + 30 ms
   at α = 0.15, so a run of 200 ms-delayed samples moves R whenever a burst sample is under that bound (at R = 65 ms it is
   290 ms): after two such samples, each 200 ms above R, R has risen by 200 · (1 − 0.85²) = 55.5 ms and this gate's bound by 83 ms, so later samples
   of a long burst pass it. They then meet the jump test (a 200 ms one-way
   excess is 6 ticks), and only a run of `reseedRunSamples` of them re-seeds, onto a burst sample; the next honest run
   re-seeds back. The tests' bursts are single samples.

## §9 Where it is fed, and what reads it

`NetworkTimeEstimator` owns one (`m_serverTickEstimator`, the defaults with its `TimeConfig`'s `tickFrequency`) and calls
`noteSample` at the end of `noteSimClockSample`'s valid path, for every valid sample at either RTT source, after `updateRTT`,
so the gate reads the smoothed RTT this sample produced (`TimeConfig-rationale.md` §15). Its accessor is
`NetworkTimeEstimator::serverTickEstimator()`. A sample the owner judges invalid (a non-finite stamp, an RTT not above 0)
never reaches it. Reads: none in production yet.
