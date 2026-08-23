---
name: feedback-fork-identity-and-duplicate-dispatch
description: ListAgents' "main session" string doesn't disambiguate a fork from its parent; duplicate-dispatch collisions on the same review task happen and are resolvable by cooperative de-duplication, not by asserting authority.
metadata:
  type: feedback
---

Two lessons from a task-14 (`SimulationInputResolution.h`) safety review where two full
reviewer sessions ended up independently dispatched on the same task concurrently, each
writing to the same `impl/review_wave5_14.md` / `Backlog.md` status line.

**1. `ListAgents`' "This process's main session is X" string is NOT reliable evidence of
identity.** Both colliding sessions saw the identical string and each concluded *they* were
the main session X and the other was a fork of it. It does not disambiguate a fork from its
parent in this environment.

**What DOES disambiguate, from direct behavioral evidence:**
- A true fork gets a `<fork-boilerplate>`-wrapped directive ("Execute ONE directive, then
  stop") and is explicitly told "Do NOT spawn subagents with the Agent tool." If a session
  has spawned its own subagent (Agent tool call, `subagent_type: "fork"` or otherwise) and
  received messages back from it across multiple turns, it is not that fork — a fork can't
  spawn itself before it exists or spawn siblings.
- Attempting to launch a fork *from inside* a forked worker throws `"Fork is not available
  inside a forked worker"` — a session that hits this error on its own attempt is itself a
  fork, confirmed structurally, not just by the ambiguous main-session string.

**How to apply:** if a peer session disputes your identity in a multi-agent review/orchestration
setup, don't try to win it by re-citing `ListAgents` — that string is symmetric and won't
settle anything. Cite spawn/nesting behavior instead. And don't let the dispute block the
actual work: verify claims from source on both sides rather than deferring to "whoever is
the real main session," and the correct deliverable converges regardless of who technically
owns the session.

**2. Duplicate-dispatch collisions on the same review task are real and are best handled by
cooperative de-duplication, not a rewrite war.** When two sessions independently reach the
same verdict with overlapping-but-not-identical findings: pick one file as the survivor,
have each side spot-check (not blindly trust) the other's specific claims before accepting
them, fold in genuinely additive minor findings via a small addendum rather than a full
rewrite, and leave an explicit provenance note in the deliverable disclosing the collision
and that it was independently converged, not merged by authority. Surface the duplicate
dispatch itself to the user/lead as a possible orchestration bug — it's not something either
session can fix from inside.
