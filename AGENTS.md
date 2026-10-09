# Shared AI contributor instructions

Applies to Claude, ChatGPT, Codex, and other coding assistants. Read this file at the start of each session, then `AI_HANDOFF.md`, `CONTRIBUTING.md`, `docs/ROADMAP.md`, and the files relevant to the current task. `CLAUDE.md` provides an entry point for Claude Code, not different project rules.

## Project rules

- This is a native New 3DS-first port of Pokémon Platinum based on the pinned pret/pokeplatinum decompilation. The intended 3DS backend should later be reusable for HeartGold/SoulSilver.
- **Never commit copyrighted game data:** ROMs, extracted assets, saves, built game binaries, or leaked proprietary code. The player's legally dumped game content must remain local.
- Do not vendor downloaded decompilation/SDK sources. `tools/bootstrap.py` fetches them under `build/`, pinned by `upstream.lock` and the decomp's subproject revisions. Persist modifications as patches under `patches/<project>/`, preferably guarded by `SDK_PORT_3DS` where appropriate.
- Keep the existing C platform/hardware-compatibility architecture unless a reviewed change documents an alternative.
- Follow `CONTRIBUTING.md`: descriptive feature branch, PR to `main`, CI, human testing/review, owner-controlled merge.
- Avoid touching another agent's in-progress files or branches without an explicit handoff. Never force-push, rewrite another agent's commits, or push directly to `main`.

## Starting work (every fresh session)

1. Read the *current* remote state: `main` HEAD, recent commits, open PRs, active branches, issues, and CI. A handoff note can be stale.
2. Read `AI_HANDOFF.md` and cross-check every statement against GitHub history. Uncommitted code in another assistant's workspace is **not** available just because it was described in chat.
3. Identify one bounded task, confirm whether anyone already owns it, and use a separate branch. Prefer an existing tracked issue or clearly describe the intended scope in a PR.
4. For SDK changes, read the patches, `3ds_port/nitro/README.md`, and the relevant subsystem of `docs/HARDWARE_LAYER.md` before implementation.

## Testing and publishing

- Validate what you can. Existing NitroSDK test commands (from `3ds_port/nitro/README.md`): `python tools/bootstrap.py`; `make -C 3ds_port/nitro` (host tests); `make -C 3ds_port/nitro TARGET=3ds` (requires devkitARM); `python tools/check_sdk_coverage.py --strict` (after bootstrap/build).
- New NitroSystem code needs its own documented build and tests; **do not** imply NitroSDK coverage proves NitroSystem works.
- Record **exactly** which checks were actually run, skipped, or failed. CI green is not proof of a playable game, Azahar validation, or real New 3DS testing.
- Inspect changed-file names and diffs for forbidden assets and accidental generated files before committing.
- Open/update a PR against `main`; never merge on an AI assistant's own authority.
- Update `AI_HANDOFF.md` in the same branch/PR with the latest **verified** state, branch/PR link, blockers, actual tests, and the next specific action. Keep it brief; lengthy records belong in issues and PR descriptions.
- State AI involvement accurately in the PR or commit per `AI_DISCLOSURE.md`.

## Multi-agent conflict prevention

- GitHub is the source of truth; chats are disposable.
- `AI_HANDOFF.md` is a shared snapshot, **not** a lock, task queue, or replacement for GitHub issues/PRs.
- One active owner per workstream whenever possible. Work on separate branches and nonoverlapping files.
- Before continuing another agent's branch, verify the branch's latest commit and read its PR, tests, and unresolved TODOs.
- If an agent reaches a context/session limit, preserve work as a commit on a feature/WIP branch (clearly labeled incomplete), then document the exact SHA and next action. Do not claim unpushed work is preserved.
