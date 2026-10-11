#!/usr/bin/env bash
# Run the game on the PC runner, in WSL (Ubuntu) on Windows, with your data
# pack: the quickest way to find where the game stops, with function names.
#
#   bash run_pc_wsl.sh 'C:\path\to\platinum3ds.pak' 'C:\path\to\manifest.json' [options]
#
# Options after the two paths go to the runner, e.g. --frames 20000 to stop
# by itself after that many frames.
#
# Installs what the 32-bit PC build needs (Ubuntu packages; no devkitPro),
# clones the port into ~/pokeplatinum-3ds in Ubuntu (or updates it), builds
# the PC runner (3ds_port/game/build/host32/pokeplatinum-pc) and runs it.
# The PC runner has no screen: it prints the log. If the game gets stuck it
# prints where, as function names and file:line, and stops.
#
# The log also goes to pokeplatinum-pc-log.txt in your Windows user folder.
# The save is ~/pokeplatinum-pc.sav in Ubuntu, apart from the 3DS one.
# BRANCH=<name> builds another branch (default: main). Nothing is uploaded.
set -euo pipefail

PAK="${1:-}"
MANIFEST="${2:-}"
REPO_DIR="$HOME/pokeplatinum-3ds"
BRANCH="${BRANCH:-main}"
step() { printf '\n==> %s\n' "$*"; }

if [ -z "$PAK" ] || [ -z "$MANIFEST" ]; then
    echo "Usage: bash $0 'C:\\path\\to\\platinum3ds.pak' 'C:\\path\\to\\manifest.json'"
    exit 1
fi
to_linux() {
    case "$1" in
        [A-Za-z]:\\*|[A-Za-z]:/*) wslpath -u "$1" ;;
        *) printf '%s\n' "$1" ;;
    esac
}
PAK="$(to_linux "$PAK")"
MANIFEST="$(to_linux "$MANIFEST")"
for f in "$PAK" "$MANIFEST"; do
    [ -f "$f" ] || { echo "Can't find $f"; exit 1; }
done
WIN_HOME="$(wslpath -u "$(cmd.exe /c 'echo %USERPROFILE%' 2>/dev/null | tr -d '\r')")"
LOG="$WIN_HOME/pokeplatinum-pc-log.txt"

step "Installing git, python, make, gcc with 32-bit support, patch"
sudo apt-get update -qq
sudo apt-get install -y -qq git python3 make gcc gcc-multilib patch binutils >/dev/null

if [ -d "$REPO_DIR/.git" ]; then
    step "Updating $REPO_DIR ($BRANCH)"
    git -C "$REPO_DIR" fetch origin
    git -C "$REPO_DIR" checkout "$BRANCH"
    git -C "$REPO_DIR" reset --hard "origin/$BRANCH"
else
    step "Cloning the port into $REPO_DIR ($BRANCH)"
    git clone -b "$BRANCH" https://github.com/GabeSud0/pokeplatinum-3ds.git "$REPO_DIR"
fi
cd "$REPO_DIR"

step "Fetching the decompilation and the SDKs"
python3 tools/bootstrap.py --clean

step "Building the PC runner (32-bit, as on the 3DS; ~1,000 game files)"
make -j"$(nproc)" -C 3ds_port/game TARGET=host32 program

step "Running the game (log: $(wslpath -w "$LOG"))"
status=0
3ds_port/game/build/host32/pokeplatinum-pc "$PAK" "$MANIFEST" --save "$HOME/pokeplatinum-pc.sav" \
    "${@:3}" 2>&1 | (trap '' INT; tee "$LOG") || status=$?
# (tee ignores Ctrl+C, so the log keeps everything up to the runner's last
# line: Ctrl+C only stops the runner, which says at which frame.)
echo
echo "Done. Send the log: $(wslpath -w "$LOG")"
exit $status
