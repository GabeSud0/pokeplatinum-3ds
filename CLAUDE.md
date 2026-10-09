# Claude Code entry point

Read `AGENTS.md`, `AI_HANDOFF.md`, and `CONTRIBUTING.md` before doing any implementation. Also consult `docs/ROADMAP.md` and `docs/HARDWARE_LAYER.md` for architecture.

This repository is shared across Claude and ChatGPT sessions. Never rely on a previous AI chat to carry task state: compare `AI_HANDOFF.md` against current GitHub commits, open PRs, and branches at every session start. In particular, confirm whether unfinished NitroSystem work has been **pushed** before treating it as available.

Work on a focused feature branch, use pull requests instead of pushing directly to `main`, avoid overwriting the other assistant's branch, and report only tests that actually ran. Before ending or approaching a session limit, commit/push recoverable work (mark as WIP if incomplete), then update `AI_HANDOFF.md` with branch, commit/PR, actual status, blockers, and exact next step.

Follow `AGENTS.md` for the shared details and never commit any ROM, extracted game assets, or built game binaries.
