"""Offline tests for the CI artifact allow-list check."""
from __future__ import annotations

import unittest

from check_ci_artifacts import ALLOWED, uploaded_paths

WORKFLOW = """
jobs:
  build:
    steps:
      - name: Build
        run: make

      - name: Upload hello3d
        uses: actions/upload-artifact@v4
        with:
          name: hello3d
          path: 3ds_port/hello3d/hello3d.3dsx

      - name: Upload several
        uses: actions/upload-artifact@v4
        with:
          name: reports
          path: |
            build/game-link-report/3ds.md
            3ds_port/game/build/3ds/platinum.elf

      - uses: actions/upload-artifact@v4
        with:
          name: no-path

      - name: Not an upload
        run: echo path: anything
"""


class CiArtifactTests(unittest.TestCase):
    def test_finds_every_uploaded_path(self):
        self.assertEqual(uploaded_paths(WORKFLOW), [
            "3ds_port/hello3d/hello3d.3dsx",
            "build/game-link-report/3ds.md",
            "3ds_port/game/build/3ds/platinum.elf",
            "<upload step without a path>",
        ])

    def test_allow_list_has_no_game_builds(self):
        import fnmatch
        for path in ("3ds_port/platinum_smoke/platinum-smoke.3dsx",
                     "3ds_port/game/build/3ds/libplatinum_game.a",
                     "3ds_port/game/build/3ds/link/platinum.elf"):
            self.assertFalse(any(fnmatch.fnmatch(path, p) for p in ALLOWED), path)


if __name__ == "__main__":
    unittest.main()
