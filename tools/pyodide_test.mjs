// Runs Python tests in Pyodide, the Python the web builder runs in the
// player's browser, to check the builder works there as on a PC:
//
//   npm install --prefix build/pyodide pyodide@VERSION
//   node tools/pyodide_test.mjs build/pyodide/node_modules/pyodide TEST_MODULE...
//
// (VERSION: python3 tools/build_web_payload.py --pyodide-version, the one the
// website loads.) The repository is mounted at /repo, and the tests run from
// there as `python -m unittest TEST_MODULE...` would. Fails if a test fails
// or is skipped (a skipped test checked nothing).
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const [pyodideDir, ...modules] = process.argv.slice(2);
if (!pyodideDir || modules.length === 0) {
  console.error("usage: node tools/pyodide_test.mjs PYODIDE_DIR TEST_MODULE...");
  process.exit(2);
}
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const { loadPyodide } = await import(pathToFileURL(path.resolve(pyodideDir, "pyodide.mjs")).href);
const pyodide = await loadPyodide();
pyodide.FS.mkdir("/repo");
pyodide.FS.mount(pyodide.FS.filesystems.NODEFS, { root: repo }, "/repo");
pyodide.globals.set("test_modules", pyodide.toPy(modules));
const ok = await pyodide.runPythonAsync(`
import os, sys, unittest
sys.dont_write_bytecode = True
os.chdir("/repo")
sys.path.insert(0, "/repo")
print(f"Python {sys.version.split()[0]} on {sys.platform}")
suite = unittest.defaultTestLoader.loadTestsFromNames(list(test_modules))
result = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite)
for test, why in result.skipped:
    print(f"SKIPPED (not allowed here): {test}: {why}")
result.wasSuccessful() and not result.skipped and result.testsRun > 0
`);
process.exit(ok ? 0 : 1);
