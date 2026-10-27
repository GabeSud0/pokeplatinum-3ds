#!/usr/bin/env bash
# CI (build-3ds, after the program and the native CIA tools are built): the
# CIA tools for the browser (tools/build_cia_tools_wasm.sh) against the
# native ones on this program (tools/test_cia_wasm.mjs): the flat banner and
# a made-up 3D scene (no ROM in CI).
set -euxo pipefail
apt-get install -y --no-install-recommends curl nodejs
node --version
bash tools/build_cia_tools_wasm.sh build/cia-tools-wasm
python3 tools/banner3d/make_banner.py --fetch-pycgfx-only
python3 - <<'PY'
import sys, tempfile
sys.path[:0] = ["tools", "tools/banner3d"]
import test_banner3d, to_cgfx
with tempfile.TemporaryDirectory() as d:
    open("build/web/synthetic.cgfx", "wb").write(to_cgfx.convert("build/pycgfx", test_banner3d.synthetic_scene(d)))
PY
OUT=3ds_port/game/build/3ds
node tools/test_cia_wasm.mjs --wasm build/cia-tools-wasm --native build/cia-tools/bin \
    --elf build/web/pokeplatinum.elf --icon "$OUT/pokeplatinum.smdh" \
    --rsf 3ds_port/game/cia/pokeplatinum.rsf --chime 3ds_port/game/cia/chime.wav \
    --flat build/home-menu-art/banner.png --cgfx build/web/synthetic.cgfx
