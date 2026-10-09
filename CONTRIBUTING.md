# Contributing

## The one rule that can't be broken

**Never commit game content.** No ROMs, no files extracted from a ROM, no save
files, no built game binaries, and nothing copied from leaked source code or
from ports that bundle the game's assets. The `.gitignore` blocks the common
file types, but check the list of changed files before every commit. If a
`.nds`, `.sav`, `.narc` or an image from the game shows up there, stop.

## How changes get in

`main` always holds a version that has been tested in Azahar.

1. **Branch.** Every change goes on its own branch, named by what it does:
   `docs/hardware-layer`, `port/virtual-vram`, `tools/data-builder`.
2. **Pull request.** Open a pull request into `main` describing what changed
   and how to test it.
3. **CI.** GitHub Actions builds the 3DS code and runs the Python tools on
   every push. A pull request with a red build doesn't get merged.
4. **Test.** Download the `.3dsx` from the pull request's Actions run (the
   **Artifacts** section at the bottom of the run) and try it in Azahar. Changes
   that affect performance or stereo also get checked on a New 3DS when
   possible.
5. **Merge.** The repository owner merges after testing.

## Changing the decompilation

The pret/pokeplatinum code is never committed directly. To change it:

```sh
python tools/bootstrap.py --clean       # fresh copy with existing patches
cd build/upstream
# ... edit, then commit inside build/upstream ...
git format-patch -1 -o ../../patches/pokeplatinum/
```

Then rename the new file to the next number (`0003-…patch`) and commit it in
this repository. Keep each patch focused on one change.

## Code style

- **C:** match the surrounding code. In `3ds_port/`, four-space indents and
  comments that explain *why*, especially anything 3DS- or DS-specific.
- **Python:** Python 3.10+, standard library only unless there's a good reason,
  `python -m py_compile` clean. Tools must never modify or upload a ROM.

## Commit messages

Short summary line in the imperative ("Add virtual VRAM banks"), blank line,
then what and why if it isn't obvious.
