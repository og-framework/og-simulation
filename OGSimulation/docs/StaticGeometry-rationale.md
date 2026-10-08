<!-- SPDX-License-Identifier: MPL-2.0 -->
# `StaticGeometry.h` — rationale

Engine-independent descriptors for the static (never-moving) collision geometry of a level, and the
concept a physics backend implements to build its static world from them. The header holds the
licence, a docs pointer, the code and a compile-time self-check; this file carries the why.

**If this file and `StaticGeometry.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** It names no physics
engine and no game engine. Units are centimetres, Z up, as in `QueryGeometry.h`.

---

## 1. Why statics get their own descriptors

A level's static geometry is authored in the host engine's editor. A physics backend that runs
outside that engine needs the same geometry, so the host walks its level once, emits a
`StaticWorldDescription`, and the backend builds its static world from it. Neither side names the
other: the host knows only these descriptors, and the backend knows only these descriptors.

Statics are built **once** and are never part of a rollback snapshot
(`PhysicsWorldAdapter.h` :: `SnapshotCoverage`, `staticShapes`).

## 2. The shapes

Exactly five shapes, the set the first host importer emits:

| type | fields | notes |
|---|---|---|
| `StaticBox` | `halfExtents` | half the edge length on each local axis |
| `StaticSphere` | `radius` | |
| `StaticCapsuleZ` | `radius`, `totalHalfHeight` | the capsule's axis is local **Z** |
| `StaticConvexHull` | `points` | the hull of the points; the backend computes the hull |
| `StaticTriangleMesh` | `vertices`, `indices` | three indices per triangle, into `vertices` |

Cylinders, height fields and planes are deliberately absent. Adding one is a new alternative in
`StaticShape`; the report and the index helper follow automatically (§4), and the round-trip test
refuses to compile until it gets an arm for the new shape.

### 2.1 `totalHalfHeight` includes the hemispheres

`StaticCapsuleZ::totalHalfHeight` is measured from the centre to the tip of either hemisphere, so it
is never less than `radius`. That is the convention of `QueryGeometry.h` :: `CapsuleGeometry`,
`halfHeight`, and of the host engine's capsule component that feeds it, so a host can copy the value
through unchanged. Some physics backends describe a capsule by the half-height of its **cylinder**
section instead; such a backend's builder subtracts `radius`. The field is named `totalHalfHeight`,
not `halfHeight`, so that the conversion is visible at the line where the builder reads it.

## 3. `StaticShapeDescriptor` — one placed shape

| field | meaning | default |
|---|---|---|
| `shape` | one of the five shapes | a zero-size `StaticBox` |
| `localToWorld` | the shape's transform in level space | identity |
| `categories` | the collision categories the shape is **in** (`QueryGeometry.h` :: `CollisionCategories`) | none |
| `blockingCategories` | the categories the shape **collides with** | none |
| `friction`, `restitution` | surface response | `0` |
| `stableKey` | a 64-bit key, identical on every peer for the same shape | `0` |

`categories` and `blockingCategories` mirror the pair on the dynamic side
(`QueryGeometry.h` :: `ShapeDescriptor`, `categories`, `blockingCategories`). How a backend combines
two shapes' pairs into "these two collide" is that backend's rule; this header only carries both
sets so that the rule has what it needs.

`friction` and `restitution` default to zero, and so does `stableKey`. The defaults exist so that a
descriptor is never left uninitialised; they are not tuned values. A host importer fills every field.

`stableKey` lets a backend add statics in the same order on every peer (sorting by key), which keeps
static body creation, and therefore anything that depends on creation order, identical across peers.
How the key is derived is the host importer's business.

## 4. `StaticWorldBuildReport` and the shape index

The report counts shapes **per type**, in `shapeCountByType`, indexed by the shape's position in the
`StaticShape` variant, plus the seconds the build took. `kStaticShapeIndex<Shape>` gives that position
at compile time and `countOf<Shape>()` reads it, so callers never hard-code an index.

The array's size is `kStaticShapeTypeCount`, which is `std::variant_size_v<StaticShape>`. A new
alternative therefore grows the report by itself; there is no second list to keep in step.
`kStaticShapeIndex` of a type that is not an alternative fails to compile.

## 5. `StaticWorldBuilder`

A backend implements `build(const StaticWorldDescription&)`, returning a `StaticWorldBuildReport`.
The description is taken by `const` reference: building statics does not consume or edit the host's
description.

The self-check block `staticWorldBuilderSelfCheck` pins the concept with one conforming type and two
near-misses (`ConsumesTheDescription` takes a non-`const` description; `ReportsSomethingElse`
returns `bool`), and pins the alternative order of the index helper.

## 6. Guards

**None.** The shape set, the index order and the builder's signature are compile-checked (§4, §5).
The remaining obligations, such as subtracting `radius` for a cylinder-section capsule or adding
statics in `stableKey` order, are typed in a backend's builder, where a tag in this header would not
be in view.

## 7. Provenance

Added 2026-10-06 together with `PhysicsWorldAdapter.h`. The design work behind it is private
initiative material and is **not distributed with this submodule**.
