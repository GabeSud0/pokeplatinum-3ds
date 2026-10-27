#!/usr/bin/env bash
# Pyodide (Python for the browser), the version the web builder loads
# (tools/build_web_payload.py --pyodide-version), for testing the builder
# outside a browser's own download of it:
#
#   bash tools/fetch_pyodide.sh DEST      -> DEST/pyodide/pyodide.mjs ...
#
# Its "core" files only (Python and its standard library: all the builder
# needs), from Pyodide's GitHub release, checked by their SHA-256.
set -euo pipefail

DEST="${1:?usage: fetch_pyodide.sh DEST}"
VERSION=0.27.7
SHA256=9bc8f127db6c590b191b9aee754022cb41b1a36c7bac233776c11c5ecb541be8
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ "$(python3 "$HERE/build_web_payload.py" --pyodide-version)" != "$VERSION" ]; then
    echo "fetch_pyodide.sh: update VERSION and SHA256 to build_web_payload.py's Pyodide" >&2
    exit 1
fi
if [ -f "$DEST/pyodide/pyodide.mjs" ] && [ "$(cat "$DEST/version" 2>/dev/null)" = "$VERSION" ]; then
    exit 0
fi
mkdir -p "$DEST"
curl -fsSL -o "$DEST/core.tar.bz2" \
    "https://github.com/pyodide/pyodide/releases/download/$VERSION/pyodide-core-$VERSION.tar.bz2"
echo "$SHA256  $DEST/core.tar.bz2" | sha256sum -c --quiet
rm -rf "$DEST/pyodide"
tar xjf "$DEST/core.tar.bz2" -C "$DEST"
rm "$DEST/core.tar.bz2"
echo "$VERSION" > "$DEST/version"
echo "Pyodide $VERSION in $DEST/pyodide"
