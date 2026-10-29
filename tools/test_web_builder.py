#!/usr/bin/env python3
"""The web builder, end to end, in a real browser (Chromium, with Playwright):

    python3 tools/test_web_builder.py --site _site --pyodide build/pyodide-core/pyodide \\
        --pycgfx build/pycgfx --rom made-up.nds [--cia --banner flat|3d]

The site (tools/build_site.py) must carry a release that accepts the ROM
(tools/build_web_payload.py --synthetic --synthetic-rom ROM ...). The test
serves the site, Pyodide and pycgfx from this computer, picks the ROM on the
page, builds, downloads the ZIP and checks it: the program is the release's,
the data pack and its manifest.json are what builder/web.py makes from the
ROM on this computer, and with --cia the .cia is there with the banner
expected. Needs `pip install playwright` and `playwright install chromium`.
"""
from __future__ import annotations

import argparse
import functools
import http.server
import io
import json
import os
import shutil
import sys
import tempfile
import threading
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".mjs": "text/javascript", ".js": "text/javascript", ".wasm": "application/wasm",
                      ".json": "application/json", ".zip": "application/zip"}

    def log_message(self, *args):
        pass


def serve(root: Path):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
                                             functools.partial(Handler, directory=str(root)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def expected_files(rom: bytes, followers: Path | None = None) -> dict[str, bytes]:
    import web
    with tempfile.TemporaryDirectory() as tmp:
        web.run_web_build(rom, tmp, verify=False, followers_dir=str(followers) if followers else None)
        return {name: (Path(tmp) / name).read_bytes() for name in (web.PAK, web.MANIFEST)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--site", type=Path, required=True)
    ap.add_argument("--pyodide", type=Path, required=True, help="Pyodide's folder (tools/fetch_pyodide.sh)")
    ap.add_argument("--pycgfx", type=Path, help="pycgfx's folder (else the page fetches it from GitHub)")
    ap.add_argument("--followers", type=Path,
                    help="the follower sprites at their paths in pokeheartgold (else the page fetches them from GitHub,"
                         " and the data pack isn't compared)")
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--cia", action="store_true", help="also make the HOME Menu version")
    ap.add_argument("--banner", choices=("flat", "3d"), default="flat", help="the .cia's banner expected")
    ap.add_argument("--lang", default="en")
    ap.add_argument("--timeout", type=int, default=600, help="seconds the build may take")
    ap.add_argument("--screenshot", type=Path, help="save the page when done")
    args = ap.parse_args()

    from playwright.sync_api import sync_playwright

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "www"
        shutil.copytree(args.site, root)
        shutil.copytree(args.pyodide, root / "pyodide-test")
        query = f"?lang={args.lang}&pyodide=/pyodide-test/"
        if args.pycgfx:
            shutil.copytree(args.pycgfx, root / "pycgfx-test", ignore=shutil.ignore_patterns(".git"))
            query += "&pycgfx=/pycgfx-test/"
        if args.followers:
            shutil.copytree(args.followers, root / "followers-test")
            query += "&followers=/followers-test/"
        server = serve(root)
        url = f"http://127.0.0.1:{server.server_address[1]}/{query}"
        release = root / "release"
        problems: list[str] = []
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(accept_downloads=True)
            page.on("pageerror", lambda e: problems.append(f"page error: {e}"))
            page.on("console", lambda m: print(f"  [console.{m.type}] {m.text}") if m.type in ("error", "warning") else None)
            page.goto(url)
            page.wait_for_selector("#rom:not([disabled])", state="attached", timeout=30_000)
            page.set_input_files("#rom", str(args.rom))
            page.wait_for_selector('#top[data-state="ok"]', timeout=60_000)
            if page.is_checked("#want-cia") != args.cia:
                page.click("#want-cia")
            page.click("#build")
            page.wait_for_selector('#top[data-state="done"], #top[data-state="error"]', timeout=args.timeout * 1000)
            state = page.get_attribute("#top", "data-state")
            title, detail = page.text_content("#top-title"), page.text_content("#top-detail")
            print(f"page: {state}: {title} {detail}")
            if args.screenshot:
                page.screenshot(path=str(args.screenshot), full_page=False)
            if state != "done":
                problems.append(f"the build stopped: {detail}")
            else:
                with page.expect_download() as info:
                    page.click("#download")
                data = Path(info.value.path()).read_bytes()
                banner = page.get_attribute("#top", "data-banner")
            browser.close()
        server.shutdown()

        if not problems:
            z = zipfile.ZipFile(io.BytesIO(data))
            if z.testzip() is not None:
                problems.append("the ZIP is damaged")
            names = set(z.namelist())
            print(f"ZIP: {len(data):,} bytes: {sorted(n for n in names if not n.endswith('/'))}")
            if z.read("3ds/pokeplatinum.3dsx") != (release / "pokeplatinum.3dsx").read_bytes():
                problems.append("the program isn't the release's")
            for name, want in expected_files(args.rom.read_bytes(), args.followers).items():
                if not args.followers and name.endswith(".pak"):
                    continue   # (the page's follower sprites, from GitHub: maybe not)
                if name not in names or z.read(name) != want:
                    problems.append(f"{name} isn't what builder/web.py makes from the ROM")
            if args.cia:
                cia = z.read("cia/pokeplatinum.cia") if "cia/pokeplatinum.cia" in names else b""
                if len(cia) < 0x2020 or int.from_bytes(cia[:4], "little") != 0x2020:
                    problems.append("no .cia, or not a .cia")
                if banner != args.banner:
                    problems.append(f"the .cia got the {banner} banner, not the {args.banner} one")
            elif "cia/pokeplatinum.cia" in names:
                problems.append("a .cia nobody asked for")
            if "README.txt" not in names:
                problems.append("no README.txt")

    for problem in problems:
        print(f"FAIL: {problem}")
    print("FAILED" if problems else "OK: the web builder made the expected SD card files")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
