<!-- SPDX-License-Identifier: MPL-2.0 -->
# og-simulation

OG Simulation is a game-agnostic, deterministic C++20 simulation framework. It provides a data-driven physics and gameplay simulation layer designed for multiplayer games where server-authoritative deterministic simulation with client-side prediction is required.

## Position in the og-framework graph

og-simulation is the **pure C++ simulation core**. It has no build-system root and no Unreal Engine dependencies.

```
og-simulation  (this repo — pure source)
    ↓ consumed by
og-simulation-ue     — UE plugin shell
og-simulation-tests  — Catch2 test source
og-brawler           — brawler core (links og_simulation)
    ↓ assembled by
og-tests-cmake-runner — CMake build + test runner
og-brawler-unreal     — UE game project
```

## Related repos

| Repo | Role |
|---|---|
| [og-simulation-ue](https://github.com/og-framework/og-simulation-ue) | Wraps this repo as a UE plugin |
| [og-simulation-tests](https://github.com/og-framework/og-simulation-tests) | Catch2 tests for this repo |
| [og-brawler](https://github.com/og-framework/og-brawler) | Brawler core that depends on this repo |
| [og-tests-cmake-runner](https://github.com/og-framework/og-tests-cmake-runner) | CMake harness that builds + runs tests |
| [og-brawler-unreal](https://github.com/og-framework/og-brawler-unreal) | UE game project; consumes this via og-simulation-ue |

## Quickstart

og-simulation is a **source distribution** — it is not directly buildable on its own. Consume it via a parent build:

**CMake (tests / standalone):** clone [og-tests-cmake-runner](https://github.com/og-framework/og-tests-cmake-runner) with `--recurse-submodules`. It assembles the full CMake tree.

```bash
git clone --recurse-submodules https://github.com/og-framework/og-tests-cmake-runner
cd og-tests-cmake-runner
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

**Unreal Engine:** use the [og-simulation-ue](https://github.com/og-framework/og-simulation-ue) plugin shell, consumed via [og-brawler-unreal](https://github.com/og-framework/og-brawler-unreal).

In your own CMake consumer, add the simulation module directly:

```cmake
add_subdirectory(extern/og-simulation/OGSimulation)
target_link_libraries(MyTarget PRIVATE og_simulation)
```

## Canonical workflow

See [`og-brawler-unreal/docs/cross-repo-dev-loop.md`](https://github.com/og-framework/og-brawler-unreal/blob/main/docs/cross-repo-dev-loop.md) for the multi-repo development workflow (submodule push order, pin management via og-tools).

## License and contributing

Mozilla Public License 2.0 — see [LICENSE](LICENSE).

Contributions welcome. By submitting a pull request you agree your contribution is licensed under MPL 2.0 (inbound = outbound). See [CONTRIBUTING.md](https://github.com/og-framework/og-brawler-unreal/blob/main/CONTRIBUTING.md) for the decision tree on where to make your change.
