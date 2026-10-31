#!/usr/bin/env bash
# Build the 3DS program on Windows, in devkitPro's MSYS2 terminal, and make
# your data pack from your own ROM:
#
#   bash build_windows_msys2.sh 'C:\path\to\your\platinum.nds'
#
# Installs what the build needs besides devkitPro (git, python, gcc, patch),
# clones the port into ~/pokeplatinum-3ds (its own clone, with Linux line
# endings) or updates it, builds pokeplatinum.3dsx and pokeplatinum.cia (for
# the HOME Menu, with your ROM's banner: it opens the .3dsx), makes the data
# pack, and puts the SD card files in %USERPROFILE%\pokeplatinum-sd (and in
# Azahar's SD folder, if there is one). Nothing is uploaded. The program and
# the pack contain game code and data: keep them to yourself.
set -euo pipefail

ROM="${1:-}"
REPO_DIR="$HOME/pokeplatinum-3ds"
BRANCH="${BRANCH:-main}"
step() { printf '\n==> %s\n' "$*"; }

if [ -z "$ROM" ]; then
    echo "Usage: bash $0 'C:\\path\\to\\your\\platinum.nds'"
    exit 1
fi
ROM="$(cygpath -u "$ROM")"
if [ ! -f "$ROM" ]; then
    echo "Can't find the ROM at: $ROM"
    exit 1
fi

WIN_HOME="$(cygpath -u "$USERPROFILE")"
SD_DIR="$WIN_HOME/pokeplatinum-sd"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"

step "Installing git, python, a C compiler, patch and iconv"
pacman -S --needed --noconfirm git python gcc make patch libiconv-devel

step "Making sure the 3DS tools are installed"
pacman -S --needed --noconfirm 3ds-dev

if [ -d "$REPO_DIR/.git" ]; then
    step "Updating $REPO_DIR"
    git -C "$REPO_DIR" fetch origin
    git -C "$REPO_DIR" checkout "$BRANCH"
    git -C "$REPO_DIR" reset --hard "origin/$BRANCH"
else
    step "Cloning the port into $REPO_DIR"
    git -c core.autocrlf=false clone -b "$BRANCH" https://github.com/GabeSud0/pokeplatinum-3ds.git "$REPO_DIR"
    git -C "$REPO_DIR" config core.autocrlf false
fi
cd "$REPO_DIR"

# A copy of this script kept from before would build the new code the old
# way (an older one made the data pack without the follower sprites): the
# one just fetched takes over, once.
if [ -z "${BUILD_WINDOWS_FRESH:-}" ] && ! cmp -s "$0" tools/build_windows_msys2.sh; then
    step "This script changed: running the new one (kept as ~/build_windows_msys2.sh)"
    cp tools/build_windows_msys2.sh "$HOME/build_windows_msys2.sh"
    exec env BUILD_WINDOWS_FRESH=1 BRANCH="$BRANCH" bash "$HOME/build_windows_msys2.sh" "$ROM"
fi

step "Fetching the decompilation and the SDKs (a few minutes the first time)"
python3 tools/bootstrap.py --clean

step "Building pokeplatinum.3dsx (this takes a while: ~1,000 game files)"
# (the output kept: with several files compiling at once, an error scrolls
# away behind the warnings of the others; it's shown again at the end)
if ! make -j"$(nproc)" game 2>&1 | tee build.log; then
    echo
    echo "==> The build failed. The error lines:"
    grep -nE "error:|Error [0-9]|undefined reference|No such file" build.log | grep -v "Wmaybe-uninitialized" | head -30
    echo "(the whole output is in $(cygpath -w "$PWD/build.log"))"
    exit 1
fi

# The HOME Menu version (.cia). The tools are built once, in a folder kept
# between builds. If this part fails, the .3dsx above is still fine.
CIA_TOOLS="$HOME/pokeplatinum-cia-tools"
CIA=""
step "Building pokeplatinum.cia (the HOME Menu version: it opens sd:/3ds/pokeplatinum.3dsx)"
# Its banner: the animated 3D one, made from your ROM (tools/banner3d), or
# the flat picture if that can't be made.
BANNER3D="$PWD/build/home-menu-art/banner3d.bnr"
make_banner3d() {
    rm -f "$BANNER3D"
    python3 tools/banner3d/make_banner.py --rom "$ROM" --bannertool "$CIA_TOOLS/bin/bannertool" \
        --chime 3ds_port/game/cia/chime.wav -o "$BANNER3D" ||
        { rm -f "$BANNER3D"; echo "(No 3D banner: see above. The .cia gets the flat one.)"; }
}
if bash tools/build_cia_tools.sh "$CIA_TOOLS" &&
   python3 tools/home_menu_art.py "$ROM" build/home-menu-art &&
   { make_banner3d; true; } &&
   make -C 3ds_port/game TARGET=3ds HOME_ART="$PWD/build/home-menu-art" CIA_TOOLS="$CIA_TOOLS" \
       HOME_BANNER="$([ -f "$BANNER3D" ] && echo "$BANNER3D")" program cia; then
    CIA=3ds_port/game/build/3ds/pokeplatinum.cia
else
    echo "(Couldn't build the .cia: see the lines above. The .3dsx works as before.)"
fi

step "Making your data pack from $ROM"
python3 builder/extract_rom.py "$ROM" -o data_pack
# The follower Pokémon's sprites (HeartGold and SoulSilver's, downloaded
# once, checked; never in the repository): without them only Platinum's
# own Pokémon follow.
FOLLOWERS=()
if python3 builder/followers.py -o build/followers.bin; then
    FOLLOWERS=(--followers build/followers.bin)
fi
python3 builder/pack.py data_pack -o platinum3ds.pak "${FOLLOWERS[@]}"

step "Putting the SD card files in $SD_DIR"
mkdir -p "$SD_DIR/3ds/pokeplatinum"
cp 3ds_port/game/build/3ds/pokeplatinum.3dsx "$SD_DIR/3ds/pokeplatinum.3dsx"
rm -f "$SD_DIR/cia/pokeplatinum.cia"
if [ -n "$CIA" ]; then
    mkdir -p "$SD_DIR/cia"
    cp "$CIA" "$SD_DIR/cia/pokeplatinum.cia"
fi
cp platinum3ds.pak data_pack/manifest.json "$SD_DIR/3ds/pokeplatinum/"

AZ="$(cygpath -u "$APPDATA")/Azahar/sdmc"
if [ -d "$AZ" ]; then
    step "Found Azahar's SD folder: copying the files there too"
    mkdir -p "$AZ/3ds/pokeplatinum"
    cp "$SD_DIR/3ds/pokeplatinum.3dsx" "$AZ/3ds/"
    cp "$SD_DIR/3ds/pokeplatinum/platinum3ds.pak" "$SD_DIR/3ds/pokeplatinum/manifest.json" \
        "$AZ/3ds/pokeplatinum/"
else
    echo "(Azahar's SD folder wasn't found at $(cygpath -w "$AZ"): copy the 3ds folder"
    echo " from $(cygpath -w "$SD_DIR") into Azahar's sdmc folder yourself.)"
fi

WIN_SD="$(cygpath -w "$SD_DIR")"
cat <<EOF

Done. In Windows, the files are in:

  $WIN_SD\\3ds\\pokeplatinum.3dsx                       (the program, for the Homebrew Launcher)
  $WIN_SD\\cia\\pokeplatinum.cia                        (the same, to install on the HOME Menu with FBI)
  $WIN_SD\\3ds\\pokeplatinum\\platinum3ds.pak            (your data)
  $WIN_SD\\3ds\\pokeplatinum\\manifest.json

Azahar:  File > Load File... > $WIN_SD\\3ds\\pokeplatinum.3dsx

After running it, send the log:
  %APPDATA%\\Azahar\\sdmc\\3ds\\pokeplatinum\\log.txt
EOF
