"""The web builder's release files (tools/build_web_payload.py), made with a
made-up program: no game code or data."""
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))

from builder.test_web import synthetic_nds_with_banner  # noqa: E402


def run_payload(out: Path):
    subprocess.run([sys.executable, str(HERE / "build_web_payload.py"), "--synthetic", "--version", "1.2.3",
                    "--out", str(out)], check=True, stdout=subprocess.DEVNULL)


class Payload(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.out = self.dir / "dist"
        run_payload(self.out)
        self.manifest = json.loads((self.out / "web-manifest.json").read_text(encoding="utf-8"))
        self.zip = zipfile.ZipFile(self.out / "PokePlatinum3DS-WebPayload.zip")
        self.addCleanup(self.zip.close)

    def test_release_files(self):
        self.assertEqual(sorted(p.name for p in self.out.iterdir()),
                         ["PokePlatinum3DS-WebPayload.zip", "SHA256SUMS.txt", "pokeplatinum.3dsx",
                          "web-manifest.json"])
        sums = (self.out / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(sums), 3)
        for line in sums:
            digest, name = line.split("  ")
            self.assertEqual(hashlib.sha256((self.out / name).read_bytes()).hexdigest(), digest)

    def test_manifest(self):
        m = self.manifest
        self.assertEqual((m["schemaVersion"], m["kind"], m["releaseTag"]), (1, "pokeplatinum-3ds-web-payload", "v1.2.3"))
        self.assertTrue(m["synthetic"])
        self.assertEqual(m["dataPack"], {"format": 1, "buildId": "platinum-nitrofs-1"})
        self.assertEqual({r["sha1"] for r in m["supportedRoms"]},
                         {"0862ec35b24de5c7e2dcb88c9eea0873110d755c", "ce81046eda7d232513069519cb2085349896dec7"})
        self.assertTrue(m["pycgfx"]["rawBase"].endswith(m["pycgfx"]["commit"] + "/"))
        # The manifest in the ZIP is the release's, and lists every other file with its hash.
        self.assertEqual(json.loads(self.zip.read("web-manifest.json")), m)
        names = set(self.zip.namelist()) - {"web-manifest.json"}
        self.assertEqual(names, set(m["files"]))
        for name, entry in m["files"].items():
            data = self.zip.read(name)
            self.assertEqual((hashlib.sha256(data).hexdigest(), len(data)), (entry["sha256"], entry["size"]))
        for path in (m["cia"]["elf"], m["cia"]["icon"], m["cia"]["rsf"], m["cia"]["chime"],
                     "program/pokeplatinum.3dsx", "python/builder/web.py"):
            self.assertIn(path, names)

    def test_no_game_files_or_pycgfx(self):
        for name in self.zip.namelist():
            self.assertFalse(name.endswith((".nds", ".pak", ".narc", ".cgfx", ".bnr", ".cia")), name)
            self.assertNotIn("__pycache__", name)
            self.assertNotIn("pycgfx", name.lower())
            self.assertFalse(Path(name).name.startswith("test_"), name)

    def test_deterministic(self):
        again = self.dir / "again"
        run_payload(again)
        for name in ("PokePlatinum3DS-WebPayload.zip", "web-manifest.json"):
            self.assertEqual((again / name).read_bytes(), (self.out / name).read_bytes(), name)

    def test_the_payloads_builder_runs(self):
        # The builder as the website gets it: from the ZIP, its own entry point.
        unpacked = self.dir / "unpacked"
        self.zip.extractall(unpacked)
        entry = self.manifest["entrypoint"]
        paths = [str(unpacked / p) for p in entry["pythonPath"]]
        script = (f"import sys; sys.path[:0] = {paths!r}\n"
                  f"import {entry['module']} as m\n"
                  f"rom = open(sys.argv[1], 'rb').read()\n"
                  f"r = m.{entry['function']}(rom, sys.argv[2], verify=False)\n"
                  "assert m.__file__.startswith(sys.argv[3]), m.__file__\n"
                  "print(sorted(r['files']))\n")
        rom = self.dir / "made-up.nds"
        rom.write_bytes(synthetic_nds_with_banner())
        result = subprocess.run([sys.executable, "-I", "-c", script, str(rom), str(self.dir / "sd"), str(unpacked)],
                                capture_output=True, text=True, cwd=self.dir)
        self.assertEqual(result.returncode, 0, result.stderr)
        outputs = entry["outputs"]
        self.assertTrue((self.dir / "sd" / outputs["pak"]).is_file())
        self.assertTrue((self.dir / "sd" / outputs["manifest"]).is_file())
        self.assertTrue((self.dir / "sd" / outputs["flatBanner"]).is_file())


if __name__ == "__main__":
    unittest.main()
