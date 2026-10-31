#!/usr/bin/env bash
# Try the web builder's website on your own computer, with the program you
# built (build_linux.sh, or `make game` and `make game-cia`), before it's
# released:
#
#   bash tools/preview_site.sh            then open http://localhost:8000
#
# Makes a release's files from your build (tools/build_web_payload.py), the
# site around them (tools/build_site.py) in build/site-preview, and serves it
# on this computer only. The page then works as the published one will: pick
# your ROM, build, download the ZIP. Ctrl+C stops it. Needs Python 3, git,
# curl and tar; the browser tools for the .cia (tools/build_cia_tools_wasm.sh)
# are built once into ~/pokeplatinum-cia-tools-wasm.
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${PORT:-8000}"
OUT=3ds_port/game/build/3ds
WASM="${WASM:-$HOME/pokeplatinum-cia-tools-wasm}"
for f in "$OUT/pokeplatinum.3dsx" "$OUT/forwarder.elf" "$OUT/pokeplatinum.smdh"; do
    if [ ! -f "$f" ]; then
        echo "$f isn't there: build the program first (bash build_linux.sh your.nds, or make game and make game-cia)."
        exit 1
    fi
done

echo "==> The .cia tools for the browser (once)"
if [ ! -f "$WASM/makerom.wasm" ] || [ ! -f "$WASM/bannertool.wasm" ]; then
    bash tools/build_cia_tools_wasm.sh "$WASM"
fi

echo "==> pycgfx (the 3D banner's converter: listed in the release, fetched by the page)"
python3 tools/banner3d/make_banner.py --fetch-pycgfx-only

echo "==> A release's files from your build"
rm -rf build/site-preview build/release-preview
python3 tools/build_web_payload.py --version 0.0.0-preview --commit "$(git rev-parse HEAD)" \
    --program "$OUT/pokeplatinum.3dsx" --elf "$OUT/forwarder.elf" --smdh "$OUT/pokeplatinum.smdh" \
    --cia-tools "$WASM" --pycgfx build/pycgfx --out build/release-preview
python3 tools/build_site.py --out build/site-preview --release-dir build/release-preview

echo
echo "==> Open http://localhost:$PORT in your browser (Ctrl+C here to stop)"
echo "    The files it makes are yours: don't share them."
# (the page's scripts are .mjs: served as JavaScript whatever the system's MIME list says)
exec python3 - "$PORT" build/site-preview <<'PY'
import functools, http.server, sys
class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".mjs": "text/javascript", ".wasm": "application/wasm"}
server = http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])),
                                         functools.partial(Handler, directory=sys.argv[2]))
try:
    server.serve_forever()
except KeyboardInterrupt:
    pass
PY
