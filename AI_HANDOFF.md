# Shared Claude ↔ ChatGPT handoff

**Source of truth:** GitHub commits, branches, PRs, CI, and issues. This document is a concise snapshot, not a substitute for checking the live repository. Each agent should update it in the PR that changes the workstream.

## Snapshot (2026-10-08, America/Vancouver)

- **Repository:** `GabeSud0/pokeplatinum-3ds` (private)
- **Stable branch:** `main`
- **Last verified main HEAD at setup:** `7f933b03b75d9bffd35dc1cb32ed97d1b19fb978` (merged PR #3: NitroSDK)
- **Last verified CI:** success for `main`, GitHub Actions run `37881317670` at that commit.
- **Current workstream reported by owner:** **NitroSystem 3DS port** — Claude was working on this when its session limit was reached.
- **CRITICAL: NitroSystem work is not yet visible in GitHub history as of this snapshot.** No NitroSystem feature branch, PR, committed `3ds_port/nitrosystem`, or `patches/NitroSystem` directory was found. It may exist only in Claude's unpushed workspace. **Do not represent it as complete or available to another agent.**
- **This coordination PR:** branch `docs/ai-handoff`. It changes only AI collaboration documentation.

## What GitHub currently confirms

- `3ds_port/hello3d`: stereo test app; project roadmap reports an Azahar test. No real New 3DS test confirmed here.
- `tools/bootstrap.py`: fetches pinned pokeplatinum, NitroSDK and NitroSystem into `build/` and applies any committed patches.
- `3ds_port/nitro`: builds NitroSDK with GCC for PC and 3DS. Merged PR #3 reports coverage of the 281 NitroSDK functions used by the game and 621 host checks.
- `3ds_port/nitro/README.md` documents which NitroSDK hardware behaviours are real and which are still hooks/stubs (thread switching, ARM7 services, drawing, reset, cartridge filesystem).
- `docs/HARDWARE_LAYER.md` explains the hardware-compatibility approach and NitroSystem's position between the game and NitroSDK.
- The project is **not yet playable**; NitroSystem build integration, game compilation and rendering remain work to verify or implement.
- The last confirmed GitHub CI is green; this does **not** validate any unpushed NitroSystem edits.

## Priority: safely resume Claude's NitroSystem work

1. **Check GitHub again first:** look for a new NitroSystem branch, commits, PR, and CI since this snapshot.
2. **Preserve any unpushed Claude work if available.** If the previous Claude workspace still exists, commit and push a clearly named `port/nitrosystem` (or WIP) branch before replacing, duplicating or restarting it. If it is inaccessible, state explicitly that its code cannot be recovered from GitHub.
3. Review the actual changes and the pinned `build/NitroSystem` upstream, then establish exactly which NitroSystem libraries/modules were implemented, which fail to compile, and which need 3DS-specific replacements.
4. Continue the existing strategy: compile original NitroSystem C where possible; keep upstream changes reproducible as `patches/NitroSystem/*.patch`; add port-side implementations and tests in a reviewed, scoped feature branch. Avoid blind stubs that hide behaviour or violate existing layouts.
5. Add a documented NitroSystem host build/test and 3DS cross-build milestone, plus CI coverage as appropriate. Only mark components completed after verifying actual build output.
6. Open/update a PR and fill in the handoff fields below with evidence.

## Active task record (edit this section on each workstream PR)

- **Task:** NitroSystem 3DS compatibility/build — status *reported in progress, unpushed work not recoverable here*.
- **Agent:** Claude (last reported by repository owner); ChatGPT (handoff documentation only so far).
- **Working code branch / PR:** not found at handoff inspection; check again.
- **Last relevant implemented commit:** `7f933b0` (NitroSDK, **not** NitroSystem).
- **Confirmed builds/tests:** last `main` CI green; NitroSystem-specific build/test **not verified**.
- **Blockers:** previous Claude session may contain code that is not yet pushed; NitroSystem design/build status unknown.
- **Next concrete action:** locate/preserve Claude's NitroSystem changes, or inspect the pinned NitroSystem source and make a small, testable first NitroSystem compilation PR if none can be recovered.

## Agent completion checklist

- [ ] Fetch latest `main`, active branches, PRs, CI and this handoff.
- [ ] Claim a bounded task; confirm nonoverlap with another agent.
- [ ] Push code to a feature branch; open or update PR (not direct to `main`).
- [ ] List precise build/test commands and their observed results, including untested areas.
- [ ] Record feature branch, PR URL, commit SHA, outstanding blockers and next action here.
- [ ] Do not merge without repository owner review and required testing.

## Relevant references

- `README.md`; `CONTRIBUTING.md`; `AI_DISCLOSURE.md`
- `docs/ROADMAP.md`; `docs/HARDWARE_LAYER.md`
- `3ds_port/nitro/README.md`
- PR #3: https://github.com/GabeSud0/pokeplatinum-3ds/pull/3
- Green main CI: https://github.com/GabeSud0/pokeplatinum-3ds/actions/runs/37881317670
