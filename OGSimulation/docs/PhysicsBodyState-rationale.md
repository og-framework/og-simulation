<!-- SPDX-License-Identifier: MPL-2.0 -->
# `OGSimulation/PhysicsBodyState.h` — rationale

Why there is more than one body-state wire shape, what the conversion bridge between them costs,
and the exhaustive list of places a body state is touched. The headers keep the contracts and the
soundness conditions at the risky lines; this file carries the map and the *why*.

**If this file and `OGSimulation/PhysicsBodyState.h` disagree, the header is authoritative and this
file is stale.** Fix this file; do not soften the header to match it.

Its companion is `OGSimulation/PhysicsDeclaration.h`, which turns the previously duck-typed
declaration contract into a concept. The two are one design and are described together here.

<!-- lint-external-ref: lockRotation -- FORWARD REFERENCE: the rotation-freezing flag on BodyDescriptor, added by the descriptor-widening seam task. This document's soundness condition is written against it deliberately, before it exists, because the condition is what makes the slim shape legitimate at all -->
<!-- lint-external-ref: LinearBodyStateTest.cpp -- test translation unit in the og-simulation-tests submodule, not distributed with this submodule -->

---

## 1. The problem: one wire shape for every body

A body-owning sub-simulation puts its body's post-solve state on the wire so the state can be
checksummed, compared, corrected and replayed. Until now there was exactly one shape for that:
`PhysicsBodyState` — position, rotation, linear velocity, angular velocity, 52 bytes.

That shape is right for a body whose orientation is a simulated result. It is 28 bytes of waste per
body for a body whose rotation is *frozen by its own descriptor*: the rotation is always identity
and the angular velocity is always zero, and both are shipped every tick anyway. The character
movement body is exactly that case, and the state composite it lives in is bounded — the correction
payload has a fixed byte budget, and a per-round packet budget sits above it.

So the wire shape had to become **the simulation's choice**. What it must NOT become is the
*engine's* problem: no adapter — Chaos, Jolt, or any future one — should need per-body-state-type
code, and no generic site should need a new branch.

## 2. Touch points — every place a body state is handled

Exhaustive as of this document. The column that matters is the last one: it is the reason a second
wire shape is cheap.

| Piece | Where | What it names | Who writes it per sim |
|---|---|---|---|
| Wire shapes | `OGSimulation/PhysicsBodyState.h` | `PhysicsBodyState`, `LinearBodyState`, `BodyStateLike` | nobody (shared) |
| Produce | `OGSimulation/PhysicsBodyAdapter.h` | `captureBodyState` | nobody (adapters) |
| Own | a sim's `State`, e.g. `OGBrawler/BrawlerMovementSimulation.h` | `SerializableFields` | **sim author** |
| Locate | `OGSimulation/PhysicsDeclaration.h` | `bodyStateOf` | **sim author** |
| Capture | `OGSimulation/SimulationIntegrationExecutor.h` | `captureBodyStatesAll` | nobody |
| Correct | `SimulationManagerUImpl.cpp` | `pushBodyState`, `SetTargetStateAtFrame` | nobody |
| Create | `SimulationManagerUImpl.cpp` | `createPhysicalObject`, `queryVolumes`, `attachmentOffset` | **sim author, in engine code** |
| Bindings | `OGSimulation/PhysicsDeclaration.h` | `PhysicsRuntimeBindings` | nobody (was: four copies) |
| Checksum / similarity | `OGSimulation/SimulationComposite.h` | `isSimilarTo` | nobody |

Both generic sites — capture and correct — deduce the type of what `bodyStateOf` returns. Neither
names a body-state type. **That is why a second body-state type slots in without touching either
one**, and it is the property the whole design leans on.

## 3. The choice rule

Stated once, in the header, at the top of the file:

* **free-rotating body → `PhysicsBodyState`** (52 B). Its orientation and spin are simulated
  results and belong on the wire.
* **rotation-locked body → `LinearBodyState`** (24 B). Its descriptor must set `lockRotation`.
* **body snapped from closed-form state every tick → keep it off the wire entirely.** The
  projectile pattern: the pose is re-derived each tick from state that IS on the wire, so
  serializing it would be redundant and would invite two sources of truth.

The rule lives at the declaration because the third option is the one people forget, and because
the second option is only legitimate under a condition — §4.

## 4. The conversion bridge, and the condition that makes it honest

`LinearBodyState` carries two conversions and nothing else:

* **capture bridge** — `operator=(const PhysicsBodyState&)`. Narrowing. Rotation and angular
  velocity are dropped.
* **push bridge** — `operator PhysicsBodyState() const`. Widening. It **fabricates** an identity
  rotation and a zero angular velocity.

### 4.1 The widening is implicit, deliberately, and must stay so

The engine's rewind push is written generically:

```cpp
pushBodyState(decl.bindings.ownBodyId, D::bodyStateOf(state.template get<S>()));
```

`pushBodyState` takes a `const PhysicsBodyState&`. The implicit conversion **at that call site** is
the entire reason this design was preferred to explicit per-shape overloads: a sim swapping its
body-state type edits its own header and nothing else. Mark the operator `explicit` and that call
site needs a hand edit — at which point there is no bridge left, only a second code path.

This is not left to good intentions. `LinearBodyStateTest.cpp` asserts
`std::is_convertible_v<LinearBodyState, PhysicsBodyState>`, which is false the moment someone adds
`explicit`, and the failure names the reason.

### 4.2 The narrowing is assignment-only, deliberately

There is **no converting constructor**, and that asymmetry is the whole safety design.

Capture only ever assigns into a member that already exists:

```cpp
D::bodyStateOf(state) = adapter.captureBodyState(decl.bindings.ownBodyId);
```

A converting constructor would buy that call site nothing while enabling two silent losses a
reviewer cannot see locally: lossy copy-initialization (`LinearBodyState x = full;`) and
lossy pass-by-value into any function taking a `LinearBodyState`. Both would compile, both would
throw away rotation, and neither would look like a conversion at the point of use.

Pinned by `!std::is_constructible_v<LinearBodyState, const PhysicsBodyState&>` — the mechanical
check that the absence is real rather than merely intended.

### 4.3 The soundness condition

> The fabricated identity rotation and zero angular velocity are sound **only for a body whose
> descriptor sets `lockRotation` to true.**

That flag is what makes the fabricated values *true* rather than a lie. Widen a free-rotating body
through this bridge and its orientation and spin die silently — no error, no warning, a plausible
pose, and a desync that looks like a physics bug.

This condition is written **on the operator itself**, not only here and not only as the choice rule,
because a reader meets the risk at that line and nowhere else. It is the same failure class this
codebase has already paid for twice: a fact that was true where it was written and false where it
was used.

The test pins the fabrication rather than describing it: the widened value is asserted to have
identity rotation and zero angular velocity, so the lossiness is documented behaviour with a
failing test behind it, not a discovery waiting to happen.

## 5. `BodyStateLike` — the two operations, and only those two

```cpp
template <typename T>
concept BodyStateLike = requires(T t, const PhysicsBodyState& f)
{
    t = f;
    { static_cast<PhysicsBodyState>(t) };
};
```

Assignable **from** a captured full state; convertible **to** one for the push. Those are exactly
the two generic sites, so the concept is the smallest thing that can be true. `PhysicsBodyState`
satisfies it trivially (copy-assign, copy-construct); `LinearBodyState` satisfies it through the
bridge. Any future shape that models the pair is admissible without further work.

`OGSimulation/SimulationIntegrationExecutor.h` asserts it inside the capture fold. The assertion
changes no logic — it converts a template-fold error hundreds of lines deep into one named line.

## 6. `PhysicsDeclaration` — what was duck-typed

Four folds reach into a sub-simulation's physics declaration by name: body creation, query-volume
registration, post-solve capture, and the rewind push. **No concept described that contract.** A
declaration missing a member failed inside a fold, far from the mistake.

`OGSimulation/PhysicsDeclaration.h` states it:

| Requirement | Why the folds need it |
|---|---|
| `descriptor()` | the body + shapes to create |
| `name` | the created object's debug name |
| `staticDataOf(gameStaticData)` | maps the GAME's aggregate static data to this sub-simulation's own slice |
| `queryVolumes(subStaticData)` | volumes to register after creation |
| `attachmentOffset(subStaticData)` | the local offset the attachment math re-snaps to |
| `StateType` + `bodyStateOf` | where the body state lives; the mutable overload's referent must be `BodyStateLike`, and the **const** overload must WIDEN to `PhysicsBodyState` — the rewind push reads through it |
| `bindings` | a **mutable member of the shared `PhysicsRuntimeBindings`** — `same_as`, not merely convertible: the creation fold writes all five of its members |

`staticDataOf` is the member that does not exist on any declaration yet, and it is here on purpose:
it is what lets the creation fold ask a declaration for its own static-data slice. Without it the
fold needs a hand-written per-declaration branch in engine code — today the single engine edit that
adding a sub-simulation still costs.

### 6.1 Why the assertions are not on the composite

`SimulationComposite.h` carries a doc comment pointing here and **no `static_assert`**. The concept
is parameterised on the game's aggregate static data type, and `SimulationPhysicsComposite` — an
engine-side alias — has no way to name it. The assertions belong in the game's own simulatable
header, one per declaration, where that type is in scope. Asserting earlier would also be false
today: existing declarations have no `staticDataOf` yet.

## 7. `PhysicsRuntimeBindings` — one struct, not four

`ownBodyId`, `parentBodyId`, `attachmentOffset`, shape ids, query-volume ids: the handles the engine
adapter fills in after it creates a declaration's body. Local-only, never serialized, never
corrected — which is precisely why they must not live in the state that gets overwritten by a
correction.

Every body-owning sub-simulation declares its own byte-identical `RuntimeBindings` copy. This header
adds the one shared definition; **the four per-sim copies are still there** and nothing uses the
shared type yet — see §9. A **root** body carries `parentBodyId == ownBodyId`: it is its own parent
rather than carrying a null id every consumer would have to special-case.

## 8. Recipe — adding a body-owning sub-simulation

1. **Pick a body-state shape** by §3.
2. `State` holds it; `SerializableFields` decides whether it is on the wire.
3. Write the declaration: `descriptor()`, `name`, `staticDataOf`, `queryVolumes`,
   `attachmentOffset`, `StateType`, both `bodyStateOf` overloads, and a `PhysicsRuntimeBindings`
   member.
4. Append the declaration to the simulatable's physics composite, and its `State` / input types to
   the corresponding composites.
5. Add the integrate block.
6. Assert the declaration against the concept in the game's simulatable header.

Nothing in this list is an engine edit — with the exception noted in §6, which `staticDataOf`
exists to remove.

## 9. What this does not do

* **It does not change any adapter.** `captureBodyState` still returns a full `PhysicsBodyState`
  from every adapter. The narrowing happens at the sim's own assignment; the widening at the push.
* **It does not put any of the four attack/projectile sub-simulations on the slim shape.** They
  still carry `PhysicsBodyState`, correctly — none of them locks rotation. The movement
  skeleton's `State::bodyState` HAS since been swapped to `LinearBodyState` and is the only
  adopter; the descriptor flag `lockRotation` does not exist yet, so what makes its drop sound in
  the meantime is stated at the swap site itself, not by the choice rule.
* **It does not put any existing declaration on the shared `PhysicsRuntimeBindings`.** The four
  per-sim `RuntimeBindings` copies remain. They are field-for-field identical to the shared struct
  and still distinct types, so each of them **fails** the concept's
  `{ d.bindings } -> std::same_as<PhysicsRuntimeBindings&>` requirement: adopting the shared type
  is a prerequisite for the assertions the next bullet defers, not a tidy-up.
* **It does not yet assert the concept anywhere.** The declarations lack `staticDataOf`; adding the
  member and the assertions is separate, sequenced work.
