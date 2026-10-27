#!/usr/bin/env bash
# bannertool and makerom for the browser: the same two tools as
# tools/build_cia_tools.sh (the same versions), compiled to WebAssembly
# (WASI) with wasi-sdk, for the web builder (web/js/cia.mjs runs them):
#
#   bash tools/build_cia_tools_wasm.sh DEST
#
# writes DEST/bannertool.wasm, DEST/makerom.wasm and DEST/LICENSES.md (their
# licences and where their sources are). Needs git, curl and tar; wasi-sdk is
# fetched into DEST/wasi-sdk (a pinned release, checked by its SHA-256: about
# 115 MB on Linux, 520 MB on Windows) unless WASI_SDK names one. tools/test_cia_wasm.mjs checks that they make
# what the native tools make.
set -euo pipefail

DEST="${1:?usage: build_cia_tools_wasm.sh DEST}"
MAKEROM_REPO=https://github.com/3DSGuy/Project_CTR.git
MAKEROM_TAG=makerom-v0.18.4
BANNERTOOL_REPO=https://github.com/diasurgical/bannertool.git
BANNERTOOL_TAG=1.2.0
WASI_SDK_VERSION=25
# (the compiler for this computer: Linux, or Windows in devkitPro's MSYS2; what
# it makes is the same either way)
case "$(uname -s)" in
    Linux)
        WASI_SDK_HOST=x86_64-linux
        WASI_SDK_SHA256=52640dde13599bf127a95499e61d6d640256119456d1af8897ab6725bcf3d89c ;;
    MINGW*|MSYS*|CYGWIN*)
        WASI_SDK_HOST=x86_64-windows
        WASI_SDK_SHA256=ebd4868f95a6e764bf29283e3bdf54df0df3902a29737176da9c17da218b939b ;;
    *)
        echo "build_cia_tools_wasm.sh: wasi-sdk is fetched for Linux or Windows only (or set WASI_SDK)" >&2
        exit 1 ;;
esac
WASI_SDK_URL="https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-$WASI_SDK_VERSION/wasi-sdk-$WASI_SDK_VERSION.0-$WASI_SDK_HOST.tar.gz"

mkdir -p "$DEST"
DEST="$(cd "$DEST" && pwd)"

if [ -z "${WASI_SDK:-}" ]; then
    WASI_SDK="$DEST/wasi-sdk"
    if [ ! -x "$WASI_SDK/bin/clang" ]; then
        echo "Fetching wasi-sdk $WASI_SDK_VERSION"
        curl -fsSL -o "$DEST/wasi-sdk.tar.gz" "$WASI_SDK_URL"
        echo "$WASI_SDK_SHA256  $DEST/wasi-sdk.tar.gz" | sha256sum -c --quiet
        rm -rf "$WASI_SDK" && mkdir -p "$WASI_SDK"
        tar xzf "$DEST/wasi-sdk.tar.gz" -C "$WASI_SDK" --strip-components=1
        rm "$DEST/wasi-sdk.tar.gz"
    fi
fi
# (makerom prints the time it was built: a fixed one, so every build makes the same files)
export SOURCE_DATE_EPOCH=1577836800
CC=("$WASI_SDK/bin/clang" --sysroot="$WASI_SDK/share/wasi-sysroot" --target=wasm32-wasip1 -O2)
CXX=("$WASI_SDK/bin/clang++" --sysroot="$WASI_SDK/share/wasi-sysroot" --target=wasm32-wasip1 -O2)
LINK=(-Wl,--strip-all)

fetch() {   # fetch REPO TAG DIR: just that tag's files
    rm -rf "$3"
    git init -q "$3"
    git -C "$3" fetch -q --depth 1 "$1" "refs/tags/$2"
    git -C "$3" checkout -q FETCH_HEAD
}

SRC="$DEST/src"
mkdir -p "$SRC"

echo "Building bannertool.wasm ($BANNERTOOL_TAG)"
fetch "$BANNERTOOL_REPO" "$BANNERTOOL_TAG" "$SRC/bannertool"
(
    cd "$SRC/bannertool"
    "${CC[@]}" -c source/pc/stb_image.c -o stb_image.o
    "${CC[@]}" -c source/pc/stb_vorbis.c -o stb_vorbis.o
    # (no C++ exceptions in WASI: it throws none itself)
    "${CXX[@]}" -fno-exceptions -DVERSION_MAJOR=1 -DVERSION_MINOR=2 -DVERSION_MICRO=0 -Wno-unused-function \
        "${LINK[@]}" -o "$DEST/bannertool.wasm" \
        source/*.cpp source/3ds/*.cpp source/pc/*.cpp stb_image.o stb_vorbis.o
)

echo "Building makerom.wasm ($MAKEROM_TAG)"
fetch "$MAKEROM_REPO" "$MAKEROM_TAG" "$SRC/Project_CTR"
(
    cd "$SRC/Project_CTR/makerom"
    # Its own makefile builds its three libraries and then makerom; here in
    # one go. mbedtls's network, timer and system-randomness parts are left
    # out (makerom uses none; they don't build for WASI).
    mapfile -t sources < <(ls src/*.c deps/libblz/src/*.c deps/libyaml/src/*.c deps/libmbedtls/src/*.c |
                           grep -v -E '/(entropy_poll|net_sockets|timing)\.c$')
    "${CC[@]}" -std=c11 -D_GNU_SOURCE -Wno-everything \
        -Ideps/libmbedtls/include -Ideps/libblz/include -Ideps/libyaml/include \
        "${LINK[@]}" -o "$DEST/makerom.wasm" "${sources[@]}"
)

cat > "$DEST/LICENSES.md" <<EOF
# bannertool.wasm and makerom.wasm

Built by tools/build_cia_tools_wasm.sh (pokeplatinum-3ds) with wasi-sdk $WASI_SDK_VERSION, unchanged, from:

- bannertool $BANNERTOOL_TAG: $BANNERTOOL_REPO (MIT licence, below), with stb_image and stb_vorbis (public domain / MIT).
- makerom $MAKEROM_TAG: $MAKEROM_REPO (MIT licence, below), with its bundled libraries:
  mbedtls (Apache-2.0), libyaml (MIT) and blz by CUE (GPL-3.0-or-later).
  makerom.wasm as a whole is therefore under the GPL-3.0, like this project; its
  sources are at the address above, at that tag.
- wasi-libc and libc++ (wasi-sdk): their own permissive licences
  (https://github.com/WebAssembly/wasi-sdk).

## bannertool

$(cat "$SRC/bannertool/LICENSE"*)

## makerom

$(cat "$SRC/Project_CTR/makerom/LICENSE")

## libyaml

$(cat "$SRC/Project_CTR/makerom/deps/libyaml/LICENSE")

## mbedtls

$(cat "$SRC/Project_CTR/makerom/deps/libmbedtls/apache-2.0.txt")

## blz

GPL-3.0-or-later: the licence text is this project's LICENSE.
EOF

ls -l "$DEST/bannertool.wasm" "$DEST/makerom.wasm"
