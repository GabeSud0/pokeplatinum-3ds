#!/usr/bin/env bash
# Build the two tools that make the HOME Menu version of the program (a
# .cia), from source, at fixed versions:
#
#   bash tools/build_cia_tools.sh DEST
#
# makerom (3DSGuy/Project_CTR, makerom-v0.18.4): the .cia itself.
# bannertool (diasurgical/bannertool, 1.2.0): the HOME Menu's banner.
# Both into DEST/bin; done once (DEST/versions says which), then reused.
# Needs git, gcc, g++ and make (the build script installs them on Windows).
set -euo pipefail

DEST="${1:?usage: build_cia_tools.sh DEST}"
MAKEROM_REPO=https://github.com/3DSGuy/Project_CTR.git
MAKEROM_TAG=makerom-v0.18.4
BANNERTOOL_REPO=https://github.com/diasurgical/bannertool.git
BANNERTOOL_TAG=1.2.0
VERSIONS="$MAKEROM_TAG $BANNERTOOL_TAG"

mkdir -p "$DEST/bin"
DEST="$(cd "$DEST" && pwd)"
if [ -f "$DEST/versions" ] && [ "$(cat "$DEST/versions")" = "$VERSIONS" ] &&
   { [ -x "$DEST/bin/makerom" ] || [ -x "$DEST/bin/makerom.exe" ]; } &&
   { [ -x "$DEST/bin/bannertool" ] || [ -x "$DEST/bin/bannertool.exe" ]; }; then
    echo "makerom and bannertool: already built ($VERSIONS)"
    exit 0
fi

fetch() {   # fetch REPO TAG DIR: just that tag's files
    rm -rf "$3"
    git init -q "$3"
    git -C "$3" fetch -q --depth 1 "$1" "refs/tags/$2"
    git -C "$3" checkout -q FETCH_HEAD
}

SRC="$DEST/src"
mkdir -p "$SRC"

echo "Building makerom ($MAKEROM_TAG)"
fetch "$MAKEROM_REPO" "$MAKEROM_TAG" "$SRC/Project_CTR"
# PROJECT_PLATFORM=GNU: the plain gcc (its WIN32 setting wants MinGW's
# cross-compilers, which MSYS2's own gcc isn't).
make -C "$SRC/Project_CTR/makerom" PROJECT_PLATFORM=GNU deps >/dev/null
make -C "$SRC/Project_CTR/makerom" PROJECT_PLATFORM=GNU -j"$(nproc)" >/dev/null
cp "$SRC/Project_CTR/makerom/bin/"makerom* "$DEST/bin/"

echo "Building bannertool ($BANNERTOOL_TAG)"
fetch "$BANNERTOOL_REPO" "$BANNERTOOL_TAG" "$SRC/bannertool"
(
    cd "$SRC/bannertool"
    # Its own Makefile needs a submodule (buildtools); the sources alone
    # build with one command.
    gcc -O2 -c source/pc/stb_image.c -o stb_image.o
    gcc -O2 -c source/pc/stb_vorbis.c -o stb_vorbis.o
    g++ -O2 -DVERSION_MAJOR=1 -DVERSION_MINOR=2 -DVERSION_MICRO=0 -Wno-unused-function \
        -o "$DEST/bin/bannertool" source/*.cpp source/3ds/*.cpp source/pc/*.cpp stb_image.o stb_vorbis.o
)

echo "$VERSIONS" > "$DEST/versions"
echo "makerom and bannertool built in $DEST/bin"
