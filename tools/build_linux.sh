#!/usr/bin/env bash
# Build the 3DS program on Linux and make your data pack from your own ROM,
# the Linux counterpart of build_windows_msys2.sh:
#
#   bash build_linux.sh /path/to/your/platinum.nds
#   PR=166 bash build_linux.sh /path/to/your/platinum.nds   (a pull request)
#
# On Bazzite, SteamOS-like and other immutable systems (Fedora Atomic:
# Silverblue, Kinoite, Bazzite, Aurora, Bluefin...), where packages can't be
# installed the usual way, it works inside a distrobox ("pokeplatinum-3ds")
# that it creates the first time from devkitPro's own image on Docker Hub (the
# one the project's CI builds with), so the 3DS tools come ready-made; gcc and
# Python go in there too and your system isn't changed. On Ubuntu or Debian it runs
# directly (it asks for sudo to install packages). Your home folder is shared
# with the distrobox, so the files it makes are in your own folders.
#
# Clones the port into ~/pokeplatinum-3ds (or updates it), builds
# pokeplatinum.3dsx and pokeplatinum.cia (for the HOME Menu, with its
# animated 3D banner made from your ROM: it opens the .3dsx), makes the data pack,
# and puts the SD card files in ~/pokeplatinum-sd (and in Azahar's SD folder,
# if there is one). Nothing is uploaded. The program and the pack contain
# game code and data: keep them to yourself.
set -euo pipefail
trap 'echo; echo "==> Stopped: the command on line $LINENO of build_linux.sh failed (see the lines above)."' ERR

ROM="${1:-}"
REPO_DIR="$HOME/pokeplatinum-3ds"
BRANCH="${BRANCH:-main}"
PR="${PR:-}"              # PR=166: build that pull request instead of main
BOX="${BOX:-pokeplatinum-3ds}"
BOX_IMAGE="docker.io/devkitpro/devkitarm:latest"
step() { printf '\n==> %s\n' "$*"; }

if [ -z "$ROM" ]; then
    echo "Usage: bash $0 /path/to/your/platinum.nds"
    exit 1
fi
if [ ! -f "$ROM" ]; then
    echo "Can't find the ROM at: $ROM"
    exit 1
fi
ROM="$(realpath "$ROM")"

# ---- not Debian/Ubuntu: run this script again inside the distrobox --------
if ! command -v apt-get >/dev/null; then
    if ! command -v distrobox >/dev/null; then
        echo "This system has neither apt (Debian/Ubuntu) nor distrobox."
        echo "Install distrobox (https://distrobox.it), or build on Debian/Ubuntu."
        exit 1
    fi
    # (compared with the real path of home: on Bazzite and Fedora Atomic, /home is
    # a link to /var/home)
    HOME_REAL="$(realpath "$HOME")"
    case "$ROM" in
        "$HOME"/*|"$HOME_REAL"/*|/var/home/*|/home/*|/run/media/*|/var/run/media/*|/media/*|/mnt/*|/var/mnt/*) ;;
        *) echo "Put the ROM in your home folder (the distrobox only sees your home and"
           echo "external drives), then run this again."; exit 1 ;;
    esac
    SELF="$(realpath "$0")"
    case "$SELF" in
        "$HOME"/*|"$HOME_REAL"/*) ;;
        *) cp "$SELF" "$HOME/build_linux.sh"; SELF="$HOME/build_linux.sh" ;;
    esac
    BOXES="$(distrobox list 2>/dev/null | awk -F'|' '{gsub(/ /, "", $2); print $2}')"
    if ! grep -qx "$BOX" <<<"$BOXES"; then
        step "Creating the distrobox \"$BOX\" ($BOX_IMAGE): a few minutes, once"
        distrobox create --name "$BOX" --image "$BOX_IMAGE" --yes
    fi
    step "Building inside the distrobox \"$BOX\""
    exec distrobox enter "$BOX" -- env BRANCH="$BRANCH" PR="$PR" bash "$SELF" "$ROM"
fi

# ---- Debian/Ubuntu (directly, or inside the distrobox) ---------------------
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
SD_DIR="$HOME/pokeplatinum-sd"

step "Installing git, Python, a C compiler and patch"
sudo apt-get update -q
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
    git python3 gcc g++ make patch libc6-dev wget ca-certificates gnupg xz-utils

# The 3DS tools: already there in devkitPro's image (the distrobox), otherwise
# installed from devkitPro's servers.
if [ -x "$DEVKITARM/bin/arm-none-eabi-gcc" ] && [ -d "$DEVKITPRO/libctru" ]; then
    step "The 3DS tools (devkitARM, libctru) are already installed"
else
    if ! command -v dkp-pacman >/dev/null; then
        step "Installing devkitPro's package manager (https://devkitpro.org/wiki/devkitPro_pacman)"
        [ -e /etc/mtab ] || sudo ln -s /proc/self/mounts /etc/mtab   # (containers and WSL lack it)
        if ! wget -nv -O /tmp/install-devkitpro-pacman https://apt.devkitpro.org/install-devkitpro-pacman; then
            echo "devkitPro's server refused the download (a 403 means it blocks this network:"
            echo "try without a VPN or on another network)."
            exit 1
        fi
        chmod +x /tmp/install-devkitpro-pacman
        sudo DEBIAN_FRONTEND=noninteractive /tmp/install-devkitpro-pacman   # (as devkitPro's own Docker images do)
    fi
    step "Installing the 3DS tools"
    sudo dkp-pacman -Syu --noconfirm
    sudo dkp-pacman -S --needed --noconfirm 3ds-dev
fi

if [ ! -d "$REPO_DIR/.git" ]; then
    step "Cloning the port into $REPO_DIR"
    git clone https://github.com/GabeSud0/pokeplatinum-3ds.git "$REPO_DIR"
fi
if [ -n "$PR" ]; then
    step "Getting pull request #$PR"
    git -C "$REPO_DIR" fetch origin "pull/$PR/head"
    git -C "$REPO_DIR" checkout -q --detach FETCH_HEAD
else
    step "Updating $REPO_DIR ($BRANCH)"
    git -C "$REPO_DIR" fetch origin
    git -C "$REPO_DIR" checkout -q "$BRANCH"
    git -C "$REPO_DIR" reset -q --hard "origin/$BRANCH"
fi
git -C "$REPO_DIR" log -1 --format='    at %h: %s'
cd "$REPO_DIR"

# A copy of this script kept from before (~/build_linux.sh) would build the
# new code the old way (an older one made the data pack without the follower
# sprites): the one just fetched takes over, once.
if [ -z "${BUILD_LINUX_FRESH:-}" ] && ! cmp -s "$0" tools/build_linux.sh; then
    step "This script changed: running the new one (kept as ~/build_linux.sh)"
    cp tools/build_linux.sh "$HOME/build_linux.sh"
    exec env BUILD_LINUX_FRESH=1 BRANCH="$BRANCH" PR="$PR" bash "$HOME/build_linux.sh" "$ROM"
fi

step "Fetching the decompilation and the SDKs (a few minutes the first time)"
python3 tools/bootstrap.py --clean

step "Building pokeplatinum.3dsx (this takes a while: ~1,000 game files)"
if ! make -j"$(nproc)" game 2>&1 | tee build.log; then
    echo
    echo "==> The build failed. The error lines:"
    grep -nE "error:|Error [0-9]|undefined reference|No such file" build.log | grep -v "Wmaybe-uninitialized" | head -30
    echo "(the whole output is in $PWD/build.log)"
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

# Azahar's SD folder: the Flatpak's (Bazzite and most desktops) or a native install's.
AZ=""
for d in "$HOME/.var/app/org.azahar_emu.Azahar/data/azahar-emu/sdmc" "$HOME/.local/share/azahar-emu/sdmc"; do
    if [ -d "$d" ]; then AZ="$d"; break; fi
done
if [ -n "$AZ" ]; then
    step "Found Azahar's SD folder: copying the files there too"
    mkdir -p "$AZ/3ds/pokeplatinum"
    cp "$SD_DIR/3ds/pokeplatinum.3dsx" "$AZ/3ds/"
    cp "$SD_DIR/3ds/pokeplatinum/platinum3ds.pak" "$SD_DIR/3ds/pokeplatinum/manifest.json" \
        "$AZ/3ds/pokeplatinum/"
else
    echo "(Azahar's SD folder wasn't found (open Azahar once to create it): copy the 3ds"
    echo " folder from $SD_DIR into Azahar's sdmc folder yourself.)"
fi

cat <<EOF

Done. The files are in:

  $SD_DIR/3ds/pokeplatinum.3dsx                     (the program, for the Homebrew Launcher)
  $SD_DIR/cia/pokeplatinum.cia                      (the same, to install on the HOME Menu with FBI)
  $SD_DIR/3ds/pokeplatinum/platinum3ds.pak          (your data)
  $SD_DIR/3ds/pokeplatinum/manifest.json

Azahar:  File > Load File... > $SD_DIR/3ds/pokeplatinum.3dsx

To build again later, run this script again: it updates the port and rebuilds.
EOF
