#!/usr/bin/env bash
# CI (build-3ds, after tools/ci_cia_wasm.sh): the website (web/), end to end
# in Chromium (tools/test_web_builder.py): this program, its .cia tools and
# pycgfx in a release that also accepts a made-up ROM (no game data here);
# the page builds the SD card's files and the ZIP is checked. The made-up ROM
# has no title screen, so the .cia gets the flat banner.
set -euxo pipefail
apt-get install -y --no-install-recommends python3-pip bzip2
pip3 install --break-system-packages -q playwright==1.48.0
python3 -m playwright install --with-deps chromium > /dev/null
bash tools/fetch_pyodide.sh build/pyodide-core
python3 - <<'PY'
import sys
sys.path.insert(0, ".")
from builder.test_web import synthetic_nds_with_banner
open("build/web/made-up.nds", "wb").write(synthetic_nds_with_banner())
PY
OUT=3ds_port/game/build/3ds
python3 tools/build_web_payload.py --synthetic --synthetic-rom build/web/made-up.nds \
    --program "$OUT/pokeplatinum.3dsx" --elf build/web/pokeplatinum.elf \
    --smdh "$OUT/pokeplatinum.smdh" --cia-tools build/cia-tools-wasm --pycgfx build/pycgfx \
    --out build/web/test-release
python3 tools/build_site.py --out build/web/test-site --release-dir build/web/test-release
# The follower sprites (builder/followers.py: downloaded and checked here),
# laid out at their paths in pokeheartgold for the page to fetch.
python3 builder/followers.py -o build/web/followers.bin --cache build/web/follower-sprites
python3 - <<'PY'
import shutil, sys
from pathlib import Path
sys.path.insert(0, "builder")
import follower_sources as s
for n in range(len(s.FILES)):
    dest = Path("build/web/followers-site") / s.SPRITE_PATH.format(s.FIRST_FILE + n)
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(f"build/web/follower-sprites/{s.FIRST_FILE + n:08d}.NSBTX", dest)
PY
python3 tools/test_web_builder.py --site build/web/test-site --pyodide build/pyodide-core/pyodide \
    --pycgfx build/pycgfx --followers build/web/followers-site --rom build/web/made-up.nds --cia --banner flat
