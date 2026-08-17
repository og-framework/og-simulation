#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "OGSimulation/InputRedundancyBundleCodec.h"
#include "OGSimulation/Network/ConnectionSlotKey.h"
#include "OGSimulation/Network/ConnectionTierTable.h"
#include "OGSimulation/Network/ServerInputDelayQueue.h"
#include "OGSimulation/PCTimeManagement/TimeConfig.h"
#include "OGSimulation/SimulationLog.h"

// ---------------------------------------------------------------------------
// ServerReceptionCoordinator<Address, SimulatableTs...> — the engine-agnostic
// OWNER and ORCHESTRATOR of the server's per-connection reception state.
// (og-netcode-v2-arch-latency / T20; the engine-boundary refactor.)
//
// WHY THIS TYPE EXISTS. Stage 3/5 delivered the per-connection primitives
// (`ConnectionTierTable`, `ServerInputDelayQueue`, `ConnectionSlotKey`) into the
// sim core — but their OWNERSHIP and the logic that ORCHESTRATES them (RTT
// sample -> tier derivation, park input, drain due input, reap dropped
// connections) stayed UE-side on `ASimulationManagerUImpl`. That reception
// policy is entirely engine-agnostic, so leaving it in the UE glue blocked a
// second engine (Godot) from reusing it. This type pulls that ownership and
// orchestration down into the core, leaving the UE layer a THIN TRANSPORT
// ADAPTER that only acquires engine primitives (Address, playerSlot, RTT,
// sim-tick, wire decode) and forwards them here.
//
// A SEPARATE TYPE, not folded into SimulationNetSync. SimulationNetSync is
// client+server-mixed and physics-thread-adjacent; the reception state here is
// AUTHORITY-ONLY and GAME-THREAD-ONLY. Keeping it in its own type is what makes
// that contract legible (see the threading note below).
//
// ---------------------------------------------------------------------------
// THREADING CONTRACT — AUTHORITY-ONLY, GAME-THREAD-ONLY. (Migrated verbatim in
// intent from ASimulationManagerUImpl.h:357-371.)
//
// Every member here is touched ONLY from the game thread. The tier table is fed
// in `noteRttSample` (input-RPC receive path), input is parked in
// `receiveRemoteInput` (also the RPC path), and the queue is drained in
// `releaseDelayedInputs` (the Chaos game-thread hook immediately before the
// physics step). None of the owned containers has any internal synchronization
// (plain `std::unordered_map` / `std::deque`), so NONE may be touched from the
// physics thread, where `onGameSimulationAuthority` runs under
// `bTickPhysicsAsync=True`. The game -> physics transition stays where it always
// was: `releaseDelayedInputs` hands each released input to the adapter's
// `deliver` callback, which feeds RemoteMoveQueue — the seam the server input
// path already crosses (lead resolution R2, 2026-07-20).
//
// ---------------------------------------------------------------------------
// OWNERSHIP + LIFETIME. This type OWNS the tier table and the delay queue
// directly (they were `std::optional` members on the UE manager). Construction
// order is load-bearing and enforced by member-declaration order: the queue
// borrows the tier table by reference, so the table is declared — and therefore
// constructed — BEFORE the queue, and destroyed AFTER it (reverse order). Both
// borrow `const TimeConfig&`; the coordinator itself must not outlive that
// config. On the UE side the whole coordinator is a `std::optional` emplaced in
// BeginPlay's authority branch (after the core manager, whose TimeConfig it
// borrows) and reset in EndPlay.
//
// ---------------------------------------------------------------------------
// THE CLAIM MAP IS id-KEYED, NOT engine-pointer-keyed. (fable review B2'.) The
// delivery-routing map stores `ConnectionSlotKey -> id` — a plain simulatable
// id, not a `TWeakObjectPtr` the core cannot own. GC-liveness of the owning
// component is therefore NOT read here; component death is handled by the
// established unregister contract (`forgetOwner`, called from the adapter's
// unregister path — the same lifecycle seam SimulationNetSync::unregisterSimulatable
// rides) plus the `deliver` callback reporting a dead owner. WIRE death is still
// handled here, by `reapConnections`, off the Address half of the key.
//
// ENGINE-AGNOSTIC. Sim-core header: includes ONLY other `OGSimulation/` headers
// and the STL. The `Address` half stays opaque, bound in production to
// `FUEConnectionHandle` (OGSimulationUnreal) and in the Catch2 suite to
// `FStandaloneTestHandle`. A grep for UE/Unreal types in this header must be
// empty.
//
// NAMESPACE NOTE: declared in the GLOBAL namespace, matching the rest of the
// OGSim core (see the same note on ConnectionTierTable.h / ServerInputDelayQueue.h
// / ConnectionSlotKey.h). The design corpus writes `ogsim::` but no such
// namespace exists in this tree.
// ---------------------------------------------------------------------------

// Reap deadline expressed as a multiple of the dwell period. Migrated from the
// former UE file-local constant (SimulationManagerUImpl.cpp): a connection whose
// last sample is older than `tierMinDwellTicks * kTierReapDeadlineDwellPeriods`
// is considered gone even if its handle never reported dead (half-open socket).
inline constexpr int32_t kTierReapDeadlineDwellPeriods = 8;

// ---------------------------------------------------------------------------
// ConnectionTierSink — the SEND boundary for the server->owning-client tier
// publish. (og-netcode-v2-arch-latency / T23.)
//
// The tier SEND used to be a two-step the adapter could silently half-complete:
// derive the tier here, then separately call a UE publish method. Nothing forced
// the second call, and the no-reading / dedup POLICY leaked UE-side. Inverting it
// so `noteRttSample` FIRES the send itself (below) makes "derived but never sent"
// structurally impossible, but the send target must stay engine-agnostic — the
// core cannot name a UE type. So the sink is a compile-time CONCEPT: any type that
// can transport a (id, tier) to the owning client satisfies it. UE binds it to
// `USimmableUpdateComponent` (owner-only replicated uint8); a second engine binds
// its own owner-only RPC / replicated var. A sink missing the method is a compile
// error at the `noteRttSample` call site, exactly like the buffer-owner concepts
// in SimulationNetSync.h. `id` identifies the target entity so a sink that is a
// central manager (rather than the entity itself) can route on it.
template <typename T>
concept ConnectionTierSink = requires(T& t, unsigned int id, uint8_t tier) {
    { t.sendConnectionTierToOwningClient(id, tier) } -> std::same_as<void>;
};

// ---------------------------------------------------------------------------
// RemoteInputDeliverySink — the DELIVERY boundary for a received remote input.
// (og-netcode-v2-arch-latency / T24, symmetric with T23's ConnectionTierSink.)
//
// The per-slot receive loop used to live UE-side on ServerReceiveRemoteMove: it
// decoded the bundle, parked each slot, and — when a slot could not be parked —
// fell back to legacy direct delivery. That loop is engine-agnostic policy, so
// it moves into `receiveInputBundle` below. But the DELIVERY itself must stay
// engine-bound (a UE component's RemoteMoveQueue, a Godot node's equivalent), so
// like the tier send it is expressed as a compile-time CONCEPT. Any type able to
// route a (id, captureTick, input) to the owning entity satisfies it. The core
// invokes it on the ONE non-parked path (a malformed slot the delay queue
// refused), and the SAME sink type is the one the drain's UE-side delivery routes
// through (ASimulationManagerUImpl::deliverRemoteInput) — unifying the two
// delivery paths onto a single method. A sink missing the method is a compile
// error at the `receiveInputBundle` call site.
//
// TWO-PARAMETER (T, InputT), unlike ConnectionTierSink: the delivered payload is
// typed, and the core is variadic over the simulatable pack, so the input type is
// supplied at the call site as `InputFor<SimT>` rather than baked into the
// concept. `id` identifies the target entity so a central-manager sink (rather
// than the entity itself) can route on it — which is exactly how the UE binding
// works (the manager owns the id->component map the drain also resolves against).
template <typename T, typename InputT>
concept RemoteInputDeliverySink =
    requires(T& t, unsigned int id, uint32_t captureTick, const InputT& in) {
        { t.deliverRemoteInput(id, captureTick, in) } -> std::same_as<void>;
    };

// ---------------------------------------------------------------------------
// RemoteInputRelaySink — the RELAY boundary for a received remote input.
// (og-netcode-v2-input-relay / T3, symmetric with T23's ConnectionTierSink and
// T24's RemoteInputDeliverySink.)
//
// WHAT THIS IS FOR. Delivery (above) routes an input INTO the simulation for the
// character that sent it. The RELAY is a separate, outbound concern: the server
// forwards the same input to the OTHER clients so each peer can simulate that
// character with its real input instead of extrapolating
// (InputRelayDesign.md §3a). Two different destinations, two different lifetimes,
// therefore two concepts — a second engine could bind one and not the other.
//
// THE STAMP (`dA`). Each relayed entry carries the SCHEDULE the authority intends
// for it: the effective input delay held for that wire AT RECEIPT
// (RelayDelaySpectrumDesign.md §5.2). The application tick is `captureTick + dA`
// and is DERIVED by the receiver, never sent. A peer cannot compute `dA` itself —
// the sender's tier is replicated owner-only — so stamping is what makes the
// receiver's scheduled read possible at all (§5.1, decision D5). `uint8_t` is
// deliberate: the delay is a small tick count (tier delays 1..4 today, floored at
// most to the LocalInputCache's residency ceiling), and one byte per entry is
// the entire wire cost of the schedule.
//
// TWO-PARAMETER (T, InputT) for the same reason RemoteInputDeliverySink is: the
// core is variadic over the simulatable pack, so the input type is supplied at the
// call site as `InputFor<SimT>` rather than baked into the concept. `id` names the
// entity whose relay payload is being written, so a central-manager sink can route
// on it — which is exactly how the UE binding works (the manager writes the
// id->component's replicated relay ring).
template <typename T, typename InputT>
concept RemoteInputRelaySink =
    requires(T& t, unsigned int id, uint32_t captureTick, uint8_t dA, const InputT& in) {
        { t.relayRemoteInput(id, captureTick, dA, in) } -> std::same_as<void>;
    };

// A do-nothing RemoteInputRelaySink — the DEFAULT for the receipt entry points
// below, so every call site that predates the relay (and every unit case that has
// no interest in it) compiles and behaves exactly as before. Production always
// passes a real sink; the concept constraint means a sink that is passed but lacks
// `relayRemoteInput` is a compile error at the call site, not a silent no-op.
struct NullRemoteInputRelaySink
{
    template <typename InputT>
    void relayRemoteInput(unsigned int, uint32_t, uint8_t, const InputT&) const {}
};

// Outcome of a single `receiveRemoteInput` call.
//
//   parked      — TRUE when the input was accepted into the delay queue. FALSE
//                 means the ADAPTER must fall back to legacy direct delivery so
//                 no player input is ever silently dropped (the malformed-slot
//                 fence is the only in-core false path; the adapter early-outs on
//                 no-wire before ever calling in). SEE `rejectedOutOfDomain`: it
//                 is the ONE false-parked case that must NOT be delivered.
//   acceptedNew — TRUE when this (id, captureTick) is the first time the
//                 coordinator has seen that capture tick for that id; FALSE for a
//                 redundancy-bundle re-send of an already-seen tick, AND for a
//                 genuinely-new but out-of-order-OLDER tick. Introduced by T20 as
//                 a pure signal for the then-future relay write (fable review B1
//                 residual); since T3 it GATES THE RELAY TAP — see the tap itself
//                 in receiveRemoteInput. It still gates nothing about
//                 parking/delivery, which are unchanged by it.
//   rejectedOutOfDomain
//               — TRUE when the out-of-domain receipt gate (T12) refused the
//                 capture tick: it fell outside
//                 `[serverTick - rollbackWindowHardCap, serverTick + hardResyncThresholdTicks]`.
//                 The input is DISCARDED — not parked, not claimed, not relayed,
//                 and (unlike the malformed-slot fence) NOT delivered undelayed.
//                 `parked` is false and `acceptedNew` is false in this case; the
//                 caller must check this flag BEFORE treating !parked as "fall
//                 back to legacy delivery", which is exactly what
//                 `receiveInputBundle` below does.
struct ReceiveRemoteInputResult
{
    bool parked              = false;
    bool acceptedNew         = false;
    bool rejectedOutOfDomain = false;
};

template <ConnectionAddress Address, SimulatableWithInput... SimulatableTs>
class ServerReceptionCoordinator
{
public:
    using TierTable   = ConnectionTierTable<Address>;
    using DelayQueue  = ServerInputDelayQueue<Address, SimulatableTs...>;
    using SlotKey     = ConnectionSlotKey<Address>;

    template <typename SimT>
    using InputFor = SimulatableInputOf_t<SimT>;

    // Borrows `const TimeConfig&` — never owned; the caller (the UE manager's
    // core SimulationManager) must outlive this coordinator. The tier table and
    // delay queue are constructed here in the load-bearing order: table first,
    // queue second (the queue binds the table by reference). Reverse teardown is
    // automatic from member-declaration order.
    explicit ServerReceptionCoordinator(const TimeConfig& cfg)
        : m_config(cfg)
        , m_tierTable(cfg)
        , m_inputDelayQueue(cfg, m_tierTable)
    {
    }

    ServerReceptionCoordinator(const ServerReceptionCoordinator&)            = delete;
    ServerReceptionCoordinator& operator=(const ServerReceptionCoordinator&) = delete;

    // Optional structured-log sink; routes the one-shot malformed-slot warning.
    // Prefix "[Warning]" is honoured by the UE logger route (see SimulationLog.h).
    void setLogger(std::function<void(const char*)> logger) { m_logger = std::move(logger); }

    // -----------------------------------------------------------------------
    // noteServerTick — the adapter's GAME-THREAD server sim-tick reference.
    // (og-netcode-v2-input-relay / T12.)
    //
    // WHY THIS EXISTS. The out-of-domain receipt gate (see receiveRemoteInput)
    // judges an inbound capture tick against the CURRENT server tick, but
    // `receiveRemoteInput` / `receiveInputBundle` are per-slot payload calls that
    // do not carry one. The server tick is an ENGINE PRIMITIVE (Address, RTT and
    // sim-tick are the three the adapter already resolves), and the adapter ALREADY
    // hands one to this type every physics frame via `reapConnections(serverTick)`,
    // which therefore records it here. No adapter signature changed for this gate.
    //
    // EXACTLY ONE FEED, AND IT IS THE DRAIN HOOK — deliberate. `noteRttSample` also
    // receives a serverTick and could plausibly feed this too (it runs on the RPC
    // path, immediately before `receiveInputBundle`), but it is NOT wired in, for
    // two reasons:
    //   * SOURCE QUALITY. `reapConnections` is called from the same game-thread
    //     hook as the drain and is passed the ChaosTickMapper-derived
    //     `firstUpcomingSimTick` — the tick source that resolution is documented as
    //     game-thread-safe. `noteRttSample`'s tick is `getServerReceptionTick()`, an
    //     acknowledged wart: an unsynchronized read of the physics-thread-written
    //     server clock. A gate that DISCARDS player input should judge against the
    //     safe source, not the racy one.
    //   * MONOTONIC-ENOUGH REFERENCE. Two feeds a tick apart could hand the gate a
    //     reference that jitters backwards between consecutive receipts, making an
    //     input sitting exactly on a boundary accepted or rejected depending on
    //     which feed wrote last. One feed, one cadence, no jitter.
    // The cost is that the gate is unarmed until the first physics frame — a window
    // in which the server tick is ~0 and warm-up capture ticks are legitimately
    // in-domain anyway. It is PUBLIC so an adapter (or the Catch2 suite) can arm it
    // explicitly.
    //
    // LAST-WRITE-WINS, NOT a monotonic max: a max would stick forever if the clock
    // ever restarted (PIE restart, seamless travel), permanently rejecting every
    // subsequent input.
    //
    // FAIL-OPEN UNTIL ARMED, deliberately. Before the first call the coordinator
    // has NO tick reference, so the gate cannot judge and accepts everything: a
    // gate that failed CLOSED with an unset reference would silently discard every
    // player input on any path that forgot to feed it. `m_serverTickKnown` makes
    // "never armed" distinguishable from "armed at tick 0". This is also what keeps
    // the pre-T12 Catch2 cases — which never drive a tick — byte-identical.
    void noteServerTick(int32_t serverTick)
    {
        m_serverTickKnown = true;
        m_serverTick      = serverTick;
    }

    // -----------------------------------------------------------------------
    // (1) noteRttSample — ONCE PER BUNDLE. (fable review B1'.)
    //
    // A bundle is one datagram and therefore one arrival event; FNetPing's
    // RoundTrip value only advances on ack receipt anyway. Sampling per SLOT
    // would feed the same reading into the tier EMA up to kMaxSlots times and
    // couple the effective smoothing rate to redundancy depth — the exact bug the
    // former per-bundle sample site guarded against.
    //
    // `rttMs` is `double` — narrowing to int32 would shift every EMA update
    // sub-millisecond (ConnectionTierTable feeds `onRttSample` a `double`), a
    // silent tier-behaviour change. The value is the engine's RAW RoundTrip in
    // milliseconds; the tier table's own EMA is the only smoothing (single-
    // smoothing, Option A).
    //
    // FIRES THE SEND ITSELF — returns nothing (T23). The two policies that used to
    // live UE-side move INTO the core here:
    //   * the rttMs < 0 "no reading yet" skip — a negative reading is not sampled
    //     and publishes nothing, exactly as the old adapter sentinel did; and
    //   * publish-only-on-change — the sink is invoked ONLY when the tier the
    //     owning client should now hold differs from what it was last told.
    //
    // WHY THE DEDUP IS KEYED ON ownerId, NOT Address (lead spec correction
    // 2026-07-23). Two couch-coop characters can share ONE root connection (one
    // Address) while each owns its OWN owner-only replicated tier property. A
    // per-Address last-published would publish a transition to the first sibling
    // and skip the other forever. Keying on ownerId — the SINK TARGET — faithfully
    // mirrors the old per-component dedup: both siblings converge. It is also
    // symmetric with the per-id captureTick watermark, and cleared in the same
    // place (forgetOwner). A missing entry means "never told", whose baseline is
    // the replicated property's default of 0 — so a first sample that derives tier
    // 0 correctly publishes nothing.
    //
    // WHY WE COMPARE result.newTierIndex, NOT the transition delta. onRttSample
    // reports the wire's CURRENT tier on every call, and the transition it signals
    // belongs to whichever owner's sample happened to cross the dwell gate. A
    // sibling that did not trigger the transition still sees `newTierIndex` = the
    // new tier and, having last been told 0, must be published to. Gating on
    // "did THIS sample transition" would starve that sibling — the couch-coop bug
    // above, restated at the publish decision.
    //
    // `rttMs` is `double`; see the once-per-bundle + single-smoothing notes above.
    template <ConnectionTierSink Sink>
    void noteRttSample(const Address& addr, unsigned int ownerId, int32_t serverTick,
                       double rttMs, Sink&& sink)
    {
        if (rttMs < 0.0)
        {
            return;         // engine "no reading yet" sentinel — do not sample it
        }
        const TierSampleResult result = m_tierTable.onRttSample(addr, serverTick, rttMs);
        const int32_t newTier = result.newTierIndex;

        const auto it = m_lastPublishedTier.find(ownerId);
        const int32_t lastPublished = (it == m_lastPublishedTier.end()) ? 0 : it->second;
        if (newTier == lastPublished)
        {
            return;         // unchanged for this owner — do not dirty the wire
        }

        if (it == m_lastPublishedTier.end())
        {
            m_lastPublishedTier.emplace(ownerId, newTier);
        }
        else
        {
            it->second = newTier;
        }

        sink.sendConnectionTierToOwningClient(ownerId, static_cast<uint8_t>(newTier));
    }

    // -----------------------------------------------------------------------
    // (2) receiveRemoteInput — PER SLOT.
    //
    // `playerSlot` is a PARAMETER (fable B4'): it is an engine primitive (the UE
    // child-connection id via GetPlayerSlotForActor), exactly like Address and
    // RTT, and the core cannot derive it. Parks `input` under (addr, playerSlot)
    // to be released on `captureTick + effectiveDelay`. Returns the fallback
    // decision + the dedup signal (see ReceiveRemoteInputResult).
    //
    // The malformed-slot fence is the only in-core false path that still DELIVERS:
    // a slot outside the uint8 substitution-mask range means a malformed topology,
    // not a supported configuration. It is warned ONCE per (id, slot) — never per
    // tick — because the un-throttled UE predecessor produced 28,192 lines / 6.4 MB
    // in 94 s of a single PIE session. The input still takes the legacy undelayed
    // path. The out-of-domain gate below is the one false path that DISCARDS.
    //
    // -----------------------------------------------------------------------
    // THE OUT-OF-DOMAIN RECEIPT GATE (og-netcode-v2-input-relay / T12;
    // RelayDelaySpectrumDesign.md §8.1). RUNS FIRST — before the dedup watermark,
    // before the slot fence, before park/claim, and before the (T3) relay tap.
    //
    // WHAT IT REJECTS. A capture tick outside
    //     [serverTick - rollbackWindowHardCap, serverTick + hardResyncThresholdTicks]
    // (both bounds INCLUSIVE, both sourced from TimeConfig — no literals here).
    //
    // WHY. A client that is warming up or free-running has not yet been anchored
    // to the server's tick numbering: it emits capture ticks from its OWN counter
    // (0, 1, 2 …) while the server is at 600+. Today that garbage is merely parked
    // and later purged, so it is invisible past the drain — but once T3 makes
    // receipt-time input PEER-VISIBLE through the relay, an unfiltered garbage tick
    // is broadcast to every other client and resolved against THEIR timelines. The
    // gate exists so nothing outside the server's own tick domain ever reaches the
    // relay tap.
    //
    // WHY THESE BOUNDS.
    //   * Lower `serverTick - rollbackWindowHardCap` is exactly the drain's
    //     `staleBefore` (see releaseDelayedInputs) — one window semantics for the
    //     whole reception path. An out-of-window receipt is now rejected up front
    //     instead of parked-then-purged; both mean "too old to matter".
    //   * Upper `serverTick + hardResyncThresholdTicks` is the failsafe drift
    //     threshold: a client legitimately captures AHEAD of the server (that is
    //     the whole prediction offset), and steady-state lead is roughly
    //     2·jitter·freq plus the ±3 dead band — comfortably inside 21 at any
    //     playable RTT. Beyond it the client is further adrift than the hard-resync
    //     backstop tolerates, i.e. it is about to be snapped anyway.
    //   * `hardResyncThresholdTicks > rollbackWindowHardCap` is a TimeConfig
    //     ordering invariant, so the window is never empty or inverted.
    //
    // WHY BEFORE THE DEDUP WATERMARK (load-bearing). `noteCaptureTick` keeps a
    // MONOTONIC MAX per owner id. One garbage tick far in the future would raise
    // that watermark permanently, and every subsequent LEGITIMATE input would then
    // report `acceptedNew == false` — silently suppressing the [Park] trace and,
    // post-T3, the relay write itself. The gate must run before the watermark is
    // touched, not merely before the park.
    //
    // A REJECTED INPUT IS DISCARDED, NOT DELIVERED. It returns
    // `parked == false` like the malformed-slot fence, but with
    // `rejectedOutOfDomain == true` so `receiveInputBundle` (and any other adapter)
    // does NOT take the undelayed fallback — falling back would hand the very
    // garbage tick we just refused straight to the delivery sink.
    //
    // AUDIT — THE SECOND, PRE-EXISTING GUARD (kept deliberately; review ruling
    // "document both"). `RemoteMoveQueue::queueMove` (SimulationQueues.h) already
    // rejects `captureTick > serverAuthorityTick + rollbackWindowTicks` (+12,
    // TIGHTER than this gate's +21) at the PHYSICS-THREAD DELIVERY layer, with its
    // context published per authority tick via `setAuthorityGuardContext`. The two
    // do not conflict and neither subsumes the other: this one guards RECEIPT (and
    // therefore the relay tap) on the game thread; that one guards DELIVERY into
    // the simulation. On the parked path the queue guard is effectively dead —
    // drained entries are due-or-overdue, never ahead of authority. It fires only
    // on the fallback paths (no-wire adapter early-out, malformed slot), where the
    // ACCEPTED COMPOSITE BEHAVIOUR is: an input in the band
    // `(authorityTick + rollbackWindowTicks, serverTick + hardResyncThresholdTicks]`
    // passes THIS gate and is then discarded by `queueMove` as too-far-future. That
    // is accepted and documented, not silent.
    //
    // -----------------------------------------------------------------------
    // THE RELAY TAP (og-netcode-v2-input-relay / T3; InputRelayDesign.md §3a,
    // RelayDelaySpectrumDesign.md §5). `relay` is an OUTBOUND TAP on this path:
    // when a genuinely-new capture tick is parked, the same input is handed to the
    // RemoteInputRelaySink, stamped with the schedule the authority intends for it.
    // It is a TAP in the strict sense — it reads what the path already computed and
    // changes nothing about park/claim/delivery. Defaulted to a no-op sink so every
    // pre-relay call site stays byte-identical.
    //
    // AT RECEIPT, NOT AT RELEASE (decision, InputRelayDesign.md §3a). The relay's
    // job is to feed remote-proxy prediction PROMPTLY; holding the input for the
    // tier delay before forwarding would shorten every peer's prediction runway by
    // exactly that delay. Capture-tick identity means peers key it correctly
    // regardless of when the authority applies it. The documented trade-off (D1) is
    // that we relay the RECEIVED input, which on server underrun/substitution can
    // differ from what the server APPLIED — a divergence the every-frame state
    // channel heals.
    //
    // THE GATE IS `parked && acceptedNew` (review A6) — and it is STRUCTURAL here,
    // not a re-tested boolean: the tap sits after both non-parked returns (the
    // out-of-domain rejection and the malformed-slot fence), so reaching it already
    // means `parked == true`, and it lives inside the `acceptedNew` arm. Bare
    // `acceptedNew` would be WRONG: it is computed BEFORE the parked/fallback split,
    // so a malformed-slot input — which the adapter then delivers UNDELAYED — would
    // be relayed carrying a stamp promising application at `captureTick + dA` while
    // the authority applies it at arrival. A peer would then schedule it wrongly.
    // There is NO relay on any fallback path.
    //
    // THE OTHER HALF OF THE GATE — `parked && !acceptedNew` (spectrum doc §5.3a).
    // Two populations land there. A redundancy-bundle RE-SEND of an already-parked
    // tick is uninteresting. But a genuinely-new OUT-OF-ORDER-OLDER tick (one that
    // arrived after a newer tick had moved the watermark) IS applied by the server —
    // the delay queue accepts it and T26's capture-order release delivers it — and
    // is deliberately NOT relayed: the relay stream is monotonic in capture tick by
    // construction, and at the shipped depth of 1 the payload is replace-latest, so
    // writing an older input would move every peer's "latest" BACKWARDS. Peers
    // experience a HOLE (scheduled read misses -> last-known fallback -> one-tick
    // proxy mispredict -> healed by the every-frame state anchor). The two
    // populations are told apart by `enqueue`'s return value, and the second is
    // counted as `relayOooSkipCount()` and logged, because the depth>1 future must
    // REOPEN this gate decision on measured evidence (T9's probe), not on argument.
    template <typename SimT, typename RelaySink = NullRemoteInputRelaySink>
        requires RemoteInputRelaySink<RelaySink, InputFor<SimT>>
    ReceiveRemoteInputResult receiveRemoteInput(unsigned int id,
                                                const Address& addr,
                                                uint8_t playerSlot,
                                                int32_t captureTick,
                                                const InputFor<SimT>& input,
                                                RelaySink&& relay = RelaySink{})
    {
        if (rejectOutOfDomainCaptureTick(id, captureTick))
        {
            return ReceiveRemoteInputResult{ /*parked=*/false, /*acceptedNew=*/false,
                                             /*rejectedOutOfDomain=*/true };
        }

        // Dedup watermark: a pure signal for the future relay write, computed and
        // updated BEFORE the parked/fallback split so it covers both paths.
        // Reaped in forgetOwner, so it is bounded by live component ids.
        const bool acceptedNew = noteCaptureTick(id, captureTick);

        const SlotKey key(addr, playerSlot);

        if (!key.hasValidSlot())
        {
            warnOutOfRangeSlotOnce(id, playerSlot);
            return ReceiveRemoteInputResult{ /*parked=*/false, acceptedNew,
                                             /*rejectedOutOfDomain=*/false };
        }

        // Delivery routing: which owner id this slot's released input goes to. A
        // plain overwrite — re-registering the same id is a no-op, and an id
        // legitimately replacing a dead one on the same slot (respawn / seamless
        // travel) takes the slot over. Two characters on one machine are two
        // distinct keys, so there is nothing to conflate.
        m_delayedInputTargets[key] = id;

        // TRUE when this capture tick was not already resident in the slot's deque,
        // i.e. the server really did take a new input here (T3 — the discriminator
        // between a redundancy re-send and an out-of-order-older arrival).
        const bool queuedNewEntry =
            m_inputDelayQueue.template enqueue<SimT>(key, captureTick, input);

        // [Park] (Log severity — on-demand under LogOGNet Verbose). The full
        // per-tick input timeline for deep dives; its companion is [Release] at
        // the drain. Gated on `acceptedNew` so a redundancy bundle re-sending an
        // already-seen capture tick (the common case) does not spam the timeline —
        // a genuinely new park is logged exactly once. No [Warning] prefix, so it
        // stays at Log severity and is hidden under the default LogOGNet=Warning.
        if (acceptedNew)
        {
            const int32_t delay = m_inputDelayQueue.effectiveDelay(key);
            SIMLOG(m_logger, "[Park] id=%u captureTick=%d delay=%d releaseTick=%d",
                id, captureTick, delay, captureTick + delay);

            // --- THE RELAY TAP (T3) -----------------------------------------
            // Reached only on the parked path (see the long note above), and only
            // for a capture tick this id has never sent before => each newer tick
            // is relayed EXACTLY ONCE, at receipt, stamped with the wire's CURRENT
            // effective delay. `delay` is the very value the [Park] line reports,
            // read from the same queue the release schedule uses, so the stamp and
            // the authority's own plan cannot drift apart at the moment of stamping.
            // (After T11 lands, the relay floor is already folded INSIDE
            // effectiveDelay — no code dependency between the two tasks, only value
            // composition.)
            relay.relayRemoteInput(id, static_cast<uint32_t>(captureTick),
                                   stampFromDelay(delay), input);
        }
        else if (queuedNewEntry)
        {
            // Genuinely new, but older than this id's watermark: applied, not
            // relayed. Counted + traced — see the gate note above.
            noteRelayOooSkip(id, captureTick);
        }

        return ReceiveRemoteInputResult{ /*parked=*/true, acceptedNew,
                                         /*rejectedOutOfDomain=*/false };
    }

    // -----------------------------------------------------------------------
    // (2b) receiveInputBundle — the whole PER-BUNDLE receive loop. (T24.)
    //
    // Relocated verbatim in intent from the UE ServerReceiveRemoteMove per-slot
    // loop: decode the wire bundle with the (already-core) redundancy codec, park
    // each slot via receiveRemoteInput, and on the ONE non-parked path (a malformed
    // slot the delay queue refused) deliver immediately through `deliver` — the
    // SAME RemoteInputDeliverySink the drain's UE delivery routes through, so the
    // park-then-drain path and the deliver-now fallback share one delivery method.
    //
    // `wire` is any type satisfying the codec's Buffer concept (bundleByteNum /
    // readFromBuffer / ...); in production it is the FInputRedundancyBundle USTRUCT
    // itself (which IS a Buffer), in the Catch2 suite a std::vector-backed test
    // buffer — the core never names either. The bundle was written for one input
    // type, so it is decoded as `InputFor<SimT>`, matching how it was appended.
    //
    // The NO-WIRE early-out stays ADAPTER-side (fable ruling): the adapter only
    // reaches here when it has a live wire + coordinator, so the core handles only
    // `hasValidSlot`. The once-per-bundle RTT sample (noteRttSample) is the
    // adapter's responsibility BEFORE this call — this method is purely the slot
    // loop. The genericized `[ServerReceive] id tick` trace replaces the former
    // brawler-specific attackLeft log (the core is templated over SimT and cannot
    // read a game-specific input field); it routes through the coordinator logger.
    //
    // [T3] `relay` is threaded straight through to receiveRemoteInput's relay tap —
    // this method adds no relay policy of its own. Production passes the SAME object
    // for both sinks (the manager binds delivery AND relay), but they stay separate
    // parameters because they are separate boundaries: delivery routes an input INTO
    // the simulation, the relay forwards it OUT to the other clients. Defaulted to
    // the no-op sink so a caller that has no relay wiring is unaffected.
    template <typename SimT, typename Buffer, typename Sink,
              typename RelaySink = NullRemoteInputRelaySink>
        requires RemoteInputDeliverySink<Sink, InputFor<SimT>>
              && RemoteInputRelaySink<RelaySink, InputFor<SimT>>
    void receiveInputBundle(unsigned int id, const Address& addr, uint8_t playerSlot,
                            const Buffer& wire, Sink&& deliver,
                            RelaySink&& relay = RelaySink{})
    {
        inputRedundancyBundle::forEachSlot<InputFor<SimT>>(
            wire,
            [&](std::uint32_t captureTick, const InputFor<SimT>& input)
            {
                SIMLOG(m_logger, "[ServerReceive] id=%u tick=%u",
                    id, static_cast<unsigned int>(captureTick));

                const ReceiveRemoteInputResult result = receiveRemoteInput<SimT>(
                    id, addr, playerSlot, static_cast<int32_t>(captureTick), input, relay);

                // TWO different false-parked meanings, and only one of them
                // delivers (T12):
                //   * rejectedOutOfDomain — the receipt gate refused the capture
                //     tick. It is NOT this server's tick domain, so it is DROPPED
                //     here: delivering it undelayed would defeat the gate (and
                //     `queueMove`'s own future-guard would likely refuse it anyway).
                //     Already counted + rate-limit-logged by receiveRemoteInput.
                //   * otherwise (malformed slot) — the delay queue refused it and
                //     already warned once. Deliver undelayed so no player input is
                //     ever silently dropped, through the same sink the drain uses.
                if (!result.parked && !result.rejectedOutOfDomain)
                {
                    deliver.deliverRemoteInput(id, captureTick, input);
                }
            });
    }

    // -----------------------------------------------------------------------
    // (3) releaseDelayedInputs — THE DRAIN. (fable review B2'; relocated from
    // ASimulationManagerUImpl::releaseDelayedInputsForStep.)
    //
    // Drains every claimed slot's due input for the `numSteps` sim ticks the
    // upcoming physics step(s) will simulate, and delivers each through the
    // per-id `deliver` callback — NOT a TWeakObjectPtr. `firstUpcomingSimTick`
    // (the ChaosTickMapper `+1` derivation) and `numSteps` are supplied by the
    // adapter, which owns the game-thread-safe tick source; the core never reads
    // a physics-thread clock.
    //
    // `deliver(id, captureTick, input) -> bool`. Returning FALSE means the owner
    // is gone (component GC'd without an unregister); the claim entry is then
    // dropped, mirroring the old drain's `target.Get()==nullptr` prune. The
    // delivered capture tick is the entry's STORED captureTick, surfaced by
    // `tryDequeueForTick` (F1) — NOT a reconstructed `simTick - delay`. Under the
    // T26 due-or-overdue release an overdue entry is released a tick or more late,
    // so `simTick - delay` would name a FUTURE input's tick and collide with
    // RemoteMoveQueue's capture-tick dedup; delivering the true stored tick keeps
    // that dedup and the [InputGap] watermark meaningful — the delay is expressed
    // purely as WHEN this fires, and any lateness is reported as `late=N`.
    template <typename SimT, typename DeliverFn>
    void releaseDelayedInputs(int32_t firstUpcomingSimTick, int32_t numSteps, DeliverFn&& deliver)
    {
        if (m_delayedInputTargets.empty())
        {
            return;
        }

        // The rollback-window lower bound, shared by BOTH the release gate (F3,
        // passed into tryDequeueForTick) and the purge below. Using the one value
        // for both is what keeps the purge the SINGLE drop point: tryDequeueForTick
        // never releases an entry the purge would reclaim, and the purge reclaims
        // exactly what the release gate skipped. May be non-positive early in a
        // session (before tick `rollbackWindowHardCap`), where it harmlessly gates
        // nothing — captureTicks are non-negative — and the purge below is skipped.
        const int32_t staleBefore = firstUpcomingSimTick - m_config.rollbackWindowHardCap;

        for (auto it = m_delayedInputTargets.begin(); it != m_delayedInputTargets.end();)
        {
            const SlotKey&     key = it->first;
            const unsigned int id  = it->second;

            // Read the delay ONCE per slot per drain. Every slot on a wire shares
            // that wire's tier by design, so this resolves through the Address
            // half. It is used only for the [DelayShift] memo and the `late=N`
            // lateness measurement; the delivered captureTick comes from the queue
            // (F1), so this side no longer reconstructs it from the release tick.
            const int32_t delay = m_inputDelayQueue.effectiveDelay(key);

            // [DelayShift] (Warning) — correlate drops with tier moves. Keyed on
            // the WIRE (Address): every slot on a wire shares its tier, so this
            // fires at most once per wire per drain, not once per slot.
            noteDelayShift(key.address, delay, firstUpcomingSimTick);

            bool ownerAlive = true;
            for (int32_t s = 0; s < numSteps; ++s)
            {
                const int32_t simTick = firstUpcomingSimTick + s;

                InputFor<SimT> released{};
                int32_t deliveredCaptureTick = 0;
                if (!m_inputDelayQueue.template tryDequeueForTick<SimT>(
                        key, simTick, released, staleBefore, &deliveredCaptureTick))
                {
                    continue;   // nothing due-and-in-window for this slot on this tick
                }

                // F3 lateness: 0 in the in-time steady state; >0 when this input is
                // released after its exact due tick. Logged so the next PIE run
                // MEASURES the plateau instead of assuming it.
                const int32_t late = simTick - (deliveredCaptureTick + delay);

                if (!deliver(id, static_cast<uint32_t>(deliveredCaptureTick), released))
                {
                    ownerAlive = false;     // owner gone — drop the claim below
                    break;
                }

                // [Release] (Log — the per-tick timeline companion to [Park]) then
                // [InputGap] (Warning) + window-stats accounting: the drain deliver
                // point observes every released capture tick per id, so it is the
                // natural home for both the timeline trace and the gap watermark.
                SIMLOG(m_logger, "[Release] id=%u captureTick=%d releaseTick=%d late=%d",
                    id, deliveredCaptureTick, simTick, late);
                noteDeliveredForGap(id, deliveredCaptureTick);
            }

            it = ownerAlive ? std::next(it) : m_delayedInputTargets.erase(it);
        }

        // Reclaim input parked but never released — beyond the rollback window (the
        // release gate above left it for the purge, so this is the ONE drop point).
        // The window is the span over which a capture tick can still legitimately
        // matter. Same `staleBefore` as the release gate, by construction.
        if (staleBefore > 0)
        {
            // [InputDrop] (Warning) — name the strand. purgeOlderThan now RETURNS
            // the reclaimed entries instead of erasing them silently; each one is
            // a remote input that was parked but never released. The window drop
            // counter is deliberately NOT bumped here: [InputGap] is the single,
            // cause-agnostic source of truth for the [InputStats] aggregate (a
            // stranded-then-purged tick generally re-surfaces as a gap at the next
            // delivery, so counting it here too would double-count). The per-line
            // [InputDrop] is the precise, attributable record.
            const std::vector<typename DelayQueue::PurgedEntry> dropped =
                m_inputDelayQueue.template purgeOlderThan<SimT>(staleBefore);
            for (const typename DelayQueue::PurgedEntry& d : dropped)
            {
                const int32_t dropDelay = m_inputDelayQueue.effectiveDelay(d.slotKey);
                SIMLOG(m_logger,
                    "[Warning][InputDrop] slot=%u captureTick=%d never released; delay=%d staleBefore=%d",
                    static_cast<unsigned int>(d.slotKey.playerSlot), d.captureTick,
                    dropDelay, staleBefore);
            }
        }
    }

    // -----------------------------------------------------------------------
    // (4) reapConnections — evict dropped WIRES. (Relocated from the arrival-
    // gated reap block in sampleAndDeriveConnectionTier.)
    //
    // CADENCE CHANGE, documented and benign (fable concern). The old reap fired
    // only when a bundle arrived on a dwell-boundary tick, so an idle server
    // NEVER reaped — a latent leak. This runs on EVERY dwell-boundary tick
    // regardless of traffic (the adapter calls it once per tick). Strictly more
    // reaps; the internal `serverTick % dwell` gate keeps the frequency the same
    // as before on a busy server. T22 watches it.
    //
    // The production-relevant branch is the DEAD one (`!isAlive()`): the engine
    // GCs the connection and the Address liveness goes stale IN PLACE under an
    // unchanged key. The Catch2 suite cannot model that in-place transition (T4's
    // finding), so that path is proven in the PIE smoke test, not by unit tests.
    void reapConnections(int32_t serverTick)
    {
        // T12: the once-per-physics-frame tick reference for the out-of-domain
        // receipt gate. Recorded BEFORE the dwell gate below so it refreshes every
        // frame, not once per dwell period.
        noteServerTick(serverTick);

        // [InputStats] (Warning) — the periodic drop-rate summary. Driven from
        // this once-per-tick hook and evaluated BEFORE the dwell gate below, so
        // the ~2s window is measured purely in SERVER-TICK time (no wall-clock —
        // the core forbids Date/chrono in this path; the window is derived from
        // TimeConfig::tickFrequency).
        maybeEmitInputStats(serverTick);

        const int32_t dwell = m_config.tierMinDwellTicks;
        if (dwell <= 0 || (serverTick % dwell) != 0)
        {
            return;
        }

        const int32_t deadline = dwell * kTierReapDeadlineDwellPeriods;

        m_tierTable.reapDeadHandles(serverTick, deadline);
        m_inputDelayQueue.reapDeadHandles(serverTick, deadline);

        // Claim map: liveness is read off the Address (wire) half only — a dead
        // wire drops every one of its slots, matching the queue's reap. Component
        // (owner) death is NOT read here; it rides the unregister contract
        // (forgetOwner) and the deliver callback, per the id-keyed design.
        for (auto it = m_delayedInputTargets.begin(); it != m_delayedInputTargets.end();)
        {
            it = (!it->first.address.isAlive())
                ? m_delayedInputTargets.erase(it)
                : std::next(it);
        }

        // [DelayShift] memo (T25): prune dead wires alongside the claim map, off
        // the same Address liveness. Keyed on Address, so a wire that goes away
        // does not keep its last-delay entry alive forever.
        for (auto it = m_lastDrainDelay.begin(); it != m_lastDrainDelay.end();)
        {
            it = (!it->first.isAlive()) ? m_lastDrainDelay.erase(it) : std::next(it);
        }

        // m_loggedOutOfRangeSlots is deliberately NOT pruned here: clearing it on
        // the reap cadence would let the warning re-fire every dwell period for a
        // persistently malformed topology. Truly one-shot is the intent.
    }

    // -----------------------------------------------------------------------
    // Lifecycle: forget an owner id. (The unregister-contract half of the id-keyed
    // claim map — replaces the TWeakObjectPtr GC liveness the core cannot hold.)
    // Called from the adapter's unregister path so a component's claim + dedup
    // watermark are dropped promptly on unregister, rather than waiting for GC to
    // make an engine handle stale. Any input still parked under the connection is
    // reclaimed by purgeOlderThan / reapConnections on the usual cadence.
    void forgetOwner(unsigned int id)
    {
        for (auto it = m_delayedInputTargets.begin(); it != m_delayedInputTargets.end();)
        {
            it = (it->second == id) ? m_delayedInputTargets.erase(it) : std::next(it);
        }
        m_captureTickWatermark.erase(id);
        m_lastPublishedTier.erase(id);
        m_lastDeliveredCaptureTick.erase(id);   // [InputGap] watermark (T25)
    }

    // -----------------------------------------------------------------------
    // Introspection — read-only, for the adapter and the Catch2 suite.
    const TierTable&  tierTable() const  { return m_tierTable; }
    const DelayQueue& delayQueue() const { return m_inputDelayQueue; }

    int32_t lookupTierIndex(const Address& addr) const { return m_tierTable.lookupTierIndex(addr); }

    std::size_t claimCount() const { return m_delayedInputTargets.size(); }

    // T12: cumulative count of capture ticks the out-of-domain receipt gate has
    // refused since construction. Never reset (the per-window counter that drives
    // the [InputDomain] log line is separate) — this is the lifetime total for
    // introspection and the Catch2 suite.
    std::size_t outOfDomainRejectCount() const { return m_outOfDomainRejectTotal; }

    // T3: cumulative count of capture ticks the server APPLIED but the monotonic
    // relay stream skipped as out-of-order-older (`parked && !acceptedNew` with the
    // delay queue actually accepting the entry). Never reset. This is the evidence
    // T9's probe reads: the depth>1 future must decide whether to widen the relay
    // gate on a measured rate, not on argument (RelayDelaySpectrumDesign.md §5.3a).
    // Zero in the ordered steady state, and NOT bumped by redundancy re-sends.
    std::size_t relayOooSkipCount() const { return m_relayOooSkipTotal; }

    bool hasClaim(const SlotKey& key) const
    {
        return m_delayedInputTargets.find(key) != m_delayedInputTargets.end();
    }

private:
    // -----------------------------------------------------------------------
    // The schedule stamp, narrowed to its wire type. (T3;
    // RelayDelaySpectrumDesign.md §5.2 — `dA` is one byte per relay entry.)
    // Effective delays are small tick counts (tier delays 1..4 today; the T11 floor
    // is itself capped at 44 by the client ring's residency limit), so this clamp
    // never fires in any shipped configuration — it exists so that a misconfigured
    // delay can only SATURATE the stamp, never wrap it into a tiny value and make
    // peers schedule an input wildly early.
    static uint8_t stampFromDelay(int32_t delay)
    {
        if (delay <= 0)
        {
            return 0;
        }
        return (delay >= 255) ? uint8_t{ 255 } : static_cast<uint8_t>(delay);
    }

    // [Verbose][RelaySkip] — one line per genuinely-new-but-out-of-order-older
    // capture tick the relay stream deliberately drops, plus the counters the
    // per-window summary and `relayOooSkipCount()` read. (T3, spectrum doc §5.3a.)
    // Verbose severity: in the ordered steady state this never fires, and when the
    // network does reorder we want the per-event detail available on demand without
    // it riding the default log. The per-WINDOW total is emitted at Warning by
    // maybeEmitInputStats, on the same window [InputStats]/[InputDomain] already use.
    void noteRelayOooSkip(unsigned int id, int32_t captureTick)
    {
        ++m_relayOooSkipTotal;
        ++m_windowRelayOooSkips;

        const auto it = m_captureTickWatermark.find(id);
        const int32_t watermark = (it == m_captureTickWatermark.end()) ? captureTick : it->second;
        SIMLOG(m_logger,
            "[Verbose][RelaySkip] id=%u captureTick=%d watermark=%d out-of-order-older; "
            "applied by the authority, not relayed",
            id, captureTick, watermark);
    }

    // First-seen watermark per owner id. `captureTick > seen` => newly accepted.
    // Missing id => first sample, accepted. Monotonic max; NOT a gate on delivery.
    bool noteCaptureTick(unsigned int id, int32_t captureTick)
    {
        const auto it = m_captureTickWatermark.find(id);
        if (it == m_captureTickWatermark.end())
        {
            m_captureTickWatermark.emplace(id, captureTick);
            return true;
        }
        const bool acceptedNew = captureTick > it->second;
        if (acceptedNew)
        {
            it->second = captureTick;
        }
        return acceptedNew;
    }

    // -----------------------------------------------------------------------
    // The out-of-domain receipt gate's decision + accounting. (T12.) Returns TRUE
    // when `captureTick` must be REFUSED; see the long rationale block on
    // receiveRemoteInput for the bounds and why this runs before everything else.
    //
    // Both bounds come from TimeConfig — there is deliberately no literal here.
    // `hardResyncThresholdTicks` is uint32_t and `rollbackWindowHardCap` int32_t,
    // so the upper bound is cast explicitly: an unsigned promotion of the sum would
    // wrap the comparison for an early-session (or negative) serverTick.
    //
    // Counting is split in two on purpose: `m_outOfDomainRejectTotal` is the
    // lifetime total (introspection), `m_windowOutOfDomainRejects` is the burst
    // counter that maybeEmitInputStats drains into ONE [InputDomain] line per
    // window — the SAME window mechanism [InputStats] already uses, deliberately
    // reused rather than duplicated, so a free-running client cannot produce one
    // log line per tick.
    bool rejectOutOfDomainCaptureTick(unsigned int id, int32_t captureTick)
    {
        if (!m_serverTickKnown)
        {
            return false;       // no tick reference yet — fail open, see noteServerTick
        }

        const int32_t lower = m_serverTick - m_config.rollbackWindowHardCap;
        const int32_t upper = m_serverTick + static_cast<int32_t>(m_config.hardResyncThresholdTicks);

        if (captureTick >= lower && captureTick <= upper)
        {
            return false;       // in domain — the accepted path is untouched
        }

        ++m_outOfDomainRejectTotal;
        ++m_windowOutOfDomainRejects;
        m_lastRejectedId          = id;
        m_lastRejectedCaptureTick = captureTick;
        m_lastRejectedServerTick  = m_serverTick;
        return true;
    }

    // [InputGap] (Warning) — the primary, cause-agnostic drop signal. (T25.)
    // Called at the drain deliver point with the ORIGINAL capture tick. It tracks
    // the last-delivered capture tick per owner id; when the next delivered tick
    // jumps by more than 1, a hole opened in that id's input stream from SOME
    // cause (strand, wire loss, dedup) and it is logged with the count of missing
    // ticks. Also feeds the [InputStats] window: every delivery bumps the total,
    // every gap the drop count. The watermark is reset in forgetOwner and is
    // therefore bounded by live owner ids.
    void noteDeliveredForGap(unsigned int id, int32_t deliveredCaptureTick)
    {
        ++m_windowDelivered;

        const auto it = m_lastDeliveredCaptureTick.find(id);
        if (it == m_lastDeliveredCaptureTick.end())
        {
            m_lastDeliveredCaptureTick.emplace(id, deliveredCaptureTick);
            return;         // first delivery for this id — no baseline to gap from
        }

        const int32_t last = it->second;
        if (deliveredCaptureTick > last + 1)
        {
            const int32_t dropped = deliveredCaptureTick - last - 1;
            SIMLOG(m_logger, "[Warning][InputGap] id=%u lastDelivered=%d now=%d dropped=%d",
                id, last, deliveredCaptureTick, dropped);
            m_windowDropped += dropped;
        }
        // A monotonic max — a late/reordered older tick neither logs a gap nor
        // rewinds the watermark.
        if (deliveredCaptureTick > last)
        {
            it->second = deliveredCaptureTick;
        }
    }

    // [DelayShift] (Warning) — correlate drops with tier moves. (T25.) Keyed on
    // the WIRE: a first observation records the delay and logs nothing; a later
    // drain that sees a different effective delay for that wire logs the shift.
    // Address is opaque, so the log identifies the wire by its stable hash.
    void noteDelayShift(const Address& wire, int32_t delay, int32_t tick)
    {
        const auto it = m_lastDrainDelay.find(wire);
        if (it == m_lastDrainDelay.end())
        {
            m_lastDrainDelay.emplace(wire, delay);
            return;
        }
        if (it->second != delay)
        {
            SIMLOG(m_logger, "[Warning][DelayShift] addr=%zu delay %d->%d tick=%d",
                static_cast<std::size_t>(std::hash<Address>{}(wire)),
                it->second, delay, tick);
            it->second = delay;
        }
    }

    // [InputStats] (Warning) — periodic drop-rate summary. (T25.) Called once per
    // tick from reapConnections; emits at most one line per ~2s window of SERVER
    // TICK time (window length derived from TimeConfig::tickFrequency — no
    // wall-clock). Emits only when the window actually carried remote input, so an
    // idle server does not heartbeat a Warning line every 2s. Counters reset each
    // window.
    //
    // T12 ALSO RIDES THIS WINDOW. The out-of-domain gate's rejections are summed
    // per window and emitted here as one [InputDomain] line — the rate limit the
    // task asks for IS this mechanism, not a second one: same window length, same
    // start tick, same reset point, same "emit only if the window carried
    // something" rule. The two lines are independent (a window can carry rejects
    // and no deliveries, or the reverse) but they share the one timer.
    void maybeEmitInputStats(int32_t serverTick)
    {
        const int32_t window = static_cast<int32_t>(2.0 * m_config.tickFrequency);
        if (window <= 0)
        {
            return;         // degenerate config — no meaningful window
        }

        if (!m_statsWindowStarted)
        {
            m_statsWindowStarted  = true;
            m_statsWindowStartTick = serverTick;
            return;
        }
        if (serverTick - m_statsWindowStartTick < window)
        {
            return;         // window still open
        }

        const int32_t total = m_windowDelivered + m_windowDropped;
        if (total > 0)
        {
            const int32_t pct = (m_windowDropped * 100) / total;
            SIMLOG(m_logger, "[Warning][InputStats] dropped %d / %d remote inputs = %d%%",
                m_windowDropped, total, pct);
        }

        // [InputDomain] (Warning) — ONE line per rejection burst (T12). Names the
        // most recent offender and the window it was judged against, so a garbage
        // capture tick is diagnosable from the log alone: a warm-up/free-running
        // client shows up as `captureTick` near 0 against a window in the hundreds.
        if (m_windowOutOfDomainRejects > 0)
        {
            const int32_t lower = m_lastRejectedServerTick - m_config.rollbackWindowHardCap;
            const int32_t upper = m_lastRejectedServerTick
                                + static_cast<int32_t>(m_config.hardResyncThresholdTicks);
            SIMLOG(m_logger,
                "[Warning][InputDomain] rejected %d out-of-domain capture ticks; "
                "last id=%u captureTick=%d serverTick=%d window=[%d,%d]",
                m_windowOutOfDomainRejects, m_lastRejectedId, m_lastRejectedCaptureTick,
                m_lastRejectedServerTick, lower, upper);
        }

        // [RelaySkip] (Warning) — ONE line per window naming how many genuinely-new
        // capture ticks the server applied but the monotonic relay stream skipped as
        // out-of-order-older (T3). Its own tag rather than a field appended to
        // [InputStats], following the [InputDomain] precedent: same window, same
        // reset point, no second timer, and the existing [InputStats] string that
        // the PIE scripts grep stays byte-identical. Silent when the window carried
        // no skips, which is the ordered steady state.
        if (m_windowRelayOooSkips > 0)
        {
            SIMLOG(m_logger,
                "[Warning][RelaySkip] %d out-of-order-older capture ticks applied but not "
                "relayed this window; lifetime %zu",
                m_windowRelayOooSkips, m_relayOooSkipTotal);
        }

        m_windowDelivered          = 0;
        m_windowDropped            = 0;
        m_windowOutOfDomainRejects = 0;
        m_windowRelayOooSkips      = 0;
        m_statsWindowStartTick     = serverTick;
    }

    // One-shot (id, slot) warning. Keyed on a packed 64-bit (id<<8 | slot) so a
    // component is warned about a given offending slot exactly once ever.
    void warnOutOfRangeSlotOnce(unsigned int id, uint8_t playerSlot)
    {
        const uint64_t logKey = (static_cast<uint64_t>(id) << 8) | static_cast<uint64_t>(playerSlot);
        if (m_loggedOutOfRangeSlots.find(logKey) != m_loggedOutOfRangeSlots.end())
        {
            return;
        }
        m_loggedOutOfRangeSlots.insert(logKey);
        SIMLOG(m_logger,
            "[Warning][InputDelay] id=%u derived player slot %u exceeds the supported "
            "maximum of %u -- falling back to undelayed delivery for this owner. This "
            "indicates a malformed connection topology; see ConnectionSlotKey.h. "
            "(Logged once per owner/slot pair.)",
            id, static_cast<unsigned int>(playerSlot),
            static_cast<unsigned int>(SlotKey::kMaxPlayerSlot));
    }

    const TimeConfig& m_config;

    // Declaration order is load-bearing: table before queue (queue binds the
    // table by reference), destroyed in reverse.
    TierTable  m_tierTable;
    DelayQueue m_inputDelayQueue;

    // Delivery routing table + the set the drain iterates. id-keyed (B2').
    std::unordered_map<SlotKey, unsigned int> m_delayedInputTargets;

    // Dedup watermark, per owner id. Bounded by live ids (reaped in forgetOwner).
    std::unordered_map<unsigned int, int32_t> m_captureTickWatermark;

    // Last tier PUBLISHED to each owner id (the publish-only-on-change dedup, T23).
    // Keyed on ownerId (the sink target), not Address, so couch-coop siblings on
    // one wire each converge. A missing entry => never published => baseline tier 0
    // (the replicated property default). Bounded by live ids (reaped in forgetOwner).
    std::unordered_map<unsigned int, int32_t> m_lastPublishedTier;

    // One-shot malformed-slot memo, keyed on packed (id<<8 | slot). Never pruned.
    std::unordered_set<uint64_t> m_loggedOutOfRangeSlots;

    // --- Input-path diagnostics (T25) --------------------------------------
    // [InputGap] per-owner last-delivered capture-tick watermark. id-keyed; reset
    // in forgetOwner, so bounded by live owner ids.
    std::unordered_map<unsigned int, int32_t> m_lastDeliveredCaptureTick;

    // [DelayShift] per-wire last-seen effective delay. Address-keyed; pruned for
    // dead wires in reapConnections.
    std::unordered_map<Address, int32_t> m_lastDrainDelay;

    // [InputStats] server-tick window accounting. No wall-clock: the window is a
    // span of server ticks (see maybeEmitInputStats).
    bool    m_statsWindowStarted   = false;
    int32_t m_statsWindowStartTick = 0;
    int32_t m_windowDelivered      = 0;
    int32_t m_windowDropped        = 0;

    // --- Out-of-domain receipt gate (T12) ----------------------------------
    // The adapter's game-thread server-tick reference, fed by noteRttSample (per
    // bundle) and reapConnections (per physics frame). `m_serverTickKnown` keeps
    // "never armed" (fail open) distinct from "armed at tick 0" — see
    // noteServerTick.
    bool    m_serverTickKnown = false;
    int32_t m_serverTick      = 0;

    // Lifetime total (introspection) + the per-window burst counter that
    // maybeEmitInputStats drains into ONE [InputDomain] line, with the most recent
    // offender kept for that line.
    std::size_t m_outOfDomainRejectTotal   = 0;
    int32_t     m_windowOutOfDomainRejects = 0;
    unsigned int m_lastRejectedId          = 0;
    int32_t     m_lastRejectedCaptureTick  = 0;
    int32_t     m_lastRejectedServerTick   = 0;

    // --- Relay tap (T3) ----------------------------------------------------
    // Lifetime total + per-window burst counter for out-of-order-older capture
    // ticks the server applied but did not relay. Rides the [InputStats] window,
    // exactly like the out-of-domain counters above. NOT reset in forgetOwner: this
    // is a session-scoped diagnostic, not per-owner state.
    std::size_t m_relayOooSkipTotal  = 0;
    int32_t     m_windowRelayOooSkips = 0;

    std::function<void(const char*)> m_logger;
};
