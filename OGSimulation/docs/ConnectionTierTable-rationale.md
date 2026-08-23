<!-- SPDX-License-Identifier: MPL-2.0 -->
# `ConnectionTierTable.h` — rationale

This is the narrative, the derivations and the deferral history for `ConnectionTierTable`, the
shared tier→behaviour lookups that live beside it, and the relay delay floor helpers
(`Network/ConnectionTierTable.h`). The header keeps the orientation block, every per-declaration
contract and every fence; this file carries everything else. The header is the operational
reference — read it first. Come here when you need the *why*, not the *what*.

**If this file and `Network/ConnectionTierTable.h` disagree, the header is authoritative and this
file is stale.** Fix this file; do not soften the header to match it.

⛔ **This file is not the source of truth for any VALUE.** Compiled defaults live in
`TimeConfig`'s initialisers; the shipped configuration lives in its configuration keys, which are a
separate fact. Where a number appears below it is there to make an argument readable, and every
worked example states the configuration it was computed at.

⛔ **Do not move a fence into this file.** Everything marked ⛔ in the header is a guard at the site
where the mistake gets made.

**Read alongside:** `TimeConfig-rationale.md` — the fields this file's math reads.
`RemoteInputCache-rationale.md` §2 — the receiver-side capacity §5 below takes a minimum over.
`Perspective-RemoteInputFlow.md` — why a relayed input needs a floor at all.

Origin: `og-netcode-v2-input-relay` Stage 3 / D3.4, backlog C1 (the ownership lock, 2026-07-19), C2
(the *replaces* lock), T9 / T10 / T11 / T15, items 62 and 69, the 2026-08-03 review, the design
notes `proposal_ogbrawler_netcode.md` §1.2 / §8.1 / §8.2, `risks_and_plan.md` Stage 5 D5.1 and its
`R-A2` mitigation, `RelayDelaySpectrumDesign.md` §3.2 / §6 / §7 / §10 / §11, `ReviewNotes.md` RN-12,
and the `og-source-doc-extraction` extraction of 2026-08-23.

> ⚠ **Every document named in that Origin line is private working material from the
> `og-netcode-v2-input-relay` initiative archive and is NOT distributed with this submodule.**
> They are named as provenance, deliberately unlinked; every claim this file *asserts* is anchored
> to a file in this repository and only to those. The declarations below tell
> `tools/lint/doc_anchor_lint.ps1` that these names are intentionally unresolvable.

<!-- lint-external-ref: proposal_ogbrawler_netcode.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: risks_and_plan.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: RelayDelaySpectrumDesign.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: ReviewNotes.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: RelayDelayFloorTest.cpp -- test translation unit in the og-simulation-tests submodule, not distributed with this submodule -->
<!-- lint-external-ref: FStandaloneTestHandle -- the Catch2 suite's engine-free connection handle, in the og-simulation-tests submodule -->
<!-- lint-external-ref: RelayDelayFloorTicks -- a configuration KEY, not an identifier: the shipped override for TimeConfig::relayDelayFloorTicks -->
<!-- lint-external-ref: bucketOf -- ILLUSTRATIVE ONLY: the naive lookup the two anti-flap gates replace. It is deliberately not a function in this tree and must not resolve -->

---

## 1. What owns this, and what it borrows

**Ownership — Option A, locked 2026-07-19.** The table is owned by the SERVER only. The authority
derives each connection's tier from its own per-connection RTT and replicates the resulting index to
the owning client; clients do NOT run a second instance of this table and do NOT derive their own
tier.

That keeps the codebase's existing single-source-of-truth shape — server owns the authoritative
quantity, client consumes it — instead of introducing a second, independently-drifting estimator.
Two estimators fed by different RTT observations would disagree by construction, and the disagreement
would show up as per-tick misprediction rather than as an error anyone could see.

⚠ **Nothing in the header enforces that placement.** It is stated so a future reader does not
mistake the type for a symmetric client/server component. The enforcement is that no client-side
code constructs one.

**The production route, end to end**, because *"the server owns it"* does not tell a reader where to
put a breakpoint:

```
  the per-character adapter component        (GAME thread; one adapter's binding is
                                              USimmableUpdateComponent)
    -> ServerReceptionCoordinator::noteRttSample
    -> ConnectionTierTable::onRttSample
```

`ServerReceptionCoordinator` (`Network/ServerReceptionCoordinator.h`) holds the only production
instance, **by value**, constructs it with the shared `TimeConfig`, reaps it on its own cadence, and
exposes it read-only. `ServerInputDelayQueue` (`Network/ServerInputDelayQueue.h`) borrows a
`const ConnectionTierTable*` from it and never mutates it. One adapter instantiates the coordinator
as `ServerReceptionCoordinator<FUEConnectionHandle, ...>`, so a production
`ConnectionTierTable<FUEConnectionHandle>` exists in the shipped build.

**The config is BORROWED, not owned.** One `TimeConfig` instance is shared by the whole
time-management stack, and a copy inside the table could silently diverge from the one the clocks
read. The caller must outlive the table.

**The `Address` contract.** Wire identity arrives as an opaque template parameter so this header can
stay engine-free. The `ConnectionAddress` concept requires a hashable, regular, default-initializable
value type with an `isAlive()` liveness probe — **deliberately the same requirement set `NetConfig<C>`
(`SimulationManagerConcept.h`) enforces on `C::Address`**, restated here as a standalone concept so
this header constrains its own template parameter directly rather than requiring an entire
`NetConfig` to be threaded through. Any `C` satisfying `NetConfig` therefore has a `C::Address`
satisfying `ConnectionAddress`, and a `static_assert` in the Catch2 suite pins that relationship so
the two cannot silently drift.

---

## 2. The shared lookups, and why neither end owns a private copy

Under Option A the tier is derived ONCE, on the server, and the resulting index is replicated. Both
ends must then turn that one integer into the same three Layer-1 quantities — input delay, rollback
ceiling, echo mute — **or the client predicts against a rule the server is not applying and every
tick mispredicts by the difference.**

The free functions in this header are the single source for that math:

| function | what it answers |
|---|---|
| `tierInputDelayTicks` | the effective Layer-1 input delay for a tier |
| `tierRollbackCeiling` | the per-tier soft ceiling for `rollbackWindowTicks` |
| `tierShouldMuteEcho` | whether the render-side input echo is suppressed |
| `tierDelayDeltaTicks` | the delay change across a transition |
| `shouldStallForTierTransition` | whether a transition owes a prediction stall (§8) |

`ConnectionTierTable`'s address-keyed members are thin wrappers over exactly these; the client's
`ReplicatedTierConsumer` (`Network/ReplicatedTierConsumer.h`) calls them directly. The
address-keyed and index-keyed forms therefore compute the identical answer **by construction**, which
is what lets a client with only a replicated index — no table, no `Address` — stay in lockstep with
the server. They take a bare `(tierIndex, cfg)` precisely so that is possible.

**Tier-index clamping exists because the client's tier arrives OFF THE WIRE as a uint8.** A corrupt,
hostile, or version-mismatched value would otherwise index the `TimeConfig` arrays out of bounds. On
the server path clamping is the identity, because `ConnectionTierTable` only ever produces in-range
indices — the clamp is there for the half of the system that does not derive its own value.

**The tier count is derived, never a literal 4.** `kConnectionTierCount` is the extent of
`TimeConfig::rttTierBoundariesMs`, so the count and the config arrays cannot drift apart and adding
a tier is a one-line `TimeConfig` change.

**`tierInputDelayTicks` is the REPLACES value (locked at C2).** It IS the effective delay, and it is
never *added* to the no-tier fallback. The fallback applies only when NO tier is available at all —
`ReplicatedTierConsumer` and `ServerInputDelayQueue::effectiveDelay` each own that condition for
their own end — and since the dedicated no-tier baseline field was retired (§4), the fallback's value
is `rttTierInputDelays[kMaxConnectionTierIndex]`, the WORST tier, floored the same way.

---

## 3. The tier ladder: two gates, an EMA, and a reaper

**Why two independent anti-flapping gates.** Every tier transition changes the player's effective
input delay, which is felt directly as a change in control latency. A naive `tier = bucketOf(rtt)`
lookup flaps on ordinary jitter, so transitions pass two gates that fail for different reasons:

1. **Directional hysteresis** (`tierHysteresisMs`) — a connection promotes only above
   `boundary + hysteresis` and demotes only below `boundary − hysteresis`. This is asymmetric by
   construction: promotion tests the CURRENT tier's upper boundary while demotion tests the
   tier-BELOW's boundary, which leaves a `2 × hysteresis` dead-band straddling every boundary in
   which neither direction fires. That dead-band is precisely what stops boundary-adjacent jitter
   from flapping the tier. It kills fast oscillation *around a boundary*.
2. **Minimum dwell** (`tierMinDwellTicks`) — however far the RTT moves, a connection may not leave a
   tier it entered fewer than N ticks ago. This bounds the transition RATE for a connection that is
   genuinely oscillating over a *wide* range, which hysteresis alone cannot do.

**Both must pass. Neither subsumes the other**, and the two failure modes are different: hysteresis
answers "is this noise around one boundary?", dwell answers "is this connection changing too often
to follow?".

**At most one step per call.** A genuine multi-tier RTT jump walks up one tier per dwell period
rather than teleporting. Deliberate: each step is a player-visible input-delay change, and stepping
keeps that change bounded and monotone.

**The dwell discard is a discard, not a deferral.** There is no pending-transition memory. If the
condition is real it still holds on the next sample after the gate opens; if it was a transient it
correctly evaporates. A dwell-blocked sample reports NO transition — unchanged tier, zero delta — so
the discard is invisible to the caller by design, exactly as it is to `lookupTierIndex`.

**The EMA is seeded with the first sample, never with 0.** Seeding from zero would drag every fresh
connection through a spurious tier-0 phase while the average climbed, and on a genuinely bad link
that phase hands the player a delay far too short for their actual RTT — the worst possible moment
to under-delay. The dwell counter is incremented on EVERY sample including the one that creates the
entry, so a brand-new connection also serves the dwell period before its first transition.

**An `Address` never sampled reports the BEST tier.** Before any evidence of a bad link exists, the
optimistic assumption costs the player the least input delay, and the first sample corrects it.

**Reaping, and why two conditions.** Without `reapDeadHandles` the map grows for the lifetime of the
process, one entry per connection ever seen. `isAlive()` is the prompt signal for a clean
disconnect; the deadline catches a connection that stopped being sampled *without* its handle
reporting dead — a half-open socket, or a handle whose liveness cannot go stale (which is the case
for the Catch2 suite's `FStandaloneTestHandle`).

**`TierSampleResult`'s shape is locked.** The caller needs the resulting tier and the delay delta
and nothing else — no separate up/down booleans, since the sign of `deltaDelayTicks` already carries
the direction and `> 0` is exactly the "delay increased" predicate the transition consumers test. A
sample that produces no transition reports the unchanged tier with a zero delta, so callers never
need to remember the previous tier themselves.

⚠ **But a real transition CAN legitimately carry a zero delta** — two adjacent tiers configured with
the same input delay. Such a transition changes the rollback ceiling and the echo-mute behaviour
while requiring no delay-driven reaction, which is precisely why the *delta* rather than the index
change is the thing reported.

`onRttSample` is deliberately **not** `[[nodiscard]]`: the production sampler and every
arrange-phase loop in the suite drive the table for its state and have no use for the per-sample
outcome. Only a consumer reacting to transitions reads the result.

---

## 4. The relay delay floor: what it is, and the four sites

`TimeConfig::relayDelayFloorTicks` is a SESSION-scoped minimum on the effective Layer-1 input delay,
independent of the per-wire tier.

**Why the tier is the wrong instrument here.** The tier covers the SENDER's uplink — arrival margin
at the server — while a peer scheduling a *relayed* input needs the RECEIVER's round trip covered.
The floor is that receiver-coverage budget, and it is session-wide because every receiver must be
able to schedule every sender: a per-wire quantity cannot express "B must be able to schedule A".

**The rule is a MAX, never a SUM, and it lives in one place.** Every production site that derives an
effective input delay routes through `applyRelayDelayFloor`. **A missed site is a divergence bug**,
not a cosmetic gap: the server parks the input at its own effective delay while the client predicts
at a different one, so every tick mispredicts by the difference.

**There are FOUR such sites.** The count was corrected by the 2026-08-03 review, which found the
design corpus and the backlog each naming a different, incomplete *three* — exactly the mistake the
single helper exists to prevent:

| # | site | shape |
|---|---|---|
| 1 | `tierInputDelayTicks` | the sole production reader of `rttTierInputDelays[]`, serving BOTH the server table and the client consumer |
| 2 | `ServerInputDelayQueue::effectiveDelay` | its no-tier fallback |
| 3 | `ReplicatedTierConsumer::effectiveInputDelayTicks` | its no-tier fallback |
| 4 | the composition-root pre-tier baseline publish | derives nothing of its own since the T10 tier-channel migration — it publishes site 3's answer, so it is floored by construction |

**The completeness guard is STRUCTURAL, not a grep, and that changed.** The dedicated no-tier
baseline field that sites 2 and 3 used to read is RETIRED (item 62 / RN-12); both now read
`rttTierInputDelays[kMaxConnectionTierIndex]` directly. So the old guard — *"grep the retired
field's reads"* — no longer applies, because there is no second field to grep for. What is countable
instead is the number of sites routing through `applyRelayDelayFloor`: still 4, with sites 1–3
calling it directly and site 4 transitively through site 3.

**Precedence over the LAN override.** `lanZeroDelayOverride` collapses tier 0 to zero delay: on a
sub-millisecond local link there is no round trip left to hide, so the configured delay is pure added
lag. Only tier 0 is affected — a bad connection inside a LAN session still gets its own tier's delay.
**The floor is applied AFTER the override branch, so a nonzero floor DOMINATES the override**
(`max(floor, 0) == floor`). That ordering is the point, not an accident: on a mixed session a LAN
sender must still be schedulable by WAN receivers. It is documented at `TimeConfig`'s own
`lanZeroDelayOverride` too.

**At a floor of 0 the application is the identity** for every non-negative base, which is what makes
the whole feature ship degenerate — floor 0 behaves exactly as the pre-floor build did.

⛔ **The compiled default and the shipped configuration are two separate facts.** `TimeConfig`'s
initialiser is the compiled default; the shipped value lives at the `RelayDelayFloorTicks` key. Read
the key; do not infer the shipped value from an initialiser or from a sentence.

**Clamping runs at both intakes and again on every read.** `clampRelayDelayFloorTicks` is called at
the composition-root configuration intake, at the client's floor replication callback, and once more
inside `applyRelayDelayFloor`. That belt-and-braces shape is deliberate and is the same one
`clampConnectionTierIndex` uses for the replicated tier: the value arrives off the wire as a uint8,
so "clamped at intake" alone would leave a corrupt or version-mismatched byte able to break the
regime if any *future* intake point forgot to clamp.

---

## 5. The hard cap, derived from two capacities

`relayDelayFloorHardCapTicks` is the absolute ceiling for an effective floor. It is DERIVED, not a
literal, and derived from the **smaller of two ring capacities** — because a configured floor delays
the read on BOTH ends of the relay and each end holds the value in a ring of its own.

**Sender side** (`LocalInputCache`, `kLocalInputCacheCapacityTicks`). The local capture is pushed AT
capture, read at `capture + floor`, and revisited by a resim up to `rollbackWindowHardCap` ticks
further back. It must therefore survive `floor + rollbackWindowHardCap` ticks of pushes.

**Receiver side** (`RemoteInputCache`, `kRemoteInputCacheCapacityTicks`). The relayed entry for
capture tick `c` enters the store at arrival (~`c + wire`), is consumed by the scheduled read at
`c + dA`, and must stay resident for resim reads until the frontier passes
`c + dA + rollbackWindowHardCap`. The store is fed at up to one entry per tick, so `c` is evicted at
about `c + capacity + wire`. Residency therefore needs `dA + rollbackWindowHardCap ≤ capacity + wire`
— **the same inequality as the delay line's, with `wire = 0`.** It is not a different quantity; it
is a bound that merely happens not to bind while both capacities are equal and the wire slack is
non-negative.

⛔ **So the minimum is the point, not a flourish.** Halve the receiver capacity and a
delay-line-only derivation would still admit a floor that puts scheduled entries beyond eviction
before their resim window closes — **the scheduled regime degenerating SILENTLY into permanent
fallback reads**, which is precisely the failure this cap exists to make impossible. A comment
coupling the two capacities would have been the silent-drift hole; the derivation closes it.

**Why the derivation is split into `relayDelayFloorHardCapForCapacities`.** Both capacities are
`constexpr` constants, so a test can only prove *"the cap follows the store capacity down"* by
feeding a different capacity in. The split exists to make the claim testable at all; a pathological
`rollbackWindowHardCap` must not produce a negative cap, so the result is floored at 0.

`RelayDelayFloorTest.cpp` is the pin.

---

## 6. The floor advisory, and why it is not an assert

`classifyRelayDelayFloor` classifies the CONFIGURED floor **for startup logging only**.

⛔ **It is never an assert and is never used to reject or correct a value.** A
`relayDelayFloorTicks` of 0 is the documented "scheduled regime OFF" mode, so a hard check on
`floor < 2` would forbid a mode this codebase deliberately supports. Correcting a value is
`clampRelayDelayFloorTicks`'s job, and it always runs first. The callers — the composition-root
configuration intake and the client's floor replication callback, the same two sites that clamp —
decide how loudly to log each case.

| configured floor | advisory | why |
|---|---|---|
| 0 | `None` | the documented off mode; silent |
| 1 | `BelowHiccupBaseline` | below the hiccup-absorption baseline, above off — the one value that is neither. WARN. |
| 2 .. < `max(rttTierInputDelays)` | `None` | an ordinary scheduled-regime floor; silent |
| ≥ `max(rttTierInputDelays)` | `UniformDFairnessActive` | every derivation path — tier, LAN override, and the no-tier fallback — collapses to this one value. A NOTE, not a warning: it is a legitimate configuration, and it was **invisible in the logs until this advisory existed**. |

**`UniformDFairnessActive` is checked BEFORE `BelowHiccupBaseline`.** Under a tier table whose best
tier carries a nonzero delay the two conditions cannot both hold, so the ordering only decides the
degenerate case where every tier delay is ≤ 1 — and it decides it in favour of the stronger, more
informative advisory rather than leaving it unspecified.

**`maxRttTierInputDelay` is computed, not assumed to be the last array entry**, so it stays correct
independent of the monotonic-non-decreasing invariant `TimeConfigTierArrayOrderingTest.cpp` pins. That
the two agree today is itself asserted by a test rather than assumed here.

⚠ **Uniform-D fairness is reachable on a shipped configuration**, and when it is, every tiered
derivation collapses to the floor. That is not a bug — it is what a session-wide receiver-coverage
budget larger than every sender-coverage budget *means*. It does, however, mask §8's transition
arithmetic entirely, because if every tier maps to the same effective delay then every transition
delta is zero. §8's worked table therefore states the configuration it was computed at.

---

## 7. Intended API, no production caller

Four surfaces here and next door are defined and tested but have **no production caller**:
`tierRollbackCeiling`, `ConnectionTierTable::lookupRollbackCeiling`,
`ReplicatedTierConsumer::effectiveRollbackCeiling`, and both `shouldMuteEcho` forms (plus
`tierShouldMuteEcho` behind them). Only the Catch2 suite calls them.

**They are not dead code to delete, and the reasons differ.**

- **The rollback ceilings.** The client-side soft resim clamp that would consume them is INTENDED,
  NOT IMPLEMENTED. `TimeConfig::rttTierRollbackCeilings` carries the same statement at its own
  declaration, which is the authoritative one for the field. Wiring is deferred to the sparse-state
  increment. Keeping the lookup here is what stops a future consumer re-deriving the tier math at
  its own call site — which is the failure §2 exists to prevent.
- **The echo mute.** Its wiring task is *optional*, not merely deferred. The query ships here so the
  policy lives with the tier state rather than being re-derived at a call site later.

⚠ **"No production caller" is not "nothing reads it".** All three surfaces *do* read their
`TimeConfig` fields; what is absent is a caller outside the test suite. That distinction matters
because a sweep for readers of `muteEchoOnDegradedTier` finds three and concludes the field is live.
`TimeConfig-rationale.md` §10 tabulates the three shapes of "no consumer" this codebase uses.

---

## 8. The prediction-stall decision, and the `hadAnyTier` fix

`shouldStallForTierTransition` answers: does this wire tier transition owe the client a prediction
stall?

**The quirk it has to survive.** The `oldTier` an adapter's tier-arrival callback receives is the
replicated property's value BEFORE that callback — but on a fresh connection's FIRST real arrival,
that "before" value is the property's **compiled default (0)**, not a tier the client was ever
actually running at. Before the first arrival the client runs the pre-arrival no-tier fallback
(`rttTierInputDelays[kMaxConnectionTierIndex]`, floored — see `ReplicatedTierConsumer`'s PRE-ARRIVAL
FALLBACK note), never tier 0's delay.

⛔ **So `hadAnyTier` must be PASSED, never inferred from `oldTier`**, because 0 is both the property
default and a legal tier. It says whether the caller had ALREADY received an authoritative tier
before this call.

**One adapter's bindings for the three roles this section names**, so a reader can put a breakpoint:
the tier-arrival callback is `ISimulationConnectionRelayListener::onConnectionTierReceived`, the
late-join entry point is `onConnectionTierReplayed`, and the stall applier is
`applyTierTransitionStall` — all three in the `OGBrawlerUnreal` adapter subtree. Another adapter
substitutes its own; every claim here is about those ROLES.

### The worked table

⚠ **Computed at `rttTierInputDelays = {1, 2, 3, 4}`, `lanZeroDelayOverride = false`, and a relay
delay floor of 0.** At a floor that dominates the tier table (§6) every effective delay collapses to
the floor, every delta is 0, and the defect below is masked rather than absent — which is exactly
why it survived unnoticed. Re-derive before quoting these numbers against another configuration.

The client is on its FIRST real tier arrival, so it has been running the no-tier fallback,
`rttTierInputDelays[3] = 4`. The callback reports `oldTier = 0` (the property default).

| `newTier` | delay the client was ACTUALLY running | new effective delay | TRUE delay change | `tierDelayDeltaTicks(0, newTier)` — what the pre-fix code asked for | pre-fix stall | correct stall |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 4 | 1 | **−3** | 0 | 0 | 0 |
| 1 | 4 | 2 | **−2** | +1 | **1** | 0 |
| 2 | 4 | 3 | **−1** | +2 | **2** | 0 |
| 3 | 4 | 4 | **0** | +3 | **3** | 0 |

⇒ **For every `newTier` except 0 the pre-fix code requested a multi-tick prediction stall for a
transition whose true delay change was zero or NEGATIVE** — a stall for a transition that made the
client's delay *shorter*. Wrong quantity, wrong direction, on every ordinary client's first tier
resolution.

With `lanZeroDelayOverride = true` it is worse: tier 0's effective delay becomes 0, so
`tierDelayDeltaTicks(0, 1)` reads +2 rather than +1 and the spurious stall grows.

### The fix, and its three early-outs

**FIRST-EVER RESOLUTION → ALWAYS ZERO**, regardless of `newTier`. Nothing was predicted against a
previous TIER's delay, only against the fallback. This is exactly the reasoning the *replayed*-tier
entry point already used for late joiners (no stall: nothing was predicted against a previous
tier) — a first real arrival is the same situation, arriving through the other door.

**The `oldTier == newTier` early-out is preserved.** A replication callback can fire for an unchanged
value, and nothing transitioned.

**Only a genuine (`hadAnyTier == true`) transition reaches `tierDelayDeltaTicks`, and only a POSITIVE
delta is ever returned.** A downward or delay-neutral transition needs no correction — the client
may now predict FURTHER ahead, which the ordinary drift path reaches by advancing normally, a natural
extension rather than a stall. This matches the caller's pre-existing *"non-positive deltas are
dropped"* contract, and `ClientPredictionClock::requestInputDelayIncreaseStall` is also a no-op below
zero, so this is belt-and-braces rather than the only guard.

### Why `tierDelayDeltaTicks` must go through `tierInputDelayTicks`

⛔ **Never as a bare `cfg.rttTierInputDelays[to] - cfg.rttTierInputDelays[from]`.** With
`lanZeroDelayOverride` set, tier 0's effective delay is 0 rather than `rttTierInputDelays[0]`, so the
raw-array form reports the wrong delta for **every transition that touches tier 0** — which is the
most common transition there is. The suite pins this with a dedicated case plus a verified negative.

**A floor that dominates both endpoints collapses the delta to 0, and that is CORRECT**, not a
swallowed transition: with the floor dominating, the player's felt delay does not change across the
transition, so there is no prediction-stall debt to pay. Publishes are unaffected — the server's
publish predicate compares tier INDICES, not deltas (`ServerReceptionCoordinator`), so the client
still learns the new tier.

**A standing divergence this does NOT fix, stated so it is not rediscovered as a regression.** The
core never publishes tier 0 as a FIRST value, so a wire that never leaves tier 0 produces no publish
and no call into the client tier path at all: the client sits on the pre-arrival fallback while the
server parks at tier-0 delay. That divergence is today's behaviour, is preserved deliberately, and
changing its cause changes felt input lag. `hadAnyTier` only suppresses the spurious *stall*; it
does not touch the divergence.

---

## 9. R0 corrections — claims this header made that the tree does not support

Every factual claim in the pre-compression header was checked against the tree before it was
compressed. **Compression makes a false statement shorter, denser and more authoritative**, and the
coverage arms, the subject gate and the doc-anchor lint all pass it, because they verify that a
symbol exists and never that a sentence is true.

### F-38-3 — "NO CONSUMER YET" was false, and the file contradicted itself 90 lines later

The header block read:

> *"NO CONSUMER YET. As of Stage 3 nothing outside this directory constructs or calls a
> ConnectionTierTable; the wiring lands in Phase B (T9/T10). The type is delivered standalone with
> test coverage so the escalation policy can be reviewed on its own, ahead of the tick-loop
> integration that depends on it."*

**The wiring landed.** `ServerReceptionCoordinator` owns a `ConnectionTierTable` by value,
constructs it, drives `onRttSample` from `noteRttSample`, reaps it, and exposes it;
`ServerInputDelayQueue` borrows it; the adapter's per-character component calls `noteRttSample` from
production code; and one adapter's composition root instantiates the coordinator with a real
connection handle. §1 has the route.

⚠ **And the same file already said so.** The relay-delay-floor block, ~90 lines below, reads *"since
the T10 tier channel migration"* — past tense, about the very migration the header block calls
future. **A self-contradiction inside one file**, which is the recurring defect class this
initiative keeps re-finding and which no mechanical sweep in it can see.

The orientation block now states the production route positively. "No consumer" survives only where
it is still true and is scoped to the specific surface (§7).

### F-38-4 — a quotation of a sentence that no longer exists

The floor-validation block justified itself by quoting `TimeConfig`'s own comment:

> *"(see the field's own comment: `"Default: 0 (degenerate — today's behaviour, byte-for-byte)"`)"*

**That sentence is gone.** `TimeConfig.h`'s 40 `Default: N` restatement lines were removed in the
2026-08-23 extraction of that file, deliberately, because a `Default:` comment beside an initialiser
is a second source of truth. A quotation is a join like any other, and it broke the moment the quoted
text was edited. The argument survives without it: the off mode is documented by the field's guards
and by this section's table.

### F-38-5 — a pointer to a block that was replaced

`tierRollbackCeiling` and `lookupRollbackCeiling` both cited *"TimeConfig.h ADR status note"*. There
is no ADR block in `TimeConfig.h`; the 183-line one was replaced by an orientation block in the same
2026-08-23 extraction. **A repository-wide search for `ADR` in that file returns zero.** The fact
survives at the declaration of `TimeConfig::rttTierRollbackCeilings`, whose own guard states
`INTENDED API, NO PRODUCTION CALLER`, so the pointer is now anchored on the DECLARATION rather than
on a block title.

### F-38-6 — a citation loop, and neither end had the table

`shouldStallForTierTransition`'s block said the full worked table was elsewhere:

> *"(`SimulationManagerUImpl.cpp`'s PRESERVED QUIRK note has the full worked table)"*

The PRESERVED QUIRK note exists. **It has no table.** And at the commit this extraction was taken
from, that note said the table was *"in Backlog.md item 69 and in `shouldStallForTierTransition`'s
own doc comment, ConnectionTierTable.h"* — i.e. **here**. Each end pointed at the other; neither held
it; and the only home either of them named that could have held it is an initiative-workspace file
that is not distributed with this repository at all.

**Closed by writing the table**, at §8, with the configuration it was computed at stated above it.
This is the sharper half of the finding: an anchor that RESOLVES can still be wrong about what is at
the other end, and no anchor lint can see that — the lint asks whether the name exists, not whether
the described content is there.

**Verified TRUE and left alone** (recorded so a reviewer need not re-derive them): the FOUR
`applyRelayDelayFloor` sites and their transitive shape; the publish predicate comparing tier
indices; `lanZeroDelayOverride` being documented at its own definition; `rttTierInputDelays[0] == 1`;
`64 − 20 = 44` at the current `rollbackWindowHardCap`; `TimeConfigTierArrayOrderingTest.cpp` existing and
asserting what is claimed; `ReplicatedTierConsumer`'s PRE-ARRIVAL FALLBACK note; and
`UniformDFairnessActive` being reachable on the shipped configuration — floor 6 against a maximum
tier delay of 4.

---

## 10. Provenance — the workspace citations this header used to carry

The pre-compression header carried 32 comment lines bearing initiative-workspace tags. None resolve
for a reader with no initiative workspace, which is what the standalone-truth rule forbids. They are
recorded here once, as provenance, and are not repeated in the header:

| tag | what it introduced |
|---|---|
| Stage 3 / D3.4 | the type itself and its escalation policy |
| backlog C1 | the Option A ownership lock, 2026-07-19 (§1) |
| backlog C2 | the *replaces*, never *adds*, lock on the tier delay (§2) |
| R-A2 | the two-independent-gates mitigation (§3) |
| T9 / T10 | the tier channel and its migration; the wiring §9's F-38-3 says had not landed |
| T11 | the relay delay floor (§4) and the shared application helper |
| T15 | the render-side echo mute — still OPTIONAL (§7) |
| item 62 / RN-12 | the retirement of the dedicated no-tier baseline field (§4) |
| item 69 | the `hadAnyTier` prediction-stall fix (§8) |
| review finding A5 / AM-3 | the hard cap, widened to BOTH capacities on 2026-08-04 (§5) |
| the 2026-08-03 review | the correction from three floor sites to four (§4) |
