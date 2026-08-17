<!-- SPDX-License-Identifier: MPL-2.0 -->
# `ResimGatePolicy.h` — rationale

This is the narrative, history, why-not-the-naive-form derivation, and measurement record for
`resimGate` (`ResimGatePolicy.h`). The header keeps every fence, the per-parameter contracts, and a
short orientation block; this doc carries everything else. **Pilot note:** this doc/header split was
piloted here by backlog item 65 (RN-15) — see that task's impl notes for the verdict on whether the
split works. The header is the operational reference; read it first. Come here when you need the
*why*, not the *what*.

Origin: `og-netcode-v2-input-relay` items 45/46/47/48, `design_task43_resim_gate_fix.md` §1/§3/§3.1/§4,
`finding_task31_resim_rate.md` (the defect this repairs), and `ReviewNotes.md` RN-15 (this split).

## 1. What the gate used to be, and why it changed

The gate was **level-triggered** on two per-slot bitsets. `needsResimulation` re-derived its answer
every physics frame from a newest-first scan starting *at* the frontier, and `pushPredictionTick`
copied the frontier's `m_isResimulated` bit forward to hold the gate closed after a completed resim.

That inheritance was a **load-bearing guard, not a bug** (design §1): without it, the just-replayed
slot one below an unflagged frontier re-triggered a 1-tick resim every tick, forever. The actual
defect was the **re-open condition** — only a correction landing *exactly* on the frontier slot could
clear the shadowing bit, so the trigger rate measured clock alignment (~2 events against ~8,759
behind-frontier corrections in the archived run) instead of divergence. The inheritance line was
deleted *because* the reader (`m_isResimulated`) is gone — never on its own; a partial removal would
have reopened the storm the guard existed to prevent.

The replacement is **edge-triggered**: a landed correction is an event that may set a per-character
`m_pendingResimAnchorTick` (one atomic word, CAS-max, coalescing to the newest tick).
`needsResimulation()` is `anchor != 0 && anchor != frontier` — nothing is derived from per-slot bits,
there is no scan. The resim-completion edge consumes the anchor with a literal CAS against the value
captured at prepare time; a correction landing mid-replay makes that CAS fail and survives as the next
trigger. Termination is now structural, not policed by a re-derivation of state the replay just wrote.

## 2. Why the policy arithmetic lives in its own STL-only header

Exactly the `correctionRotation` argument (`Network/CorrectionRotation.h`), for exactly the same
reason. `shouldSetPendingAnchor` is consulted from `StateCorrectionCache::tryInsertingCorrectState`'s
hit branch on the **game thread** (W1); `policyEnforcesDepthCeiling` and `isAnchorWithinDepthPolicy`
are consulted from `SimulationReconciliation::checkDivergenceAll`, per character, on the **physics
thread** (R1); `classifyResimSlotWrite` is consulted from
`StateCorrectionCache::tryInsertingResimulatedState` on the **physics thread**, reading landing stamps
the game thread wrote. Neither call site can be reached from a test without a cache or a storage tuple
respectively. Pure predicates here can be swept exhaustively; the wiring is then proven by one case per
site (`ResimGate.Policy.TheResimGateProbeAccessorObservesTheShippedFeed` and neighbours in
`ResimGatePolicyTest.cpp`, `og-simulation-tests`). A green kernel with an unwired caller ships the
legacy gate, so both halves are required — this is *why* the header stays deliberately thin on
integration and thick on the predicates themselves.

## 3. Why there is no trigger cooldown — the full argument

(User ruling, 2026-08-11, during item 45's implementation. Design §4 and backlog items 45/46 both
named a `resimCooldownTicks` rate ceiling; it was built, then removed on this ruling. Recorded in full
here because a reader who finds it named in those documents and absent from the code must find the
reason, not a hole.)

The argument is about correctness, not cost:

1. A cooldown does the thing this whole item exists to stop. Item 45 repairs a gate that absorbed
   corrections *known* to disagree with prediction and acted on none of them. A rate ceiling takes a
   correction known to disagree and defers acting on it for up to R frames, during which the client
   free-runs on state the authority has already contradicted. That is the same defect with a smaller
   constant.
2. **The throttle is already structural**, and it is demand-driven rather than arbitrary: the gate is
   consulted only on non-resim physics frames (Chaos calls `TriggerRewindIfNeeded_Internal` on solver
   advances, never inside its own rewind loop), and the anchor is consumed only on the completion edge.
   So at most one resim can be in flight, and at most one more can be pending. Corrections arriving
   during a replay coalesce into the pending anchor via the CAS-max and fire once, as a single deeper
   replay, instead of N shallow ones. The bound is "one resim per completed resim", which scales with
   what the machine can actually finish.
   This makes `ACompletedResimClosesTheGateAndItStaysClosed` (the termination tripwire,
   `og-simulation-tests`) the **cost** bound as well as the correctness bound. If that case ever goes
   red, the rate ceiling is gone too.
3. The storm this point once argued "sequencing forbids" already ships. `OnDisagreement` combined
   with item 30's degenerate always-false verdict has been the live shipped configuration since
   item 43 (`Config/DefaultEngine.ini:371` — check that file for the current value, not this
   sentence, since it can change again). Item 43 measured it directly: resims fired on 86–87% of
   physics frames at 6.35×/4.18× integration cost (`finding_task43_resim_gate_live.md`). The user
   ruling that shipped it anyway (`current_state.md`, 2026-08-12) is that the direct behavioural
   improvement — a proxy's wrong prediction converging instead of persisting — outweighs the
   degenerate metrics; the storm is accepted, not prevented by sequencing. Item 46 (the *compiled
   default's* own flip to `OnDisagreement`) remains blocked on item 30 — that only governs what a
   build with no ini override runs, not whether this storm runs today.
4. If the measured rate is still unaffordable once the verdict works (design §4's honest unknown:
   remote proxies may carry a structural disagreement rate), the correct knob is **per-class
   triggering** — proxies weighted differently from locally-predicted characters — not a blanket delay.
   Per-class triggering costs accuracy where accuracy is cheap; a cooldown costs it everywhere,
   including on the local character where any disagreement at all is evidence of a real defect
   (design §4's floor).

A correction arriving mid-replay must therefore **re-anchor**, never restart and never wait: the
running resim is left alone (nothing can interrupt it), the anchor moves forward to the newer tick, and
the next non-resim frame resimulates from there. That is exactly what the consume-CAS produces.

## 4. The one known, deliberate divergence from the legacy gate

On a cache in which **no resim has yet completed** and **no correction has yet landed on the
frontier**, every `m_isResimulated` bit was false under the legacy gate, so its scan could see a
behind-frontier corrected slot and would open the gate once. `FrontierExact` does not reproduce that
corner — it costs at most one trigger per character per **virgin cache** (session start, and each hard
resync — `wipeCache` cleared those bits too) against a measured population of thousands of corrections.
That population size is why item 45's acceptance criterion asks for *statistical
indistinguishability* rather than bit-for-bit equality. Reproducing the corner exactly would mean
keeping the shadow bitset alive purely to emulate a pre-steady-state edge case — the stale-truth trap
that retiring `m_isResimulated` exists to close.

## 5. Why the depth ceiling only applies under `OnDisagreement`

The ceiling exists to bound the **disagreement** trigger's worst-case depth (design §4: the verdict
gate supplies intent, the depth skip supplies boundedness). Under the legacy policy the trigger rate is
already bounded — by frontier-exact coincidence, to ~2 events per archived run — so it has nothing to
bound, and applying it anyway would cost behaviour-neutrality: the depth skip *drops* a trigger the
legacy gate retries. An anchor stranded more than `rollbackWindowTicks` below the frontier is reachable
under the legacy gate via a long stranded-resim episode (item 42's I5 class — ~20% of prepares never
reach the apply edge, and the frontier keeps advancing throughout). Legacy retries it at growing depth;
the skip abandons it. That is a real behaviour change in a regime whose duration nobody has measured
yet — which is why the ceiling is gated on the trigger policy rather than always on.

The shipped config (`Config/DefaultEngine.ini:371`, `OnDisagreement` since item 43) already turns the
trigger and its depth ceiling on together — this is the live configuration §4's cost envelope is
computed for, and item 43 measured it directly (§10). Item 46 (the *compiled default's* own flip) is
the separate, still-blocked question of what a build with no ini override runs; it does not gate
whether this configuration runs today.

## 6. The defect this repairs — the HOLLOW ANCHOR

`tryInsertingCorrectState` writes authority state into slot T on the game thread. An in-flight replay
on the physics thread then reaches tick T, and `tryInsertingResimulatedState` overwrote slot T with the
*replayed* state, unconditionally — while `m_containsCorrectTick` stayed set, so the slot claimed
authority provenance over a re-derivation. Item 45's surviving anchor then correctly triggered a
follow-up resim at T, `prepareResimAll` restored the clobbered state, and the replay faithfully
reproduced the same prediction. **The trigger was real; the data it pointed at was gone.**

Pre-existing in the clobber, new in that it matters: before item 45 a mid-replay landing triggered
nothing, so nothing was waiting on that data. Item 45 made the trigger correct and, in doing so, made
this pre-existing clobber observable for the first time.

## 7. The replay write rule, in full — "authority beats a re-derivation of it"

The rule is **protect-all-corrected**, and it is one bit wide. A replay never overwrites a slot with
`m_containsCorrectTick` set — not "never overwrites a *fresh* one", never overwrites *any* of them. The
reason is the invariant followed to its conclusion (item 47 amendment 2, after user review): even a
"stale" corrected slot holds server truth that the replay does not supersede, because the replay
derives from the old *prediction* at the shared restore tick, not from newer authority. There is no
input to a replay that makes its output better than the authority state it would overwrite.

Costs, checked before adopting it rather than after:
- the replay computes its live trajectory regardless — the cache write is a *record* of what it
  computed, not a step in computing it;
- no production consumer needs a corrected slot to hold replay output (`m_isResimulated`, the only
  provenance reader, retired with item 45);
- the frontier slot is ruled the same way *on purpose* (§7a below).

Two consequences worth stating as invariants, because both were broken before this item:
1. a replay never overwrites a slot carrying a fresh, unconsumed correction (authority beats a
   re-derivation of it); and
2. **the provenance bit cannot lie.** Nothing authority-marked is ever overwritten, so no bit is ever
   cleared, and "bit set ⇒ the state in this slot is authority-grade" holds *by construction* rather
   than by discipline across sites. The committed pre-47 state — bit set, state replayed — was the
   worst of both.

**"Authority-grade" means adopted-authority *or* within-tolerance-of-authority.** On an agreeing
landing, `tryInsertingCorrectState` sets the bit and *skips* the state copy (`if
(!predictionWasCorrect)`), so the slot keeps the predicted value — which the authority has explicitly
certified as matching. Protect-all preserves both readings; a rule that protected only adopted state
would need a second bit to tell them apart.

### 7a. The frontier slot is not an exception

(Item 47 amendment 3.) The frontier slot is what `applyResimAll` reads to publish the resim's result
into live state, so protecting it means a resim that ends on a corrected frontier *publishes the
authority state* rather than the replay's. That is the invariant's own logic applied at the one slot
where it is externally visible, and it is pinned by a named test case rather than implied.

## 8. Fresh vs. stale — full classification derivation

This split is **classification only**. The write decision above depends on exactly one input —
`slotContainsCorrectTick`. Everything else `classifyResimSlotWrite` takes exists to split the
protections into two *populations* for the probe: `freshClobbersAvoided` is the live rate of the §6
defect, `staleClobbersAvoided` is hygiene. Under protect-all the two produce the same action; only the
counter differs. That is what keeps item 47's cross-thread reads observational (see the threading note
on `StateCorrectionCache::m_slotLandingSeqNr`'s declaration).

A slot is FRESH iff **either** clause holds, and **neither clause alone suffices**, because the defect
has three exposure populations:

- **(a) mid-replay landings** — a correction lands on the game thread while the physics thread is
  replaying;
- **(b) the multi-character min-fold** — a character's corrected slot *above* the shared restore tick,
  landed *before* prepare;
- **(c) post-item-46 agreeing landings above the anchor.**

**Clause 1 — landing sequence** (`slotLandingSeqNr > preparedLandingSeqNr`). A per-cache monotonic
counter stamped on each landing's slot, captured at prepare. Catches (a) exactly — "this slot was
written after this resim was prepared" is the definition of mid-replay. It calls (b) and (c) *stale*,
because those landed before the prepare.

**Clause 2 — tick vs. the per-cache captured anchor** (`slotTick >= capturedAnchorTick`). Catches (b)
and (c): a corrected slot at or above the tick *this cache* anchored is information the resim was
supposed to act on. It misses (a) when the mid-replay landing sits *below* the captured anchor, which
is reachable because the restore tick is a min across characters and another character's deeper anchor
can drag the span below this one's.

**Why the naive form fails, precisely:** `containsCorrectTick && tick > runningAnchor` reaches for a
single global (min-folded) anchor. With that anchor, population (b) misclassifies wholesale — the
newer-anchored character's slots sit above the global min, so everything reads "fresh" and the counter
stops discriminating. The anchor compared against must be the **per-cache** capture taken at
`prepareResimAll`, never the folded min. This is the trap the header's fence exists to stop a reader
walking into; this section is the full reason the trap is real, not just asserted.

`capturedAnchorTick == 0` means this resim was prepared with no anchor pending (an engine-side rewind
we did not ask for). Clause 2 is then vacuously true and every corrected slot classifies fresh. That is
deliberate over-protection in the safe direction, and it is also what keeps the single-character
structural zero below honest.

**Stale is a 2+-character-only signal — a free classifier-wiring check** (item 47's reachability
analysis, inherited rather than re-derived). For a single character the replay span is
`anchor+1..frontier` and "stale" needs `tick < capturedAnchor`: disjoint ranges. The only reachable
stale population is a non-min character B restored at the shared min, whose span `min+1..frontier` dips
below B's own captured anchor `A_B`, passing B's older corrections in `(min, A_B)`. So
`staleClobbersAvoided` must read 0 in any single-character session; a nonzero there means the classifier
is wired to the wrong anchor, not that a new population appeared.

## 9. The mutation-run finding — why there is one function, not a predicate plus a counter

`ResimSlotWriteOutcome::outcome != Written` **is** the protection — the action is derived from the
classifier's return value, rather than decided beside it, and that is a correctness property, not a
style choice. It was found by a mutation run: an earlier draft had a separate
`replayMayOverwriteSlot(bit)` predicate at the write site and used `classifyResimSlotWrite` only for
the counter. Breaking that predicate alone (a one-line mutation) produced a build in which the slot
*was* overwritten while the outcome still reported `ProtectedFresh` — i.e. `freshClobbersAvoided`
counting clobbers it had not avoided, which is precisely the "instrument measuring the wrong quantity
under the right name" failure this initiative has shipped five times over. One function, one truth: the
counter cannot disagree with the action because it *is* the action.

That does not make the fresh/stale split load-bearing for the write decision itself — whether
`classifyResimSlotWrite` returns `Written` depends on `slotContainsCorrectTick` and nothing else; the
four other parameters only choose *between* the two protected classes. So the observational argument on
the landing stamps holds exactly: a torn or stale stamp can mislabel a counter, and can never
mis-decide a write.

If a future edit needs the write rule to stop being "the bit alone", the provenance invariant (§7,
point 2) stops holding by construction, and item 47 amendment 2's fallback shape — clear
`m_containsCorrectTick` on any overwrite, and rule explicitly on the T4 capture-tick reference, which a
replayed state was *not* produced by — becomes mandatory rather than optional. Nobody has needed that
fallback so far; recorded here so a future implementer does not have to re-derive it.

## 10. Measurement records

- Legacy re-open condition (§1): ~2 frontier-exact events against ~8,759 behind-frontier corrections
  in the archived run — the finding behind `finding_task31_resim_rate.md` / item 45.
- No-cooldown termination tripwire (§3): `ACompletedResimClosesTheGateAndItStaysClosed`,
  `og-simulation-tests` — doubles as the cost bound.
- Virgin-cache divergence (§4): bounded to ≤1 trigger per character per virgin cache (session start,
  each hard resync) against a measured population of thousands of corrections.
- Depth-ceiling regime (§5): ~2 events/run under `FrontierExact` (nothing to bound); item 42's I5 class
  measured ~20% of prepares never reaching the apply edge under the legacy gate.
- Design §4's modelled cost of the degenerate always-false verdict under `OnDisagreement`: a 3-6x
  physics-cost storm — superseded by item 43's live measurement (6.35×/4.18× integration cost,
  above), not a hypothetical this sequencing prevents. Items 28→30 are sequenced as COST REDUCTIONS
  for a gate that already ships (`Config/DefaultEngine.ini:371`, since item 43), not as blockers
  that prevent it from shipping; item 46 (the compiled default's own flip) remains sequenced after
  30.

## 11. Pilot verdict pointer

The PASS/FAIL verdict on whether this extraction actually works — applied to the header *alone*, doc
closed — lives in `impl/impl_notes_task65.md` in the `og-netcode-v2-input-relay` initiative workspace,
not in this file. This doc's job is to hold the content that moved; it is not the judge of whether
moving it was the right call.
