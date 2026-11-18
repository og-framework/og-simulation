<!-- SPDX-License-Identifier: MPL-2.0 -->
# The floating-point mode of simulation code — rationale

Every translation unit that compiles og-simulation code compiles with IEEE-754 semantics and no contraction: MSVC
`/fp:precise`, clang and gcc `-ffp-contract=off`, never a fast-math mode. This file says why, and how each build sets it:
og-simulation's own CMake build (`CMakeLists.txt`), and the Unreal modules of a host project. It was written for
og-input-latency task 20, which set the mode.

**If this file and the build files disagree, the build files are authoritative and this file is stale.**

Engine source citations are to Unreal Engine 5.6's UnrealBuildTool (`Engine/Source/Programs/UnrealBuildTool/`). They are
written without backticks because the engine is outside every tree this document's lint resolves against. So are the names
of a host project's modules and files.

---

## 1. Why one mode, and why the precise one

- **Peers must step alike.** A client predicts and the server corrects. A prediction that differs from the authority only
  because the two binaries rounded differently still fails the correction comparator and costs a resim.
- **`/fp:fast` lets the optimiser rewrite arithmetic per call site.** It may reassociate a sum, multiply by a reciprocal
  instead of dividing, merge a `sin` and a `cos` of one angle into one call, and assume no NaN exists. Which rewrite it
  makes depends on inlining and on the optimisation state around the code, so two builds of the same source can differ.
  og-input-latency task 19 measured the two scripted golden traces of og-brawler-tests built with MSVC `/O2 /fp:fast` and
  `/O2 /fp:precise`. On the 7-character slope trace a compared field moved 1.09e-4 m/s within 2 ticks, at a landing.
- **Unreal gives the platforms different modes by default.** UnrealBuildTool compiles MSVC Win64 with `/fp:fast` for the
  default semantics (VCToolChain.cs, lines 1169-1185), and every clang toolchain, Linux and Android included, with
  `-ffp-contract=off` (ClangToolChain.cs, lines 691-705). A Win64 client and a Linux server would run different modes.
- **The precise mode is one class across compilers.** With `/fp:precise` on MSVC and `/fp:precise -ffp-contract=off` on
  clang-cl, both goldens record byte for byte alike (task 19b; again at task 20). That leaves the C library's
  transcendental functions and the standard library's sort order as the cross-platform differences, which this mode does
  not touch.
- **The optimisation state stops mattering.** `OGSIM_OPTIMIZE_OFF` (`CompilerControl.h`) turns the optimiser off in
  Development builds and is a no-op in Shipping. Under `/fp:fast` that made Development and Shipping builds round some
  helpers differently. Under `/fp:precise` a Development build and an `OGSIM_FORCE_OPTIMIZED` build record both goldens
  byte for byte alike (tasks 19 and 20).

## 2. Every translation unit, not every library

og-simulation is mostly headers: templates and inline functions, compiled in whichever translation unit includes them.
Several translation units of one program each compile their own copy, and the linker keeps one of them. So one
translation unit compiled with `/fp:fast` that instantiates a simulation template can supply the copy that every caller
runs. The mode therefore belongs on every module and target that compiles og-simulation code, its tests and its hosts
included, not only on og-simulation's own sources.

What this does not cover: a function of the engine or of another library that a simulation path calls and that is compiled
in a module with a different mode (for example an engine math inline instantiated in both). The linker may keep either
copy. No check covers that case.

## 3. The CMake build

`CMakeLists.txt` adds the options to `og_simulation` as `PUBLIC` compile options, so every target that links
`og_simulation` (og-simulation-jolt, og-brawler, the test executables) compiles with them too:

| compiler (`CMAKE_CXX_COMPILER_ID`, frontend) | options |
|---|---|
| MSVC | `/fp:precise` |
| Clang with the MSVC frontend (clang-cl) | `/fp:precise /clang:-ffp-contract=off` |
| any other (clang, gcc) | `-ffp-contract=off` |

- clang-cl also gets `-ffp-contract=off`, the flag Unreal's clang toolchains pass for precise semantics; with it, clang-cl
  and MSVC are the pair task 19b measured equal.
- MSVC's default is already `/fp:precise`, so for a plain MSVC configure the option changes nothing. It matters when the
  parent adds `/fp:fast`: target options come after it on the command line, MSVC keeps the last `/fp` (warning D9025),
  and so simulation code stays precise. Task 20's cross-build driver relies on that: its `/fp:fast` variant now compiles
  every simulation translation unit `/fp:precise`.
- clang's default is `-ffp-contract=on`, which fuses on any target with a fused multiply-add instruction (arm64 always
  has one). So an NDK or Linux CMake build of og-simulation changed numerics at task 20, towards Unreal's Android and
  Linux mode.
- A parent's `-ffast-math` is not undone. No option here cancels it, and none of this project's builds uses it.

## 4. Unreal modules

An Unreal module sets the mode with `FPSemantics = FPSemanticsMode.Precise;` in its module rules (ModuleRules.cs, lines
777-783), which makes UnrealBuildTool pass `/fp:precise` on MSVC and `-ffp-contract=off` on clang.

- **It needs a private precompiled header.** A module whose FP semantics differ from its target's cannot use a shared
  precompiled header. Without a private one UnrealBuildTool turns precompiled headers off for the module ("Overriding
  FPSemantics requires a private PCH. Disabling PCH usage", UEBuildModuleCPP.cs, lines 2366-2382).
- **og-simulation needs the header the shared one gave it.** Its Unreal branch uses the engine's `uint32`, `TEXT`,
  `checkf` and `FORCEINLINE` without including the headers that declare them, because the shared precompiled header is
  force-included into every file. With precompiled headers off, the OGSimulationTests build stopped with about 7,400 errors
  in 49 files. So each module names, as its private precompiled header, the engine header it had as its shared one:
  `PrivatePCHHeaderFile = EngineDirectory + "/Source/Runtime/Core/Public/CoreSharedPCH.h";` for a module that depends on
  Core only, and the engine's EngineSharedPCH.h (in Engine/Source/Runtime/Engine/Public) for one that depends on Engine. UnrealBuildTool compiles it once per
  module with the module's own flags and force-includes it, as it did the shared one (UEBuildModuleCPP.cs, lines
  1956-1958). A module that already uses no precompiled header needs only the `FPSemantics` line.
- **Every module that compiles og-simulation code sets it.** In the og-brawler-unreal project (task 20, 2026-11-18):

| module | why it compiles simulation code | private precompiled header |
|---|---|---|
| OGSimulation | og-simulation's sources (and glm's detail/glm.cpp, which sits under its module directory) | Core |
| OGSimulationJolt | og-simulation-jolt and the vendored Jolt sources | Core |
| OGSimulationJoltUnreal | the Unreal host of the Jolt backend; instantiates og-simulation and og-simulation-jolt headers | Engine |
| OGSimulationUnreal | the Unreal simulation host and session; instantiates og-simulation headers | Engine |
| OGSimulationNet | og-sim-net, which includes og-simulation headers | none (it uses no precompiled header) |
| OGBrawler | og-brawler's sources, the game's simulations | Core |
| OGBrawlerUnreal | the game module; instantiates every og-brawler simulation | Engine |
| OGBrawlerTests | og-brawler-tests, the golden traces included | Core |
| OGSimulationTests | og-simulation-tests and the og-sim-net tests | Core |

- **Modules left at the target's default**, because they compile no simulation code: glm (only its module stub; the
  library is headers, and its one source file compiles in OGSimulation), og-net's OGNet and OGNetUnreal (transport; they
  include no og-simulation header), DVolumeModule (its header stores a `glm::mat4`, and the module's sources compile no arithmetic) and
  ProceduralMountainSide (no og-simulation include). OGNetTests includes og-simulation's transport headers
  (`Network/SimInbox.h`, `Network/SimTransport.h`), which carry a receive time but compute no simulation state; it is
  its own executable, so it shares no copy with the game.
- **How to check a build:** every compile response file (`.obj.rsp`) under the module's intermediate directory names its
  `/fp` option. A module added later that compiles og-simulation code needs both lines.
- **The compile-time check** (og-input-latency task 20a). `CompilerControl.h` stops the compile with an `#error` when
  `_M_FP_FAST` or `__FAST_MATH__` is defined. MSVC defines `_M_FP_FAST` under `/fp:fast` only (`_M_FP_PRECISE` under
  `/fp:precise`, `_M_FP_STRICT` under `/fp:strict`); clang-cl defines both macros under `/fp:fast` and `-ffast-math`, and
  clang defines `__FAST_MATH__` under `-ffast-math` (one-file probes, MSVC 19.38, clang 19.1.5, NDK 27.2, 2026-11-18). On
  2026-11-18 at least one translation unit of each module in the table included the header, except OGSimulationNet, whose
  one source file includes no og-simulation header (UnrealBuildTool's `.dep.json` files). Measured: OGBrawler left on the
  target's default stops the OGBrawlerTests build in its unity file Module.OGBrawler.cpp with error C1189. The check does not see
  clang's contraction setting: clang-cl `/fp:precise` without `-ffp-contract=off` defines `_M_FP_CONTRACT` and compiles
  clean. Nor does it see a module none of whose files reaches the header.
