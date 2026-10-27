// The browser's .cia (web/js/cia.mjs: bannertool and makerom compiled for
// WASI, run by web/js/wasi.mjs) against the native tools
// (tools/build_cia_tools.sh), on the same inputs:
//
//   node tools/test_cia_wasm.mjs --wasm DIR --native DIR --elf ELF --icon SMDH \
//       --rsf RSF --chime WAV --flat PNG [--cgfx CGFX]
//
// The banners and the title (.cxi) must be the same bytes. A .cia has a
// random ticket and content ID (makerom seeds its random numbers with the
// clock), so of the .cia: the same size, holding the same title.
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const { makeBanner, makeCia } = await import(pathToFileURL(path.join(here, "..", "web", "js", "cia.mjs")).href);

const opts = {};
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i += 2) opts[argv[i].replace(/^--/, "")] = argv[i + 1];
for (const need of ["wasm", "native", "elf", "icon", "rsf", "chime", "flat"]) {
  if (!opts[need]) {
    console.error(`missing --${need}`);
    process.exit(2);
  }
}
const read = (p) => new Uint8Array(readFileSync(p));
const module = async (name) => WebAssembly.compile(readFileSync(path.join(opts.wasm, `${name}.wasm`)));
const tools = { bannertool: await module("bannertool"), makerom: await module("makerom") };
const inputs = { elf: read(opts.elf), icon: read(opts.icon), rsf: read(opts.rsf), chime: read(opts.chime),
                 flatBanner: read(opts.flat) };
const cgfx = opts.cgfx ? read(opts.cgfx) : null;

const work = mkdtempSync(path.join(tmpdir(), "cia-wasm-"));
const native = (tool, args) => execFileSync(path.join(opts.native, tool), args, { cwd: work, stdio: "pipe" });
const same = (a, b) => a.length === b.length && Buffer.compare(Buffer.from(a), Buffer.from(b)) === 0;
let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failures++;
};

try {
  writeFileSync(path.join(work, "chime.wav"), inputs.chime);
  writeFileSync(path.join(work, "flat.png"), inputs.flatBanner);
  writeFileSync(path.join(work, "in.elf"), inputs.elf);
  writeFileSync(path.join(work, "in.smdh"), inputs.icon);
  writeFileSync(path.join(work, "in.rsf"), inputs.rsf);

  const banners = [["flat", null]];
  if (cgfx) {
    writeFileSync(path.join(work, "in.cgfx"), cgfx);
    banners.push(["3d", cgfx]);
  }
  for (const [kind, scene] of banners) {
    native("bannertool", ["makebanner", ...(scene ? ["-ci", "in.cgfx"] : ["-i", "flat.png"]),
                          "-a", "chime.wav", "-o", `${kind}.bnr`]);
    const nativeBnr = read(path.join(work, `${kind}.bnr`));
    const web = await makeBanner({ bannertool: tools.bannertool, chime: inputs.chime, cgfx: scene,
                                   flatBanner: inputs.flatBanner });
    check(web.banner === kind && same(web.bnr, nativeBnr), `${kind} banner: the same bytes (${nativeBnr.length})`);

    const title = ["-target", "t", "-exefslogo", "-rsf", "in.rsf", "-elf", "in.elf", "-icon", "in.smdh",
                   "-banner", `${kind}.bnr`];
    native("makerom", ["-f", "cxi", "-o", `${kind}.cxi`, ...title]);
    native("makerom", ["-f", "cia", "-o", `${kind}.cia`, ...title]);
    const nativeCxi = read(path.join(work, `${kind}.cxi`));
    const cxi = await makeCia({ ...tools, ...inputs, cgfx: scene, format: "cxi" });
    check(same(cxi.cia, nativeCxi), `${kind} banner's title (.cxi): the same bytes (${nativeCxi.length})`);
    const cia = await makeCia({ ...tools, ...inputs, cgfx: scene });
    check(cia.cia.length === statSync(path.join(work, `${kind}.cia`)).size, `${kind} banner's .cia: the same size (${cia.cia.length})`);
    check(Buffer.from(cia.cia).indexOf(Buffer.from(nativeCxi)) > 0, `${kind} banner's .cia holds the same title`);
  }
  // A scene bannertool refuses: the flat banner instead.
  const refused = await makeBanner({ bannertool: tools.bannertool, chime: inputs.chime,
                                     cgfx: new Uint8Array(16), flatBanner: inputs.flatBanner });
  check(refused.banner === "flat", "a broken 3D scene: the flat banner instead");
} finally {
  rmSync(work, { recursive: true, force: true });
}
console.log(failures ? `${failures} failed` : "all the same");
process.exit(failures ? 1 : 0);
