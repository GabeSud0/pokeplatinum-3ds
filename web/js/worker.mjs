// The build, in a Web Worker so the page stays responsive. app.mjs sends:
//
//   { type: "build", rom: ArrayBuffer, wantCia, manifest, releaseUrl, pyodideUrl, pycgfxUrl }
//
// and gets back { type: "progress", stage, fraction } as it goes, then
// { type: "done", zip: Blob, fileName, banner, bannerWhy } or
// { type: "error", key, values, detail }.
//
// The steps: Pyodide (Python in WebAssembly) loads; this version's payload
// (web-manifest.json's files, checked by their SHA-256) is unpacked into
// Pyodide's own in-memory files; builder/web.py makes the data pack and the
// banner's pieces from the ROM; web/js/cia.mjs makes the .cia; zip.mjs puts
// the SD card's files in a ZIP. Nothing leaves the browser.
import { makeCia } from "./cia.mjs";
import { makeZip } from "./zip.mjs";

class BuildError extends Error {
  constructor(key, values = {}, detail = "") {
    super(detail || key);
    this.key = key;
    this.values = values;
    this.detail = detail;
  }
}

const post = (message, transfer) => self.postMessage(message, transfer || []);
// The bar's share of each step (they add up to 1).
const WEIGHTS = { python: 0.12, release: 0.08, rom: 0.05, pack: 0.45, banner: 0.1, cia: 0.12, zip: 0.08 };
const ORDER = Object.keys(WEIGHTS);
function progress(stage, part = 0) {
  let before = 0;
  for (const s of ORDER) {
    if (s === stage) break;
    before += WEIGHTS[s];
  }
  post({ type: "progress", stage, fraction: Math.min(1, before + WEIGHTS[stage] * Math.max(0, Math.min(1, part))) });
}

async function sha256(data) {
  const digest = await crypto.subtle.digest("SHA-256", data);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function download(url, what) {
  let response;
  try {
    response = await fetch(url);
  } catch (e) {
    throw new BuildError("error.network", { what }, `${url}: ${e.message}`);
  }
  if (!response.ok) throw new BuildError("error.network", { what }, `${url}: HTTP ${response.status}`);
  return new Uint8Array(await response.arrayBuffer());
}

let pyodidePromise = null;
async function getPyodide(pyodideUrl) {
  if (!pyodidePromise) {
    pyodidePromise = (async () => {
      const { loadPyodide } = await import(`${pyodideUrl}pyodide.mjs`);
      return loadPyodide({ indexURL: pyodideUrl });
    })();
    pyodidePromise.catch(() => { pyodidePromise = null; });
  }
  return pyodidePromise;
}

function mkdirs(FS, path) {
  let at = "";
  for (const part of path.split("/").filter(Boolean)) {
    at += "/" + part;
    try { FS.mkdir(at); } catch { /* already there */ }
  }
}

function readmeText(version) {
  return [
    `Pokémon Platinum 3DS ${version}`,
    "",
    "Copy the 3ds and cia folders to the root of your SD card (merge them with the folders already there).",
    "- Homebrew Launcher: run pokeplatinum.3dsx.",
    "- HOME Menu: in FBI, SD > cia > pokeplatinum.cia > Install CIA.",
    "The game saves to sd:/3ds/pokeplatinum/platinum.sav.",
    "These files hold the game's data from your own cartridge: please don't share them.",
    "",
    "Copia las carpetas 3ds y cia a la raíz de tu tarjeta SD (combínalas con las que ya están ahí).",
    "- Homebrew Launcher: abre pokeplatinum.3dsx.",
    "- Menú HOME: en FBI, SD > cia > pokeplatinum.cia > Install CIA.",
    "El juego guarda en sd:/3ds/pokeplatinum/platinum.sav.",
    "Estos archivos contienen los datos del juego de tu propio cartucho: por favor no los compartas.",
    "",
  ].join("\r\n");
}

async function build(request) {
  const { wantCia, manifest, releaseUrl, pyodideUrl, pycgfxUrl } = request;
  let rom = request.rom;
  request.rom = null;              // (the ROM's only copy in this worker: freed once Python has it)
  progress("python");
  const pyodide = await getPyodide(pyodideUrl);
  const FS = pyodide.FS;
  progress("python", 1);

  // This version's payload, checked against SHA256SUMS.txt and, file by
  // file, against its web-manifest.json.
  progress("release");
  const sums = new TextDecoder().decode(await download(`${releaseUrl}SHA256SUMS.txt`, "SHA256SUMS.txt"));
  const payloadName = "PokePlatinum3DS-WebPayload.zip";
  const payload = await download(`${releaseUrl}${payloadName}`, payloadName);
  const expected = sums.split(/\r?\n/).map((l) => l.split(/\s+/)).find((f) => f[1] === payloadName)?.[0];
  if (!expected || expected !== (await sha256(payload))) throw new BuildError("error.checksum", { what: payloadName });
  progress("release", 0.6);
  try { FS.unlink("/payload.zip"); } catch { /* first build */ }
  FS.writeFile("/payload.zip", payload);
  const bad = pyodide.runPython(`
import hashlib, json, shutil, zipfile
shutil.rmtree("/payload", ignore_errors=True)
with zipfile.ZipFile("/payload.zip") as z:
    z.extractall("/payload")
    listed = json.loads(z.read("web-manifest.json"))["files"]
bad = [n for n, f in listed.items()
       if hashlib.sha256(open("/payload/" + n, "rb").read()).hexdigest() != f["sha256"]]
",".join(bad)
`);
  FS.unlink("/payload.zip");
  if (bad) throw new BuildError("error.checksum", { what: bad });

  // pycgfx (the 3D banner's converter), from its own GitHub at the pinned
  // commit: it has no licence that lets this project share it.
  let pycgfxDir = null;
  let bannerWhy = "";
  if (wantCia && manifest.pycgfx) {
    try {
      const base = pycgfxUrl || manifest.pycgfx.rawBase;
      for (const file of manifest.pycgfx.files) {
        const data = await download(base + file.path, "pycgfx");
        if ((await sha256(data)) !== file.sha256) throw new Error(`pycgfx's ${file.path} isn't the expected file`);
        const path = "/pycgfx/" + file.path;
        mkdirs(FS, path.slice(0, path.lastIndexOf("/")));
        FS.writeFile(path, data);
      }
      pycgfxDir = "/pycgfx";
    } catch (e) {
      bannerWhy = e.detail || e.message;
    }
  }
  progress("release", 1);

  // The data pack and the banner's pieces (builder/web.py).
  progress("rom");
  const entry = manifest.entrypoint;
  pyodide.globals.set("rom_js", new Uint8Array(rom));
  rom = null;
  pyodide.globals.set("report", (stage, done, total) => progress(stage, total ? done / total : 0));
  pyodide.globals.set("pycgfx_dir", pycgfxDir);
  pyodide.globals.set("check_rom", !manifest.synthetic);
  let result;
  try {
    result = pyodide.runPython(`
import json, shutil, sys
for p in ${JSON.stringify(entry.pythonPath.map((p) => "/payload/" + p))}:
    if p not in sys.path:
        sys.path.insert(0, p)
import ${entry.module} as builder
shutil.rmtree("/out", ignore_errors=True)
rom = rom_js.to_bytes()
del rom_js
try:
    r = builder.${entry.function}(rom, "/out", pycgfx_dir=pycgfx_dir, progress=report, verify=check_rom)
finally:
    del rom
json.dumps({"banner_error": r["banner_error"], "files": list(r["files"])})
`);
  } catch (e) {
    const text = String(e.message || e);
    if (/MemoryError|out of memory|Cannot enlarge memory/i.test(text)) throw new BuildError("error.memory", {}, text);
    throw new BuildError("top.error.detail", { message: text.split("\n").filter(Boolean).pop() }, text);
  }
  const made = JSON.parse(result);
  if (made.banner_error) bannerWhy = made.banner_error;
  const out = (name) => "/out/" + name;
  const files = [
    { name: "3ds/pokeplatinum.3dsx", data: FS.readFile("/payload/program/pokeplatinum.3dsx") },
    { name: entry.outputs.pak, data: FS.readFile(out(entry.outputs.pak)) },
    { name: entry.outputs.manifest, data: FS.readFile(out(entry.outputs.manifest)) },
  ];
  FS.unlink(out(entry.outputs.pak));

  // The HOME Menu version.
  let banner = null;
  if (wantCia) {
    progress("banner", 1);
    progress("cia");
    const cia = manifest.cia;
    const read = (name) => FS.readFile("/payload/" + name);
    const cgfxPath = out(entry.outputs.cgfx);
    const made = await makeCia({
      bannertool: await WebAssembly.compile(read(cia.bannertool)),
      makerom: await WebAssembly.compile(read(cia.makerom)),
      elf: read(cia.elf), icon: read(cia.icon), rsf: read(cia.rsf), chime: read(cia.chime),
      cgfx: FS.analyzePath(cgfxPath).exists ? FS.readFile(cgfxPath) : null,
      flatBanner: FS.readFile(out(entry.outputs.flatBanner)),
    });
    banner = made.banner;
    files.push({ name: "cia/pokeplatinum.cia", data: made.cia });
  }
  progress("cia", 1);

  progress("zip");
  files.push({ name: "README.txt", data: new TextEncoder().encode(readmeText(manifest.releaseVersion)) });
  const zip = makeZip(files);
  progress("zip", 1);
  return { zip, fileName: `pokeplatinum-3ds-${manifest.releaseVersion}-sd.zip`, banner,
           bannerWhy: banner === "flat" ? bannerWhy || "?" : "" };
}

self.onmessage = async ({ data }) => {
  if (data.type !== "build") return;
  try {
    post({ type: "done", ...(await build(data)) });
  } catch (e) {
    console.error(e);
    if (e instanceof BuildError) post({ type: "error", key: e.key, values: e.values, detail: e.detail });
    else if (e instanceof RangeError || /memory/i.test(String(e.message))) post({ type: "error", key: "error.memory", values: {}, detail: String(e) });
    else post({ type: "error", key: "top.error.detail", values: { message: String(e.message || e) }, detail: String(e.stack || e) });
  }
};
