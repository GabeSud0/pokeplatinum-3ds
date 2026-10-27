#!/usr/bin/env python3
"""The web builder's website, ready to publish (GitHub Pages):

    python3 tools/build_site.py --out _site                  (the latest release, with gh)
    python3 tools/build_site.py --out _site --release-dir DIR  (a release's files in DIR)

copies web/ to OUT (with the logo and the HOME Menu icon), and the release's files the page uses to OUT/release/:
web-manifest.json, PokePlatinum3DS-WebPayload.zip, pokeplatinum.3dsx and
SHA256SUMS.txt (GitHub doesn't let a page download release files from
another site, so the site carries the latest release's). OUT/release/
releases.json lists every release: its data pack format, for the page's
"Updating" helper, and its notes (the release's description on GitHub, as
GitHub writes it in HTML, kept to plain formatting), for "What's new". With
no release yet, the page says so.

With gh, GH_TOKEN (or gh's login) and --repo (default: GITHUB_REPOSITORY,
else GabeSud0/pokeplatinum-3ds) say where the releases are.
"""
from __future__ import annotations

import argparse
import html
import html.parser
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGES = {"docs/images/logo.png": "logo.png", "3ds_port/game/cia/icon.png": "icon.png"}
ASSETS = ["web-manifest.json", "PokePlatinum3DS-WebPayload.zip", "pokeplatinum.3dsx", "SHA256SUMS.txt"]


def gh_api(path: str, *, raw: bool = False, accept: str | None = None):
    """GitHub's REST API through gh (its login or GH_TOKEN)."""
    args = ["gh", "api", path]
    if raw:
        accept = "application/octet-stream"
    if accept:
        args[2:2] = ["-H", f"Accept: {accept}"]
    out = subprocess.run(args, check=True, capture_output=True).stdout
    return out if raw else json.loads(out)


class _Notes(html.parser.HTMLParser):
    """Release notes' HTML kept to plain formatting: these elements, links
    to web pages only, and no other attributes; anything else's text kept
    as text (a script's or a style's dropped)."""
    KEEP = {"p", "br", "hr", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "strong", "b", "em", "i",
            "code", "pre", "blockquote", "a", "del", "s", "sub", "sup"}
    VOID = {"br", "hr"}
    DROP = {"script", "style", "template", "iframe", "object", "svg", "math"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.out, self.open, self.dropping = [], [], 0

    def handle_starttag(self, tag, attrs):
        if tag in self.DROP:
            self.dropping += 1
        if self.dropping or tag not in self.KEEP:
            return
        if tag == "a":
            href = dict(attrs).get("href") or ""
            if href.startswith(("https://", "http://")):
                self.out.append(f'<a href="{html.escape(href, quote=True)}" rel="noopener">')
            else:
                self.out.append("<a>")
        else:
            self.out.append(f"<{tag}>")
        if tag not in self.VOID:
            self.open.append(tag)

    def handle_endtag(self, tag):
        if tag in self.DROP:
            self.dropping = max(0, self.dropping - 1)
            return
        if self.dropping or tag not in self.open:
            return
        while self.open:                         # (closes what was left open inside it)
            inner = self.open.pop()
            self.out.append(f"</{inner}>")
            if inner == tag:
                break

    def handle_data(self, data):
        if not self.dropping:
            self.out.append(html.escape(data, quote=False))


def clean_notes(text: str | None) -> str:
    parser = _Notes()
    parser.feed(text or "")
    parser.close()
    parser.out.extend(f"</{tag}>" for tag in reversed(parser.open))
    return "".join(parser.out).strip()


def summary(manifest: dict, release: dict | None = None) -> dict:
    release = release or {}
    return {"tag": manifest["releaseTag"], "version": manifest["releaseVersion"],
            "name": release.get("name") or manifest["releaseTag"],
            "published": release.get("published_at"), "prerelease": bool(release.get("prerelease")),
            "url": release.get("html_url"), "notes": clean_notes(release.get("body_html")),
            "dataPack": manifest.get("dataPack")}


def from_github(repo: str, release_dir: Path) -> list[dict]:
    """The latest release's files into release_dir; every release's summary."""
    listed = [r for r in gh_api(f"repos/{repo}/releases?per_page=100",       # (newest first; notes in HTML)
                                accept="application/vnd.github.html+json") if not r["draft"]]
    summaries, usable = [], []
    for release in listed:
        assets = {a["name"]: a for a in release["assets"]}
        if "web-manifest.json" not in assets or any(name not in assets for name in ASSETS):
            continue                            # a release from before the web builder
        manifest = json.loads(gh_api(f"repos/{repo}/releases/assets/{assets['web-manifest.json']['id']}", raw=True))
        summaries.append(summary(manifest, release))
        usable.append((release, assets))
    # The newest full release; a pre-release only while there's none.
    latest = next((u for u in usable if not u[0]["prerelease"]), usable[0] if usable else None)
    if latest is not None:
        release_dir.mkdir(parents=True, exist_ok=True)
        for name in ASSETS:
            data = gh_api(f"repos/{repo}/releases/assets/{latest[1][name]['id']}", raw=True)
            (release_dir / name).write_bytes(data)
    return summaries


def from_folder(folder: Path, release_dir: Path) -> list[dict]:
    release_dir.mkdir(parents=True, exist_ok=True)
    for asset in ASSETS:
        shutil.copy2(folder / asset, release_dir / asset)
    manifest = json.loads((folder / "web-manifest.json").read_text(encoding="utf-8"))
    return [summary(manifest)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--release-dir", type=Path, help="a release's files (tools/build_web_payload.py --out)")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", "GabeSud0/pokeplatinum-3ds"))
    args = ap.parse_args()

    if args.out.exists():
        shutil.rmtree(args.out)
    shutil.copytree(ROOT / "web", args.out)
    # The project's own pictures, where they are kept in the repository.
    (args.out / "img").mkdir(exist_ok=True)
    for src, dst in IMAGES.items():
        shutil.copy2(ROOT / src, args.out / "img" / dst)
    release_dir = args.out / "release"
    releases = from_folder(args.release_dir, release_dir) if args.release_dir else from_github(args.repo, release_dir)
    if releases:
        release_dir.mkdir(parents=True, exist_ok=True)
        (release_dir / "releases.json").write_text(json.dumps({"releases": releases}, indent=2) + "\n",
                                                   encoding="utf-8")
    latest = json.loads((release_dir / "web-manifest.json").read_text())["releaseTag"] \
        if (release_dir / "web-manifest.json").is_file() else "none"
    (args.out / ".nojekyll").write_text("")
    print(f"Site in {args.out}: release {latest}, {len(releases)} release(s) listed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
