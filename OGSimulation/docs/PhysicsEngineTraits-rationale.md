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
| `WorldHost<SimulatableT>` | the world-host template, instantiated with the binding's simulatable type |
| `logEngineMessage` | the engine log sink, a function whose address converts to `EngineLogSink` |

* **`CategoryTable` is an engine member** because engines take the table in different forms: one engine's adapter may
  take a `std::vector`, another's only an `std::initializer_list`. The binding concept checks the binding's table
  against it (§3), so a table in the wrong form fails there, not at the host's call.
* **The world-host template is checked by shape only:** the concept asks that `WorldHost` be a template of one type
  (`PhysicsEngineWorldHostTemplate`), because the engine traits do not know the game's simulatable type. The
  binding concept names the instance (§3). That the instance models og-simulation's `TickDriverWorldHost` is the
  driver-scheduled host's own requirement.
* **Every engine has a world host.** og-simulation's tick driver schedules every engine through its world host, so
  the concept requires the template and the log sink of every traits type. Until og-session-runtime task 6 a boolean
  world-host flag in the traits said whether the tick driver scheduled the engine, and both members were required
  only when it was true (§5).
* **The log sink.** The host hands it to the engine's world host as its message callback (og-simulation-jolt's world
  host, for example, takes it as its runtime's log function). The
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
| `kParkSnap` | the render park-snap distance, convertible to `float` |

The concept also requires `EngineTraitsT::WorldHost<BindingT::Simulatable>` to name a type.

**The park snap.** The host reads it into the presenter's controls (`PresentControls` of `TickDriverPresenter.h`). It
must convert to `float`, so a `std::nullopt_t` stand-in does not model the concept. Until og-session-runtime task 6
it was required only when the engine's world-host flag was true: a self-scheduled engine rendered through its own
interpolation, so its binding had none (§5).

**What the concept leaves out.** Requirements that name a game type (a check over the game's character, for example)
are the game's refinement, not this concept: the refinement is this concept plus its own requirements, so a binding
models the refinement exactly when it models both. A host may also read binding members that no concept requires; a
binding without them fails inside the host at its instantiation.

## §4 The self-check

Namespace `physicsEngineSeamSelfCheck` holds reference shapes, as og-simulation's other concepts hold theirs. These
must model the concepts: an engine with every member (`DriverScheduledTraits`), and a binding with a park snap
(`DriverScheduledBinding`, which is `BindingCore`, a simulatable and a category table, plus the park snap) over it.
These must not, each with one member missing or wrong:

| shape | what is wrong | the concept part it controls |
|---|---|---|
| `DriverScheduledTraitsWithoutWorldHost` | no `WorldHost` | the world-host template of `PhysicsEngineTraits`: a traits type without a world host does not model the concept |
| `DriverScheduledTraitsWithNullLogSink` | a `std::nullptr_t` log sink | the log sink taken by address |
| `BindingCore` over `DriverScheduledTraits` | no `kParkSnap` | the park snap of `PhysicsEngineBinding` |

Each negative `static_assert` is a control that its part can fail; each was seen failing, with its shape corrected,
when it was added. og-session-runtime task 6 saw the first fail again with a `WorldHost` added to its shape, once no
flag could exempt a traits type from the world host. The shapes keep the "driver-scheduled" names: every engine is
driver-scheduled now.

**The namespace may be reopened.** A game's seam header may reopen `physicsEngineSeamSelfCheck` for the shapes of its
own refinement, asserted against `DriverScheduledTraits` from here. A reopening header uses names of its own: the
names above are declared once, here.

## §5 Provenance

Added to og-simulation on 2026-10-18. The header first lived in a game-engine adapter layer and moved here unchanged
apart from its assertion messages, because it names only STL. The design work behind it is private initiative
material and is **not distributed with this submodule**.

**The self-scheduled shape, removed by og-session-runtime task 6 (2026-11-23).** Until then the traits carried a
boolean world-host flag, required by `PhysicsEngineTraits` and convertible to `bool`. An engine whose flag was false
was self-scheduled: it stepped itself, with no tick driver, so neither concept asked it for a world-host template, a
log sink or a park snap, and each concept was written as its unconditional requirements `&&` (flag false `||` the
world-host requirements). The self-check had a self-scheduled traits shape (the core members and the flag false) and a
binding shape with only a simulatable and a category table, both asserted to model the concepts; the latter was also
the negative binding shape over `DriverScheduledTraits`. The last self-scheduled engine was removed from every
consumer before task 6, so the flag was true wherever it was read. Task 6 removed the flag and both gates (each
concept is now one conjunction), the self-scheduled traits shape and the two positive asserts that named it, and
renamed the binding shape to `BindingCore`, the base of `DriverScheduledBinding` and the negative shape. Every
assert that stayed holds the verdict it held before, and the engine traits that remained model the concept unchanged
apart from losing the flag.
