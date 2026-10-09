# AI disclosure

This project is developed with help from AI coding assistants, including Claude
(Anthropic) and ChatGPT (OpenAI).

## What the AI does

- Writes and reviews C code for the 3DS backend and Python tooling.
- Researches and documents the decompilation and DS / 3DS hardware.
- Diagnoses build failures, proposes fixes, and maintains shared development handoffs.

## How AI-written changes are handled

- They arrive as pull requests, never as direct pushes to `main`.
- The assistant or assistants involved are identified truthfully in PR
  descriptions or commit metadata. Existing `Co-Authored-By: Claude` trailers
  remain part of the history. Do not attribute work to an assistant that did
  not contribute.
- CI verifies only what it actually runs. A human checks game-facing changes
  in Azahar and, for performance-sensitive work, on a New 3DS when possible.
  A passing compile alone does not count as an emulator or hardware test.
- The project owner decides what gets merged and is responsible for the
  repository's contents.
- `AGENTS.md` and `AI_HANDOFF.md` describe the cross-assistant workflow.

## What the AI is not used for

- Generating, recreating or "cleaning up" game assets. All game content comes
  from the player's own cartridge dump at build time.
- Reproducing code from leaked sources or from ports that bundle game assets.
