<!-- SPDX-License-Identifier: MPL-2.0 -->
# og-tickdriver's module — rationale

og-tickdriver is the engine-free tick driver: `SimulationTickDriver` and its backend, step driver, hook concepts, presenter,
window, thread context, log, slot table and thread-chain executor. Its library sources under `OGTickDriver/` build as the
Unreal Engine module `OGTickDriver` and as the CMake target `og_tickdriver`. This file records where the files live, why
og-tickdriver is a module of its own, and how both builds are wired (og-session-runtime task 27b). It is the docs pointer of
`OGTickDriver.Build.cs`, `OGTickDriverModule.cpp`, `OGTickDriverExport.h` and og-tickdriver's `CMakeLists.txt`. Those files
state no prohibition, so there is no guards file.

**If this file and the build files disagree, the build files are authoritative and this file is stale.**

---

## §1 Where the files live

- **og-tickdriver is a folder of the og-simulation repository**, `og-simulation/og-tickdriver/`
  (`Plugins/OGSimulation/Source/OGSimulation/og-simulation/og-tickdriver/` in the plugin). It holds `OGTickDriver/` (the
  sources, and `docs/` with each file's guards and rationale), the `CMakeLists.txt` of §5 and an empty `.ubtignore` (§2).
  Every include of one of its headers is spelled `"OGTickDriver/<File>.h"`. Until task 27b the files were in
  `og-simulation/OGSimulation/`, compiled by the `OGSimulation` module. Only file-name lists in the design tools and the
  module tier table told them apart from og-sim there.
- **The module rules are in the plugin, not in og-simulation:** `Plugins/OGSimulation/Source/OGTickDriver/` holds
  `OGTickDriver.Build.cs` and `OGTickDriverModule.cpp` (which declares the module to the engine), as og-sim-net's sit
  outside its library folder (og-sim-net's `OGSimNet-rationale.md` §1). No source under `OGTickDriver/` includes an engine
  header.
- **The library has its own root folder, `og-tickdriver/`, rather than sitting beside `OGSimulation/`.** og-sim's include
  root is the og-simulation directory itself (`OGSimulation.Build.cs` adds `og-simulation/OGSimulation/..`; the
  `og_simulation` CMake target the same). Every og-sim include is spelled `"OGSimulation/<File>.h"`, so that root cannot be
  narrowed, and any folder placed directly in it is reachable from og-sim. Measured with clang-cl and only
  `-I<og-simulation>` (task 27b's probe): an og-sim header including `"OGTickDriver/TickDriverConcepts.h"` compiles when
  the header is at `og-simulation/OGTickDriver/`, and fails with "file not found" at `og-simulation/og-tickdriver/OGTickDriver/`.
  This is og-sim-net's and og-sim-session's shape (`og-sim-net/OGSimNet/`, `og-sim-session/OGSimSession/`).

## §2 Why a module: the include path is the gate

- og-tickdriver builds on og-sim and og-sim never includes og-tickdriver (the module tier table's tiers 0 and 1). As files
  inside the `OGSimulation` module, that held only because a lint said so. As their own module, the `OGSimulation` module
  and the `og_simulation` target do not have og-tickdriver's root on their include path. So a tick-driver include in an
  og-sim file that og-sim's own sources compile no longer compiles.
- **What each instrument gates.**
  - The Unreal build of the `OGSimulation` module compiles only og-sim's own sources, so it gates only the headers they
    include. A header-only og-sim file that only the layers above include (`SimulationManager.h`; `SimulationManager.cpp`
    includes nothing) is compiled there only with those layers' include paths, which hold og-tickdriver's root.
  - The CMake target `og_simulation_header_check` (og-simulation's `CMakeLists.txt`, in both standalone configurations)
    compiles every og-sim header alone with `og_simulation`'s include directories only. So the CMake build gates every
    og-sim header.
  - `clang_header_probe.ps1` does the same on clang-cl and libc++.
  - `module_tier_lint.ps1` (A1) reads every include.
- Measured (task 27b's seen-failing cases, `#include "OGTickDriver/TickDriverConcepts.h"` added to one og-sim header):
  - In `SimulationTimeContext.h`, which `SimulationTimeContext.cpp` compiles, all four fail: the Unreal build of the
    `OGSimulation` module (C1083, file not found), the core-only standalone CMake build (C1083), the probe and A1.
  - In `SimulationManager.h`, the Unreal build stays green. The core-only CMake build fails in `og_simulation_header_check`
    (C1083), and the probe and A1 fail.
- **`.ubtignore`:** UnrealBuildTool walks a module directory recursively, and `og-simulation/og-tickdriver/` is inside the
  `OGSimulation` module's directory. The empty `og-tickdriver/.ubtignore` stops that walk at the library root, as
  og-sim-net's root `.ubtignore` does (`OGSimNet-rationale.md` §2 gives the engine's code). The folder's own files are a
  `CMakeLists.txt` and that file, none of which compile. `OGTickDriver.Build.cs` adds `og-tickdriver/OGTickDriver` back
  with ConditionalAddModuleDirectory. Measured in the Editor build: the `OGTickDriver` unity file compiles
  `TickDriverWindow.cpp` and `OGTickDriverModule.cpp`, and the `OGSimulation` unity file no longer includes
  `TickDriverWindow.cpp`.

## §3 Module settings

`OGTickDriver.Build.cs` mirrors `OGSimulation.Build.cs`, whose code it compiled until task 27b:
- **`FPSemantics = FPSemanticsMode.Precise`**, as every module that compiles simulation code (`/fp:precise` on Win64;
  og-simulation's `FloatingPointMode-rationale.md`).
- **`PCHUsage = UseExplicitOrSharedPCHs` with `PrivatePCHHeaderFile` = the engine's CoreSharedPCH.h.** Overriding the
  floating-point mode turns a module's shared PCH off unless it has a private one (`FloatingPointMode-rationale.md` §4).
  The tick-driver headers were written against og-sim's PCH, which force-includes Core.
- Measured in the Editor build: the module's compile commands carry `/fp:precise`, `/std:c++20`, `/EHsc` (the Editor target
  turns exceptions on for every module, `OGSimNet-rationale.md` §3) and `/GR-`, and they use the module's own
  PCH.OGTickDriver.h (generated by UnrealBuildTool).
- Unity and warnings are left at their defaults, as in `OGSimulation.Build.cs`. The module's two sources compile as one unity
  file.

## §4 Dependencies, the export and the plugin listing

- **`OGSimulation` is a public dependency**: og-tickdriver's headers include og-sim's. **`Core` is private**, for the module
  macro in `OGTickDriverModule.cpp`.
- **The public include path is og-tickdriver's root**, `og-simulation/og-tickdriver`.
- **Who lists the module.** `OGSimulationJolt`, `OGSimulationSession` and `OGSimulationUnreal` list it as a public
  dependency, because their headers include tick-driver headers. The test module `OGSimulationTests` lists it as a private
  one. `OGBrawlerUnreal` lists it as a **private** dependency, only for the link. The game module includes no tick-driver
  header (the module tier table's `OGBrawlerUnreal` row forbids it), but it instantiates session templates that call
  `tickDriverPercentilesOf`, and in the modular Editor UnrealBuildTool did not link the module through the public
  dependencies of `OGSimulationUnreal`. Measured without it: `error LNK2019: unresolved external symbol
  "__declspec(dllimport) struct TickDriverPercentiles __cdecl tickDriverPercentilesOf(float *,unsigned int)"` in
  `UnrealEditor-OGBrawlerUnreal.dll`.
- **The one exported function** is `tickDriverPercentilesOf` (`TickDriverWindow.cpp`), marked `OGTICKDRIVER_API`.
  UnrealBuildTool defines that macro for the module (dllexport inside the module, dllimport in its dependents).
  `OGTickDriverExport.h` defines it empty under `OG_STANDALONE_BUILD`, as og-sim's `OGExport.h` does for `OGSIMULATION_API`.
  og-sim may not name og-tickdriver, so the macro cannot live there. Measured: the Editor's `UnrealEditor-OGTickDriver.dll`
  exports `tickDriverPercentilesOf` and the engine's module marker.
- **The plugin lists `OGTickDriver`** as a Runtime module, right after `OGSimulation`.

## §5 The CMake build

- `og-tickdriver/CMakeLists.txt` builds the static library `og_tickdriver` from `OGTickDriver/` (public include directory
  `og-tickdriver/`, public link `og_simulation`, the same floating-point options as `og_simulation`). It also builds
  `og_tickdriver_header_check`, which compiles each header alone without exceptions and RTTI, as og-sim-net's and
  og-sim-session's header checks do. og-simulation's `CMakeLists.txt` has the same for og-sim's headers,
  `og_simulation_header_check` (§2).
- It takes `og_simulation` from the parent project when the target exists. Otherwise it reads `OG_SIMULATION_DIR`, which
  defaults to the og-simulation checkout that holds it. Its consumers (og-simulation-jolt, og-sim-net's tests, og-sim-session,
  the plugin's `standalone/` project) find it next to the `og_simulation` they already have: in the same checkout, at the
  `og_simulation` target's source directory's `../og-tickdriver`.
- **`og_simulation`'s include directories do not reach it.** Measured in the core-only configuration's generated project:
  `og_simulation` has `og-simulation/OGSimulation/..`, plus glm's interface directory (the folder of the project that adds
  glm, here `standalone/`). `og_tickdriver` adds `og-simulation/og-tickdriver`. og-sim's root holds `og-tickdriver/`, not
  `OGTickDriver/`, so `"OGTickDriver/<File>.h"` does not resolve from og-sim.
- **The core-only configuration** (`-DOG_STANDALONE_CORE_ONLY=ON`) still builds `og_tickdriver`. og-simulation-jolt
  (`JoltTickWorldHost.cpp`) and og-simulation-tests need it, and so do some of og-sim's own test files there (for example
  `ClockLoopHarness.h`). What the configuration shows is that `og_simulation` itself neither links nor includes it.
