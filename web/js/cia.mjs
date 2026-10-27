// The HOME Menu version (.cia), made in the browser as `make cia` makes it
// (3ds_port/game/Makefile): bannertool makes the banner, makerom the .cia,
// both compiled for WASI (tools/build_cia_tools_wasm.sh) and run by wasi.mjs.
//
//   const { cia, banner, log } = await makeCia({
//       bannertool, makerom,            // compiled WebAssembly.Modules
//       elf, icon, rsf, chime,          // the release's (web-manifest.json "cia")
//       cgfx,                           // the 3D banner's scene (builder/web.py), or null
//       flatBanner,                     // its fallback: the flat banner's PNG (builder/web.py)
//   });
//
// banner says which banner it got: "3d", or "flat" when there was no scene,
// it wasn't a CGFX file, or bannertool refused it. Everything stays in memory.
import { runWasi } from "./wasi.mjs";

async function run(module, args, files) {
  const result = await runWasi(module, args, files);
  const log = `$ ${args.join(" ")}\n${result.stdout}${result.stderr}`;
  return { ...result, log };
}

// A CGFX file, within the HOME Menu's limit for a banner's (as tools/banner3d/to_cgfx.py checks):
// bannertool wraps whatever it's given.
const CGFX_LIMIT = 0x80000;
const isCgfx = (data) => data && data.length >= 0x14 && data.length <= CGFX_LIMIT &&
  String.fromCharCode(...data.subarray(0, 4)) === "CGFX";

export async function makeBanner({ bannertool, chime, cgfx, flatBanner }) {
  const logs = [];
  if (cgfx && !isCgfx(cgfx)) logs.push("(the 3D banner's scene isn't a CGFX file within 512 KB: the flat banner instead)\n");
  if (isCgfx(cgfx)) {
    const r = await run(bannertool, ["bannertool", "makebanner", "-ci", "/banner.cgfx", "-a", "/chime.wav", "-o", "/banner.bnr"],
                        { "/banner.cgfx": cgfx, "/chime.wav": chime });
    logs.push(r.log);
    if (r.code === 0 && r.files.has("/banner.bnr")) return { bnr: r.files.get("/banner.bnr"), banner: "3d", log: logs.join("") };
  }
  const r = await run(bannertool, ["bannertool", "makebanner", "-i", "/banner.png", "-a", "/chime.wav", "-o", "/banner.bnr"],
                      { "/banner.png": flatBanner, "/chime.wav": chime });
  logs.push(r.log);
  if (r.code !== 0 || !r.files.has("/banner.bnr")) throw new Error(`bannertool failed:\n${logs.join("")}`);
  return { bnr: r.files.get("/banner.bnr"), banner: "flat", log: logs.join("") };
}

export async function makeCia({ bannertool, makerom, elf, icon, rsf, chime, cgfx, flatBanner, format = "cia" }) {
  const { bnr, banner, log } = await makeBanner({ bannertool, chime, cgfx, flatBanner });
  const out = `/pokeplatinum.${format}`;
  const r = await run(makerom, ["makerom", "-f", format, "-o", out, "-target", "t", "-exefslogo",
                                "-rsf", "/pokeplatinum.rsf", "-elf", "/pokeplatinum.elf",
                                "-icon", "/pokeplatinum.smdh", "-banner", "/pokeplatinum.bnr"],
                      { "/pokeplatinum.rsf": rsf, "/pokeplatinum.elf": elf, "/pokeplatinum.smdh": icon,
                        "/pokeplatinum.bnr": bnr });
  if (r.code !== 0 || !r.files.has(out)) throw new Error(`makerom failed:\n${log}${r.log}`);
  return { cia: r.files.get(out), bnr, banner, log: log + r.log };
}
