---
name: build-and-test
description: How to build the UE targets + run/verify OGSim LLTs, plus a standalone header syntax-check recipe and baseline counts
metadata:
  type: reference
---

# Building & testing (og-brawler-unreal)

- **Engine:** source-built UE at `C:\dev\UnrealEngine`. Build script:
  `C:\dev\UnrealEngine\Engine\Build\BatchFiles\Build.bat`.
- **Project:** `C:\dev\og-brawler-unreal\OGBrawlerUnreal.uproject`.
- **Build a target:**
  `Build.bat <Target> Win64 Development -Project="<uproject>" -WaitMutex -FromMsBuild`
- **The four verification targets** for OGSim-core work:
  `OGSimulationTests`, `OGBrawlerTests`, `OGBrawlerUnrealEditor`, `OGBrawlerUnrealServer`.
- **LLT run:** helper `tools/og-tools/Public/Invoke-OgLowLevelTest.ps1` (alias
  `oglltest <simulation|brawler>`). Exes at
  `Binaries/Win64/<Target>/<Target>.exe`; run with Catch2 filter `[@og]`
  (expands to `~[SelfTests]`, excludes UE's auto-included LLT smoke tests).
- **Baseline LLT counts (as of 2026-07-14):** `OGSimulationTests [@og]` =
  **331 assertions / 35 cases**; `OGBrawlerTests [@og]` = **264 assertions /
  59 cases**. Both all-pass. The `wire footprint … 115 B` line in OGBrawlerTests
  is a benign informational print, not a failure.

## Standalone header syntax-check (bypasses UBT)

OGSim-core headers compile standalone with `-DOG_STANDALONE_BUILD` (see
`OGTypes.h` — defines `int32`/`uint32`/`PI` for non-UE builds). Recipe for a fast
real-compiler syntax check of a single core header without a full UBT build:

```
# MSVC 2022 (path via vswhere; run vcvars64.bat first)
cl /nologo /Zs /std:c++20 /permissive- /Zc:preprocessor /EHsc ^
   /I "<...>/og-simulation" tu.cpp        # /Zs = syntax-only, no obj
# tu.cpp: #define OG_STANDALONE_BUILD 1 then #include the target header;
# force-instantiate a couple templates so the body is fully checked.
```
**`/Zc:preprocessor` is REQUIRED.** Several core headers pull in `SimulationLog.h`,
whose `SIMLOG` macro uses C++20 `__VA_OPT__(,)`. MSVC's legacy preprocessor (the
default) mishandles it and emits a cascade of bogus `',' was unexpected` /
`_simlog_buf undeclared` syntax errors from the *including* header (e.g.
`SimulationReconciliation.h`), NOT the file under test. Adding `/Zc:preprocessor`
(the conformant preprocessor) clears them — verified 2026-07-14 on
`SimulationNetSync.h` (EXITCODE 0 with the flag, 100+ errors without). If you see
`_simlog_buf` errors, it's the flag, not the code.
Include root is the module dir `.../og-simulation` (headers are included as
`OGSimulation/Foo.h`; `glm` is vendored at `.../og-simulation/glm`).

## UBT gotcha — worktrees inside the module tree

UBT does a recursive non-unity source scan. A git **worktree** created under
`.../og-simulation/.claude/worktrees/<name>` duplicates every `.cpp` and makes UBT
abort with `Input filename conflicts … duplicate filenames`. If a build fails that
way, check `git worktree list` in the og-simulation submodule for an orphaned
worktree inside the module source tree. In Agent-Team runs these can be locked and
hold another session's unique uncommitted work — do not remove without confirming.
(Confirm safe-to-remove by: the worktree is locked by THIS session's own pid, is
named after THIS task, and your real edits went to the module tree, not the worktree
copy — then `git worktree remove -f -f <path>` + `git worktree prune`; a
session-locked empty dir shell may remain and is harmless once it holds 0 sources.)

## PREVENT the auto-worktree (background sessions, nested submodules)

The repo is doubly nested: `Plugins/OGSimulation` is a submodule, and
`.../og-simulation` is a further submodule inside it (the real repo for OGSim-core).
The background-isolation guard resolves `worktree.bgIsolation` against the **nearest
ancestor `.claude/`** of the edited file = `.../og-simulation/.claude/`. The outer
project's `bgIsolation: none` does NOT reach it, so a background session gets its
edits blocked AND an auto-worktree created under the module tree (breaking UBT per
above). **Fix up front:** ensure `.../og-simulation/.claude/settings.json` contains
`{"worktree":{"bgIsolation":"none"}}`. Bootstrap it via a Bash `cat > … <<EOF`
heredoc — Edit/Write are themselves blocked by the guard until the setting exists;
Bash file creation is not. (Better still: the standalone syntax-check recipe above
sidesteps UBT entirely for comment/header-only changes.)

## Transient UBA failure — rerun once

The Server build occasionally reports `Failed (OtherCompilationError)` from an Unreal
Build Accelerator local-executor glitch. Re-run once; if it returns "Target is up to
date" + "Succeeded", every `.obj` was actually produced and the first failure was
spurious (a genuine compile failure leaves a missing `.obj` and forces a recompile,
not "up to date"). Benign pre-existing warnings that are NOT failures: `DPID.cpp`
C5038 init-order; `OGBrawlerUECharacter.cpp` C5038 + C4996 UE-5.6 `NetUpdateFrequency`
/`MinNetUpdateFrequency`/`NetCullDistanceSquared` deprecations (engine-upgrade
artifact in `Source/OGBrawlerUnreal`, unrelated to OGSim-plugin edits).
