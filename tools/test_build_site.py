"""The website's build (tools/build_site.py) from a made-up release: no
game code or data."""
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


class Site(unittest.TestCase):
    def test_site_with_a_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            rom = tmp / "made-up.nds"
            rom.write_bytes(b"not a game" * 100)
            subprocess.run([sys.executable, str(HERE / "build_web_payload.py"), "--synthetic", "--version", "2.0.0",
                            "--synthetic-rom", str(rom), "--out", str(tmp / "release")],
                           check=True, stdout=subprocess.DEVNULL)
            subprocess.run([sys.executable, str(HERE / "build_site.py"), "--out", str(tmp / "site"),
                            "--release-dir", str(tmp / "release")], check=True, stdout=subprocess.DEVNULL)
            site = tmp / "site"
            for page in ("index.html", "js/app.mjs", "js/worker.mjs", "js/wasi.mjs", "js/cia.mjs", "js/zip.mjs",
                         "css/site.css", ".nojekyll", "img/logo.png", "img/icon.png"):
                self.assertTrue((site / page).is_file(), page)
            for asset in ("web-manifest.json", "PokePlatinum3DS-WebPayload.zip", "pokeplatinum.3dsx",
                          "SHA256SUMS.txt"):
                self.assertEqual((site / "release" / asset).read_bytes(), (tmp / "release" / asset).read_bytes())
            releases = json.loads((site / "release" / "releases.json").read_text())["releases"]
            self.assertEqual([r["tag"] for r in releases], ["v2.0.0"])
            self.assertEqual(releases[0]["dataPack"], {"format": 1, "buildId": "platinum-nitrofs-1"})
            manifest = json.loads((site / "release" / "web-manifest.json").read_text())
            self.assertIn({"sha1": hashlib.sha1(rom.read_bytes()).hexdigest(), "name": "Made-up test ROM (not a game)"},
                          manifest["supportedRoms"])

    def test_release_notes_kept_to_plain_formatting(self):
        sys.path.insert(0, str(HERE))
        from build_site import clean_notes
        self.assertEqual(
            clean_notes('<h2 dir="auto">New</h2><ul><li><strong>Fixed</strong> '
                        '<a href="https://example.com/?a=1&amp;b=2" class="x">this</a></li>'
                        '<li><a href="javascript:alert(1)">that</a></li></ul>'
                        '<script>alert(1)</script><img src=x onerror=alert(1)><p>1 &lt; 2<div>end'),
            '<h2>New</h2><ul><li><strong>Fixed</strong> <a href="https://example.com/?a=1&amp;b=2" rel="noopener">'
            'this</a></li><li><a>that</a></li></ul><p>1 &lt; 2end</p>')
        self.assertEqual(clean_notes(None), "")

    def test_every_page_string_in_both_languages(self):
        # Every data-i18n key in index.html is in i18n.mjs, in English and Spanish.
        import re
        html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
        i18n = (ROOT / "web" / "js" / "i18n.mjs").read_text(encoding="utf-8")
        keys = set(re.findall(r'data-i18n(?:-html)?="([^"]+)"', html))
        en, es = i18n.split("\n  es: {")
        for key in keys:
            self.assertIn(f'"{key}":', en, key)
            self.assertIn(f'"{key}":', es, key)
        self.assertEqual(set(re.findall(r'^    "([\w.]+)":', en, re.M)), set(re.findall(r'^    "([\w.]+)":', es, re.M)))


if __name__ == "__main__":
    unittest.main()
