<!-- SPDX-License-Identifier: MPL-2.0 -->
# `ServerReceptionCoordinator.h` — rationale

The derivations, the rejected alternatives, the wire contracts and the correction record behind
`ServerReceptionCoordinator`. The header keeps every fence, the per-declaration contracts and a
short orientation block; this file carries everything else. The header is the operational
reference — read it first, and come here when you need the *why*, not the *what*.

**If this file and `ServerReceptionCoordinator.h` disagree, the header is authoritative and this
file is stale.** Fix this file; do not soften the header to match it.

⚠ **§9 IS A CORRECTION RECORD, AND SOME OF THE SENTENCES IT QUOTES ARE FALSE ON PURPOSE.** Eight
claims that stood in this header were checked against the tree and found wrong, stale or
over-stated. §9 quotes each one so the correction is legible; nowhere else in this document restates
a quoted sentence as fact.

<!-- lint-external-ref: ASimulationConnectionRelay -- ENGINE-SIDE ADAPTER ACTOR of ONE adapter (Unreal), named in Sec 4 and Sec 9 as that adapter's transport for the connection tier; another adapter substitutes its own and this token means nothing there -->
<!-- lint-external-ref: bOnlyRelevantToOwner -- ENGINE REPLICATION FLAG of ONE adapter, named in Sec 4 because it is what narrows the tier to the owning client after the property moved off the component -->
<!-- lint-external-ref: m_replicatedConnectionTier -- RETIRED SYMBOL: the component property the tier used to ride, removed when the tier moved to the connection relay. Quoted in Sec 9 precisely because it must NOT resolve -->
<!-- lint-external-ref: COND_OwnerOnly -- ENGINE REPLICATION CONDITION of ONE adapter, named in Sec 9 as the mechanism that was removed; it has no counterpart in this repository -->
<!-- lint-external-ref: GetPlayerSlotForActor -- ENGINE-SIDE ADAPTER HELPER of ONE adapter, named in Sec 2 as the source of the playerSlot primitive the core cannot derive -->
<!-- lint-external-ref: FInputRedundancyBundle -- ENGINE-SIDE WIRE TYPE of ONE adapter, named in Sec 4 as the production Buffer binding beside the suite's; the core never names it -->
<!-- lint-external-ref: ASimulationManagerUImpl -- ENGINE-SIDE COMPOSITION ROOT of ONE adapter, named in Sec 1 as that adapter's owner of this type and again in Sec 9 -->
<!-- lint-external-ref: USimmableUpdateComponent -- ENGINE-SIDE PER-CHARACTER COMPONENT of ONE adapter, named in Sec 4 as that adapter's ConnectionTierSink binding -->
<!-- lint-external-ref: FUEConnectionHandle -- ENGINE-SIDE Address BINDING of ONE adapter, named in Sec 7 beside the suite's FStandaloneTestHandle -->
<!-- lint-external-ref: Unreal -- ENGINE VENDOR NAME, quoted in Sec 9 as one of the three literal grep TOKENS the falsified sentence promised would return nothing; it names no symbol in this repository -->
<!-- lint-external-ref: getServerReceptionTick -- ENGINE-SIDE ADAPTER ACCESSOR of ONE adapter, named in Sec 5 as the tick source the gate deliberately does NOT use -->

---

## §1 — What it is, what it owns, and the construction order that is load-bearing

`ServerReceptionCoordinator<Address, SimulatableTs...>` owns the server's per-connection reception
state and the policy that orchestrates it. The per-connection primitives — `ConnectionTierTable`,
`ServerInputDelayQueue`, `ConnectionSlotKey` — were already engine-free; what was not, until this
type existed, was their **ownership** and the logic that sequences them: RTT sample → tier
derivation, park input, drain due input, reap dropped connections. That logic sat in the engine
adapter, and leaving it there is what blocked a second engine from reusing the reception path at
all. The adapter is now a thin transport layer that acquires four engine primitives — an address,
a player slot, an RTT reading and a sim tick — decodes the wire, and forwards.

**It is a separate type from `SimulationNetSync` on purpose.** `SimulationNetSync` is
client-and-server mixed and physics-thread adjacent. Everything here is authority-only and
game-thread-only. Keeping the two apart is what makes that contract legible rather than a comment
somebody has to remember.

### Ownership and lifetime

The coordinator owns the tier table and the delay queue **directly** — they were `std::optional`
members on the adapter's manager before. The queue borrows the tier table by reference, so:

* the table is **declared** before the queue, and therefore constructed before it;
* the queue is destroyed before the table, by reverse member-destruction order;
* **member-declaration order is the only thing enforcing either.** There is no assertion. That is
  why the rule is fenced twice in the header — once at the constructor, once at the members.

Both borrow `const TimeConfig&`, so the coordinator must not outlive the config. In the one adapter
that exists today — `ASimulationManagerUImpl`, that engine's composition root — the whole coordinator
is a `std::optional` emplaced inside `BeginPlay`'s authority branch, after the core manager whose
`TimeConfig` it borrows, and reset in `EndPlay` **before** that manager. The adapter's own comments
state the borrow rule at both ends. A second engine binds its own root and this name means nothing
there.

---

## §2 — The three exit paths, and why only one of them is the normal one

A received input leaves `receiveRemoteInput` on exactly one of three paths, and the whole
`ReceiveRemoteInputResult` struct exists because two of them look the same to a naive caller.

| result | meaning | what the caller must do |
|---|---|---|
| `parked == true` | queued in the delay queue; released on `captureTick + effectiveDelay` | nothing |
| `parked == false`, `rejectedOutOfDomain == false` | the **malformed-slot fence**: the slot is outside the uint8 substitution-mask range | **deliver it undelayed**, or that player input is lost |
| `rejectedOutOfDomain == true` | the receipt gate refused the capture tick (§5) | **discard it** — falling back defeats the gate |

`playerSlot` is a parameter rather than something the core derives, because it is an engine
primitive: in the one adapter that exists today it is the child-connection id read by
`GetPlayerSlotForActor`, and a second engine would supply its own.

The middle row is the only in-core false path that still delivers. A slot outside
`ConnectionSlotKey::kMaxPlayerSlot` means a malformed connection topology, not a supported
configuration, so it is warned about rather than silently accommodated — but the input still
reaches the simulation, because losing player input to a topology complaint is the worse failure.
The warning is one-shot per `(id, slot)` pair rather than per tick: the un-throttled predecessor of
this warning produced **28,192 lines / 6.4 MB in 94 s** of a single session.

### The drain, and the capture tick it delivers

`releaseDelayedInputs` runs from the adapter's pre-physics-step hook — `releaseDelayedInputsForStep`
in the one adapter that exists today. The adapter supplies `firstUpcomingSimTick` and `numSteps`
because it owns the game-thread-safe tick source; the core never reads a physics-thread clock.

**The delivered capture tick is the entry's STORED `captureTick`, never a reconstructed
`simTick - delay`.** Under due-or-overdue release an overdue entry is released a tick or more late,
so `simTick - delay` would name a *future* input's tick. That collides with `RemoteMoveQueue`'s
capture-tick dedup and makes the `[InputGap]` watermark meaningless. The delay is expressed purely
as *when* the release fires; lateness is reported separately as `late=N`.

`staleBefore = firstUpcomingSimTick - rollbackWindowHardCap` is computed once and used for **both**
the release gate and the purge. That single value is what makes the purge the one drop point:
`tryDequeueForTick` never releases an entry the purge would reclaim, and the purge reclaims exactly
what the release gate skipped. Early in a session it can be non-positive, where it gates nothing
(capture ticks are non-negative) and the purge is skipped entirely.

---

## §3 — The claim map is id-keyed, and what that costs

The delivery-routing map stores `ConnectionSlotKey -> id` — a plain simulatable id, not an engine
object handle the core could not own anyway. The consequence is that **GC liveness of the owning
component is not readable here**, so component death has to arrive by two other routes:

1. `forgetOwner(id)`, called from the adapter's unregister path — the same lifecycle seam
   `SimulationNetSync::unregisterSimulatable` rides on the core side. This is the prompt route: a
   component's claim, its capture-tick watermark, its last-published tier and its `[InputGap]`
   watermark all drop at unregister rather than waiting for an engine handle to go stale.
2. The `deliver` callback returning `false` during the drain. This is the backstop for a component
   that was collected without an unregister; the claim entry is dropped in place.

**Wire** death is different and is handled here, by `reapConnections`, off the `Address` half of the
key only. A dead wire drops every one of its slots, which matches what the delay queue's own reap
does.

The claim write is a plain overwrite. Re-registering the same id is a no-op; an id legitimately
replacing a dead one on the same slot — respawn, seamless travel — takes the slot over. Two
characters on one machine are two distinct keys, so there is nothing for the overwrite to conflate.

---

## §4 — The tier path: sample once, dedup per owner, and the send boundary

### Once per bundle, never per slot

A bundle is one datagram and therefore one arrival event, and the transport's round-trip estimate
only advances on ack receipt anyway. Sampling per *slot* would feed the same reading into the tier
EMA up to `kMaxSlots` times and couple the effective smoothing rate to redundancy depth. `rttMs`
stays `double` because the tier table's `onRttSample` takes a `double`; narrowing to `int32` would
shift every EMA update sub-millisecond, which is a silent change in tier behaviour, not a rounding
detail. The tier table's own EMA is the only smoothing in the chain.

### The send is fired from inside, not left to the caller

The tier send used to be a two-step the adapter could half-complete: derive the tier here, then
separately call a publish method. Nothing forced the second call, and the *no-reading* skip and the
publish-only-on-change dedup both leaked out to the adapter. Inverting it so `noteRttSample` fires
the send makes "derived but never sent" structurally impossible, and both policies moved into the
core with it.

### Why the dedup is keyed on `ownerId` and not on `Address`

Two couch-coop characters can share **one** root connection — one `Address` — while each owns its
own tier state on the far side. A per-`Address` last-published entry would publish a transition for
the first sibling and skip the second forever. Keying on `ownerId`, the *sink target*, makes both
siblings converge. It is also symmetric with the per-id capture-tick watermark and is cleared in the
same place, `forgetOwner`.

The same bug reappears one decision later, which is why the header fences it twice. `onRttSample`
reports the wire's **current** tier on every call, and the *transition* it signals belongs to
whichever owner's sample happened to cross the dwell gate. A sibling that did not trigger the
transition still sees `newTierIndex` equal to the new tier and, having last been told 0, must be
published to. Gating the publish on "did *this* sample transition" starves that sibling.

A missing map entry means "never told", whose baseline is the far side's default of 0 — so a first
sample that derives tier 0 correctly publishes nothing.

### The sink is a concept, and what one adapter binds it to

The core cannot name an engine type, so the send target is a compile-time concept: any type that can
transport a `(id, tier)` to the owning client satisfies `ConnectionTierSink`. `id` identifies the
target entity, so a sink that is a central manager rather than the entity itself can route on it.
A sink missing the method is a compile error at the `noteRttSample` call site, the same way the
buffer-owner concepts in `SimulationNetSync.h` fail at theirs.

The one adapter that exists today binds it to `USimmableUpdateComponent`, which does **not** hold the
tier itself. It forwards to `ASimulationConnectionRelay`, a per-connection actor whose tier property
replicates unconditionally and is narrowed to the owning client by `bOnlyRelevantToOwner`
**relevancy** rather than by a replication condition. §9 records the correction; the operational
point is that split-screen siblings resolve to the same root connection and therefore to the same
relay, so the sibling-starvation class the core's per-owner dedup guards against is also structurally
impossible on the transport side.

### `receiveInputBundle` — the loop, and what deliberately stays outside it

`receiveInputBundle` is the whole per-slot loop: decode the bundle with the redundancy codec, park
each slot through `receiveRemoteInput`, and on the one non-parked path deliver immediately. `wire` is
any type satisfying the codec's Buffer concept (`bundleByteNum`, `readFromBuffer` and the rest) — in
production `FInputRedundancyBundle`, in the Catch2 suite a `std::vector`-backed test buffer, and the
core names neither.

Two things stay outside on purpose. The **no-wire early-out** is the adapter's: it only reaches this
method when it has a live wire and a coordinator, so the core handles only `hasValidSlot`. The
**once-per-bundle RTT sample** is also the adapter's, immediately before this call, because this
method is purely the slot loop.

`relay` is threaded straight through to `receiveRemoteInput`'s tap and this method adds no relay
policy of its own. Production passes the *same object* for both sinks — the adapter's manager binds
delivery and relay — but they stay separate parameters because they are separate boundaries: one
routes an input into this simulation, the other forwards it out to everyone else.

---

## §5 — The out-of-domain receipt gate

### What it rejects, and why the relay made it necessary

A capture tick outside `[serverTick - rollbackWindowHardCap, serverTick + hardResyncThresholdTicks]`,
both bounds inclusive, both sourced from `TimeConfig` — there is deliberately no literal on this
path.

A client that is warming up or free-running has not yet been anchored to the server's tick
numbering: it emits capture ticks from its own counter (0, 1, 2 …) while the server is at 600+.
Before the relay existed that garbage was merely parked and later purged, so it was invisible past
the drain. Once receipt-time input became peer-visible through the relay, an unfiltered garbage tick
is broadcast to every other client and resolved against *their* timelines. The gate exists so that
nothing outside the server's own tick domain ever reaches the relay tap.

### Where the two bounds come from

* The **lower** bound is exactly the drain's `staleBefore`, so the whole reception path has one
  window semantics. An out-of-window receipt is rejected up front instead of parked-then-purged;
  both mean "too old to matter".
* The **upper** bound is the failsafe drift threshold. A client legitimately captures *ahead* of the
  server — that is the entire prediction offset — and steady-state lead is roughly
  `2 · jitter · frequency` plus the soft dead band, comfortably inside the threshold at any playable
  RTT. Beyond it the client is further adrift than the hard-resync backstop tolerates, i.e. it is
  about to be snapped anyway.
* `hardResyncThresholdTicks > rollbackWindowHardCap` keeps the window from being empty or inverted.
  That inequality is asserted by `TimeConfigOrderingTest.cpp`, which pins it at the compiled
  defaults; nothing clamps it at runtime, and neither field is settable from a configuration file
  today.

### Why it runs before the dedup watermark

`noteCaptureTick` keeps a **monotonic max** per owner id. One garbage tick far in the future would
raise that watermark permanently, and every subsequent legitimate input would then report
`acceptedNew == false` — silently suppressing the `[Park]` trace and the relay write itself. The
gate must run before the watermark is touched, not merely before the park.

### The tick reference, and why there is exactly one feed

The gate judges against the current server tick, but `receiveRemoteInput` and `receiveInputBundle`
are per-slot payload calls that do not carry one. The server tick is an engine primitive, and the
adapter already hands one in every physics frame through `reapConnections(serverTick)` — which
therefore records it. No adapter signature changed to arm this gate.

`noteRttSample` also receives a `serverTick` and could plausibly feed the same reference. It is
deliberately **not** wired in, for two reasons:

1. **Source quality.** `reapConnections` is called from the same game-thread hook as the drain and
   is passed the tick↔physics-frame mapper's `firstUpcomingSimTick` — the tick source documented as
   game-thread-safe. `noteRttSample`'s tick is `getServerReceptionTick()`, an acknowledged wart: an
   unsynchronized read of the physics-thread-written server clock. A gate that *discards* player
   input should judge against the safe source, not the racy one.
2. **A monotonic-enough reference.** Two feeds a tick apart could hand the gate a reference that
   jitters backwards between consecutive receipts, making an input sitting exactly on a boundary
   accepted or rejected depending on which feed wrote last. One feed, one cadence, no jitter.

`noteServerTick` is nevertheless **public**, so an adapter or the suite *could* arm it explicitly.
Nothing does: both arm through `reapConnections`. §9 records that as a precision correction rather
than a defect — the entry point is a seam, not a claim about use.

The write is **last-write-wins, not a monotonic max**: a max would stick forever if the clock ever
restarted (a session restart, seamless travel), permanently rejecting every subsequent input. And
the gate **fails open until armed**: before the first call there is no reference, so it accepts
everything. A gate that failed *closed* with an unset reference would silently discard every player
input on any path that forgot to feed it. `m_serverTickKnown` is what makes "never armed"
distinguishable from "armed at tick 0" — and it is also what keeps every unit case that never drives
a tick behaving exactly as it did before the gate existed.

The cost is that the gate is unarmed until the first physics frame, a window in which the server
tick is near 0 and warm-up capture ticks are legitimately in-domain anyway.

### The second guard, kept deliberately

`RemoteMoveQueue::queueMove` (`SimulationQueues.h`) already rejects
`captureTick > serverAuthorityTick + rollbackWindowTicks` — a **tighter** bound — at the
physics-thread *delivery* layer, with its context published per authority tick through
`setAuthorityGuardContext`. The two do not conflict and neither subsumes the other: this one guards
**receipt** on the game thread, and therefore guards the relay tap; that one guards **delivery** into
the simulation.

On the parked path the queue guard is effectively dead — drained entries are due-or-overdue, never
ahead of authority. It fires only on the fallback paths, where the accepted composite behaviour is
that an input can pass this gate and then be discarded by `queueMove` as too-far-future. That is
documented, not silent.

### The reap cadence, and the boundary it can step over

`reapConnections` runs **once per physics frame**, from the same hook as the drain and immediately
after it. Its predecessor fired only when a bundle arrived on a dwell-boundary tick, so an idle
server never reaped at all — a latent leak. This one runs regardless of traffic; the internal
`serverTick % tierMinDwellTicks` gate keeps the frequency on a busy server the same as before.

⚠ **A frame simulates `numSteps` sim ticks**, so consecutive calls can advance the tick reference by
more than one. The dwell gate is therefore sampled at *frame* granularity and a dwell boundary can
be stepped straight over, in which case the reap simply waits for the next one. Nothing depends on
hitting every boundary — the deadline is eight dwell periods — but a reader reasoning about "every
60 ticks" from the modulo alone will be wrong on a substepping frame.

---

## §6 — The relay tap and the `dA` stamp

### What the relay is for, and why it is a separate concept

Delivery routes an input *into* this simulation for the character that sent it. The relay is a
separate, outbound concern: the server forwards the same input to the other clients so each peer can
simulate that character with its real input instead of extrapolating. Two destinations, two
lifetimes, therefore two concepts — a second engine could bind one and not the other.

### The stamp

Each relayed entry carries the **schedule** the authority intends for it: the effective input delay
held for that wire *at receipt*. The application tick is `captureTick + dA` and is **derived by the
receiver**, never sent. A peer cannot compute `dA` itself, because the sender's tier is owner-only,
so stamping is what makes the receiver's scheduled read possible at all.

`uint8_t` is deliberate. Effective delays are small tick counts — the tier ladder is 1..4 today, and
a configured relay-delay floor is itself clamped by `relayDelayFloorHardCapTicks`, which derives from
the smaller of the `LocalInputCache` and `RemoteInputCache` residency capacities minus
`rollbackWindowHardCap` — so one byte per entry is the entire wire cost of the schedule. `stampFromDelay` saturates at 255 rather than truncating:
the clamp cannot fire in any shipped configuration, and it exists so that a *misconfigured* delay can
only pin the stamp, never wrap it into a tiny value and make peers schedule an input wildly early.

### At receipt, not at release

The relay's job is to feed remote-proxy prediction promptly. Holding the input for the tier delay
before forwarding would shorten every peer's prediction runway by exactly that delay, and
capture-tick identity means peers key it correctly regardless of when the authority applies it. The
accepted trade-off is that we relay the **received** input, which on server underrun or substitution
can differ from what the server **applied** — a divergence the every-frame state channel heals.

The stamp is read from the same `delay` value the `[Park]` line reports, taken from the same queue
the release schedule uses, so the stamp and the authority's own plan cannot drift apart at the moment
of stamping.

### The gate, and why bare `acceptedNew` is wrong

The tap is reached only on `parked && acceptedNew`, and that is **structural** rather than a
re-tested boolean: the tap sits after both non-parked returns and lives inside the `acceptedNew`
arm. Bare `acceptedNew` would be wrong, because it is computed *before* the parked/fallback split.
A malformed-slot input — which the adapter then delivers undelayed — would be relayed carrying a
stamp promising application at `captureTick + dA`, while the authority applies it on arrival, and the
peer would schedule it wrongly. There is no relay on any fallback path.

### The other half of the gate

`parked && !acceptedNew` holds two populations.

* A **redundancy-bundle re-send** of an already-parked tick. Uninteresting.
* A genuinely-new **out-of-order-older** tick — one that arrived after a newer tick had moved the
  watermark. The server *does* apply it: the delay queue accepts it and capture-order release
  delivers it. It is deliberately **not** relayed, because the relay stream is monotonic in capture
  tick by construction, and at the shipped depth of 1 the payload is replace-latest, so writing an
  older input would move every peer's "latest" backwards.

The peer instead experiences a hole: the scheduled read misses, it falls back to last-known, it
mispredicts the proxy for a tick, and the every-frame state anchor heals it.

The two populations are told apart by `enqueue`'s return value, and the second is counted as
`relayOooSkipCount()` and traced, because the depth>1 future must reopen this gate decision on
measured evidence rather than on argument. That counter is a lifetime total, never reset, zero in the
ordered steady state, and not bumped by redundancy re-sends.

---

## §7 — What "engine-free" means in this header, exactly

This is a sim-core header. Its includes are other `OGSimulation/` headers and the STL, and **no
declaration in it names an engine type**: the `Address` half stays opaque and is bound in production
to `FUEConnectionHandle` and in the Catch2 suite to `FStandaloneTestHandle`; every boundary that
would otherwise need an engine type is a concept instead.

The **comments** do name one adapter's bindings, in several places, and that is deliberate. Naming
both sides — a production binding and the test-side one — is what makes an engine name an *example*
rather than a definition. §9 records the correction to the sentence that used to claim otherwise.

The type is declared in the **global** namespace, matching the rest of this core
(`ConnectionTierTable.h`, `ServerInputDelayQueue.h`, `ConnectionSlotKey.h` all carry the same note).
The design corpus writes `ogsim::`; no such namespace exists in this tree.

---

## §8 — The diagnostics: ten tags, one sink, one window

Every line this file emits goes through the optional `setLogger` sink — eleven `SIMLOG` call sites
across ten tags. A leading `[Warning]` or `[Verbose]` token selects the severity; an unprefixed line
stays at Log, which is hidden under the shipped `LogOGNet=Warning`.

| tag | severity | fires | notes |
|---|---|---|---|
| `[ServerReceive]` | Log | per decoded slot | the generic replacement for a former game-specific trace |
| `[Park]` | Log | per genuinely-new park | gated on `acceptedNew`, so a redundancy bundle does not spam it |
| `[Release]` | Log | per released input | the timeline companion to `[Park]` |
| `[InputGap]` | Warning | per hole in an id's delivered ticks | the primary, cause-agnostic drop signal |
| `[InputDrop]` | Warning | per purged, never-released entry | the precise attributable record |
| `[DelayShift]` | Warning | per wire, per change | keyed on the wire, so at most once per wire per drain |
| `[InputStats]` | Warning | per ~2 s window | drop rate; silent on an idle server |
| `[InputDomain]` | Warning | per ~2 s window | one line per rejection burst, naming the last offender |
| `[RelaySkip]` | Verbose / Warning | per event / per window | Verbose per skipped tick, Warning for the window total |
| `[InputDelay]` | Warning | once per `(id, slot)` ever | the malformed-slot fence |

### The window, and why there is only one

`[InputStats]` measures a window of **server-tick time**, derived from `TimeConfig::tickFrequency` —
no wall-clock is reachable on this path. It emits only when the window actually carried remote input,
so an idle server does not heartbeat a Warning every two seconds.

`[InputDomain]` and `[RelaySkip]` ride **the same window**: same length, same start tick, same reset
point, same emit-only-if-carried rule. The rate limit each of them needs *is* that mechanism, not a
second one. The three lines are independent — a window can carry rejects and no deliveries, or the
reverse — but they share one timer. `[RelaySkip]` was given its own tag rather than an extra field on
`[InputStats]` so that the `[InputStats]` string run scripts already grep stays byte-identical.

### Two counters that deliberately do not double-count

The window drop counter is **not** bumped at the `[InputDrop]` site. `[InputGap]` is the single,
cause-agnostic source of truth for the `[InputStats]` aggregate, and a stranded-then-purged tick
generally resurfaces as a gap at the next delivery, so counting it in both places would double-count
it. `[InputDrop]` carries the per-line attributable record instead.

`m_relayOooSkipTotal` is not reset in `forgetOwner`, unlike the per-owner watermarks beside it: it is
a session-scoped diagnostic, not per-owner state.

---

## §9 — What was found FALSE, stale or over-stated here, and the evidence

Eight claims that stood in this header were checked against the tree before anything was compressed.
Compressing a false statement makes it shorter, denser and more authoritative, and every existence
check in the toolchain passes it — so the check has to be a reading of the code, not of the comment.
**The quoted "before" text in this table is reproduced so the correction is legible. It is not a
statement of fact.**

| # | the claim as it stood | what the tree says |
|---|---|---|
| **F-37-1** | `ConnectionTierSink`: *"UE binds it to `USimmableUpdateComponent` (owner-only replicated uint8)"* | **STALE in both halves of the parenthetical.** The component no longer holds the tier: `m_replicatedConnectionTier` and its `COND_OwnerOnly` registration were removed when the tier moved to `ASimulationConnectionRelay`, whose property replicates **unconditionally** and is narrowed by `bOnlyRelevantToOwner` **relevancy**. The relay's own header records the move in past tense. The *sink type* is still the component — a `static_assert` beside its implementation pins that — so only the transport description was wrong. |
| **F-37-2** | *"A grep for UE/Unreal types in this header must be empty."* | **FALSE, and falsified by the same file.** 23 comment lines in this header carry `UE`, `Unreal` or `USTRUCT`, and the names `ASimulationManagerUImpl`, `USimmableUpdateComponent`, `FUEConnectionHandle`, `FStandaloneTestHandle`, `FInputRedundancyBundle` all appear. They are there **on purpose**: the engine-name ruling this repository adopted judged the majority of this file's engine sites legitimate, precisely because it sits on a wire boundary and must name types it matches. The testable claim is about **code**: no declaration names an engine type, and the includes are `OGSimulation/` plus the STL. §7 states that version. |
| **F-37-3** | the tick-reference members: *"fed by `noteRttSample` (per bundle) and `reapConnections` (per physics frame)"* | **FALSE, and the same file contradicted it ~880 lines earlier.** `noteRttSample` does not feed the reference and never has. `noteServerTick` has exactly **one** call site in the whole tree — inside `reapConnections` — and the method's own block says *"EXACTLY ONE FEED"* and gives two reasons for excluding the second. The suite agrees in its own words: *"The gate's tick reference is armed ONLY by reapConnections."* Same class as this initiative's earlier self-contradiction findings, reached by reading the members rather than the method. |
| **F-37-4** | `noteServerTick`: *"It is PUBLIC so an adapter (or the Catch2 suite) can arm it explicitly."* | **OVER-STATED.** No adapter and no test calls it; both arm through `reapConnections`. The sentence is true as a statement about the seam and false as an implication about use, and combined with F-37-3 it is what made the two-feeds claim read as plausible. Restated as *"public, but no external caller today"*. |
| **F-37-5** | `reapConnections`: *"The Catch2 suite cannot model that in-place transition … so that path is proven in the PIE smoke test, not by unit tests."* | **OVER-STATED.** The `!isAlive()` **branch** *is* unit-tested: a case reaps a dead wire from all three containers using a handle constructed dead, and its own comment says it *"drives the !isAlive branch"*. What cannot be staged is the **in-place** transition, because `FStandaloneTestHandle`'s `aliveBit` is part of its identity — flipping it produces a *different key* rather than a stale one. The header now claims only that. (The half of the original sentence that asserts smoke-test coverage is unverifiable from this tree and was dropped rather than repeated.) |
| **F-37-6** | `reapConnections`: *"the adapter calls it once per tick"*, and `maybeEmitInputStats`: *"Called once per tick from reapConnections"* | **IMPRECISE, with a behavioural consequence the file never stated.** Both are once per physics **frame**; the adapter's own call site says so. A frame simulates `numSteps` ticks, so the `serverTick % tierMinDwellTicks` gate is sampled at frame granularity and a dwell boundary can be **stepped straight over** on a substepping frame. Nothing breaks — the reap deadline is eight dwell periods — but "once per tick" actively conceals it. |
| **F-37-7** | `setLogger`: *"routes the one-shot malformed-slot warning."* | **STALE.** It routes every line this file emits: eleven `SIMLOG` sites across ten tags. It was true when there was one. |
| **F-37-8** | the stamp: *"floored at most to the LocalInputCache's residency ceiling"* / *"capped at 44 by the client ring's residency limit"* | **HALF-RIGHT, in two places.** The cap is `relayDelayFloorHardCapTicks`, which takes the **smaller of the local and remote input-cache capacities** minus `rollbackWindowHardCap`. Both capacities are 64 today and the hard cap is 20, so the number 44 is correct and the attribution to the local cache alone is not. The header now names the function rather than restating a derivation that can drift. |

### Two things checked and found TRUE, recorded because they look like the ones above

* **The `LogOGNet` claims are correct.** Every tag this file emits is routed to `LogOGNet` by the
  adapter's tag router, and the shipped configuration does set `LogOGNet=Warning` — so an unprefixed
  `[Park]` line really is invisible by default. Verified at the key, not at a line number.
* **`hardResyncThresholdTicks > rollbackWindowHardCap` is genuinely asserted**, by
  `TimeConfigOrderingTest.cpp`, and the numbers behind the header's bound arithmetic hold at the
  compiled defaults.

### One claim retained without independent verification, declared

The `[InputDelay]` throttle's justifying measurement — **28,192 lines / 6.4 MB in 94 s** — is a
historical observation of a predecessor that no longer exists. It cannot be re-derived from this
tree. It is retained verbatim because it is the entire reason the warning is one-shot, and a reader
deciding to "just log it every time" needs the number, not a paraphrase.
