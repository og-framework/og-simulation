---
name: initiative-docs-location
description: Where the ogsim-system-api initiative's Backlog / current_state / impl notes / audit reports actually live (workspace, not the repo)
metadata:
  type: reference
---

# ogsim-system-api initiative docs live in the WORKSPACE, not the repo

For the `ogsim-system-api` initiative, `Backlog.md`, `current_state.md`, and all
`impl/*.md` notes/reviews/audits live under the **agent workspace**:

`C:\Users\olle\Documents\Notes\AgentWorkspace\Initiatives\ogsim-system-api\`
- `Backlog.md`, `current_state.md`
- `impl/` — impl notes, review notes, and the `comment_audit_ogsim.md` audit report.

They are **not** under the repo's `og-simulation/impl/` (that dir doesn't exist).
The code being edited is in the repo at
`C:\dev\og-brawler-unreal\Plugins\OGSimulation\Source\OGSimulation\og-simulation\OGSimulation\`.

The OGSim-core comment-cleanup effort is phased: Phase 1 (Option A) did the
mechanical `[Task N]` / pragma / banner sweeps; Phase 2 does domain-briefed per-file
de-scaffolding of the netcode cluster (audit §4 candidate #8) and Tier 2–4 items.
Rubric: **strip tag, keep sentence; de-scaffold, don't delete.** Invariants /
OG_CHECK rationale / wire-format spec are preserved verbatim (audit §5 preserve
list is authoritative for which task/review-tagged blocks must survive).
