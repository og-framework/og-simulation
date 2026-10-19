<!-- SPDX-License-Identifier: MPL-2.0 -->
# `PhysicsEngineTraits.h` — rationale

The narrative behind `EngineLogSink`, `PhysicsEngineWorldHostTemplate`, the two concepts `PhysicsEngineTraits` and
`PhysicsEngineBinding`, and their self-check. Its guards are `PhysicsEngineTraits-guards.md` (none live): what could go
wrong in the header is a compile error.

**If this file and `PhysicsEngineTraits.h` disagree, the header is authoritative and this file is stale.** Fix this
file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by construction: STL only.
Everything below is true to a reader with no game engine, no physics engine and no other repository.

## §1 What it is

What a host built on og-simulation needs to know about a physics engine, and what a game must supply for that engine,
with no game type and no engine type in either.

* **`PhysicsEngineTraits`** is the concept of an engine's traits (§2). One traits struct per physics engine, written
  by that engine's adapter layer.
* **`PhysicsEngineBinding`** is the concept of a game's per-engine binding, the part of it any game supplies (§3).
  A game may refine it with requirements that name its own types.
* `EngineLogSink` (`void (*)(const char*)`) and `PhysicsEngineWorldHostTemplate`, the shape check of a world-host
  template, are the two helpers the concepts use.

**Who includes it.** A host that schedules an engine through og-simulation's tick driver (it requires
`PhysicsEngineTraits` of the engine's traits), and a game's own seam header, which selects a configuration's traits
and binding and asserts both concepts on them.

## §2 `PhysicsEngineTraits`

| member | what it is |
|---|---|
| `BodyAdapter`, `ReaderAdapter`, `QueryAdapter`, `Factory`, `VizQuery`, `VizReader` | the six og-simulation adapter types |
| `CategoryTable` | the form in which the engine takes a game's category-to-channel table |
| `kName` | the engine's backend name, convertible to `const char*` |
| `computeDeterminismFingerprint(logger)` | the engine's determinism fingerprint, or none (`std::optional<uint64_t>`) |
| `kHasWorldHost` | whether og-simulation's tick driver schedules this engine |
| `WorldHost<SimulatableT>` | only when `kHasWorldHost`: the world-host template, instantiated with the binding's simulatable type |
| `logEngineMessage` | only when `kHasWorldHost`: the engine log sink, a function whose address converts to `EngineLogSink` |

* **`CategoryTable` is an engine member** because engines take the table in different forms: one engine's adapter may
  take a `std::vector`, another's only an `std::initializer_list`. The binding concept checks the binding's table
  against it (§3), so a table in the wrong form fails there, not at the host's call.
* **The world-host template is checked by shape only:** the concept asks that `WorldHost` be a template of one type
  (`PhysicsEngineWorldHostTemplate`), because the engine traits do not know the game's simulatable type. The
  binding concept names the instance (§3). That the instance models og-simulation's `TickDriverWorldHost` is the
  driver-scheduled host's own requirement.
* **The log sink is required only of a driver-scheduled engine.** Only a driver-scheduled host reads it: it hands it
  to the engine's world host as its message callback (og-simulation-jolt's world host, for example, takes it as its
  runtime's log function). No host reads it of a self-scheduled engine, so its traits need none. The
  concept takes the sink's address (`&EngineTraitsT::logEngineMessage`), so a `static constexpr std::nullptr_t`
  stand-in, which would convert to a function pointer and make a null call wherever a host stored it, is refused: its
  address is a pointer to an object, not a function.
* **The concept is what every host needs, not everything a host reads.** A host may read further members of the
  traits; traits without them fail inside the host at its instantiation, not at the requirement.

## §3 `PhysicsEngineBinding`

| member | what it is |
|---|---|
| `Simulatable` | the simulatable type the world host is instantiated with |
| `categoryChannels()` | the game's category table, in the engine's `CategoryTable` form |
| `kParkSnap` | only when the engine has a world host: the render park-snap distance, convertible to `float` |

When the engine traits have a world host, the concept also requires `EngineTraitsT::WorldHost<BindingT::Simulatable>`
to name a type.

**Why the park snap is gated.** Only a driver-scheduled host reads it, in the presenter's controls (`PresentControls`
of `TickDriverPresenter.h`); a self-scheduled engine renders through its own interpolation, so its binding has none,
and the concept asks for it only when `kHasWorldHost` is true. It must convert to `float`, so a `std::nullopt_t`
stand-in does not model the concept for a driver-scheduled engine.

**What the concept leaves out.** Requirements that name a game type (a check over the game's character, for example)
are the game's refinement, not this concept: the refinement is this concept plus its own requirements, so a binding
models the refinement exactly when it models both. A host may also read binding members that no concept requires; a
binding without them fails inside the host at its instantiation.

## §4 The self-check

Namespace `physicsEngineSeamSelfCheck` holds reference shapes, as og-simulation's other concepts hold theirs. These
must model the concepts: a driver-scheduled engine (`DriverScheduledTraits`), a self-scheduled engine
(`SelfScheduledTraits`), a binding with a park snap (`DriverScheduledBinding`) over the driver-scheduled engine, and a
binding with only a simulatable and a category table (`SelfScheduledBinding`) over the self-scheduled engine. These
must not, each with one member missing or wrong:

| shape | what is wrong | the concept part it controls |
|---|---|---|
| `DriverScheduledTraitsWithoutWorldHost` | `kHasWorldHost` true, no `WorldHost` | the world-host half of `PhysicsEngineTraits` |
| `DriverScheduledTraitsWithNullLogSink` | a `std::nullptr_t` log sink | the log sink taken by address |
| `SelfScheduledBinding` over the driver-scheduled engine | no `kParkSnap` | the park snap of `PhysicsEngineBinding` |

Each negative `static_assert` is a control that its part can fail; each was seen failing, with its shape corrected,
when it was added.

**The namespace may be reopened.** A game's seam header may reopen `physicsEngineSeamSelfCheck` for the shapes of its
own refinement, built on `SelfScheduledBinding` and asserted against `DriverScheduledTraits` and
`SelfScheduledTraits` from here.

## §5 Provenance

Added to og-simulation on 2026-10-18. The header first lived in a game-engine adapter layer and moved here unchanged
apart from its assertion messages, because it names only STL. The design work behind it is private initiative
material and is **not distributed with this submodule**.
