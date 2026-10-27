// The page: the language, the ROM check, the build (worker.mjs) and the
// update helper.
//
// The release's files are in release/ next to this page (the site's deploy,
// tools/build_site.py, puts the latest release's there). On this computer
// only (localhost: the tests), ?release=URL/, ?pyodide=URL/ and ?pycgfx=URL/
// point elsewhere; the published page ignores them.
import { STRINGS, pickLanguage, translate } from "./i18n.mjs";

const params = new URLSearchParams(location.search);
const LOCAL = ["localhost", "127.0.0.1", "[::1]"].includes(location.hostname);
const testOption = (name) => (LOCAL ? params.get(name) : null);
const RELEASE_URL = testOption("release") || "release/";
// A Pokémon Platinum ROM is 128 MiB: anything far from it isn't one (and isn't read).
const ROM_SIZE_LIMITS = [16 << 20, 256 << 20];
const $ = (id) => document.getElementById(id);

let lang = "en";
let manifest = null;
let romFile = null;
let romInfo = null;          // { name } once the ROM checked out
let worker = null;
let zipUrl = null;
let topState = { state: "idle", title: "top.idle.title", detail: "top.idle.detail", values: {}, stage: "" };

const t = (key, values) => translate(lang, key, values);
const escapeHtml = (text) => String(text).replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
// For strings that hold markup: the values filled in are text.
const tHtml = (key, values = {}) =>
  t(key, Object.fromEntries(Object.entries(values).map(([k, v]) => [k, escapeHtml(v)])));

function storage(fn) {
  try { return fn(window.localStorage); } catch { return null; }
}

function applyLanguage() {
  document.documentElement.lang = lang;
  document.title = t("page.title");
  for (const el of document.querySelectorAll("[data-i18n]")) el.textContent = t(el.dataset.i18n);
  for (const el of document.querySelectorAll("[data-i18n-html]")) el.innerHTML = t(el.dataset.i18nHtml);
  if (manifest) $("release").textContent = t("hero.release", { version: manifest.releaseVersion });
  else if (!$("release").hidden) $("release").textContent = t("hero.noRelease");
  if (romFile) $("drop-hint").textContent = romFile.name;
  $("choose").textContent = t(romFile ? "bottom.change" : "bottom.choose");
  renderTop();
  renderUpdate();
}

function setTop(state, title, detail = "", values = {}) {
  topState = { ...topState, state, title, detail, values };
  if (state !== "building") topState.stage = "";
  renderTop();
}

function renderTop() {
  const { state, title, detail, values, stage } = topState;
  $("top").dataset.state = state;
  $("top-title").textContent = title ? t(title, values) : "";
  $("top-detail").textContent = detail ? t(detail, values) : "";
  $("top-stage").textContent = stage ? t(`stage.${stage}`) : "";
  $("meter").hidden = !(state === "building" || state === "done");
}

async function sha1(buffer) {
  const digest = await crypto.subtle.digest("SHA-1", buffer);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function chooseRom(file) {
  if (!file || !manifest || building) return;
  romFile = file;
  romInfo = null;
  resetDownload();
  $("drop-hint").textContent = file.name;
  $("choose").textContent = t("bottom.change");
  $("build").disabled = true;
  // (a test release's made-up ROMs are small)
  if ((file.size < ROM_SIZE_LIMITS[0] && !manifest.synthetic) || file.size > ROM_SIZE_LIMITS[1]) {
    setTop("bad", "top.bad.title", "top.size.detail", { size: Math.round(file.size / 1048576) });
    return;
  }
  setTop("checking", "top.checking.title");
  let digest;
  try {
    digest = await sha1(await file.arrayBuffer());
  } catch (e) {
    if (romFile === file) setTop("bad", "top.bad.title", "top.read.detail", { message: String(e.message || e) });
    return;
  }
  if (romFile !== file) return;            // another was chosen meanwhile
  const known = manifest.supportedRoms.find((r) => r.sha1 === digest);
  if (!known) {
    setTop("bad", "top.bad.title", "top.bad.detail", { sha1: digest });
    return;
  }
  romInfo = known;
  setTop("ok", "top.ok.title", "top.ok.detail", { name: known.name });
  $("build").disabled = false;
}

function resetDownload() {
  if (zipUrl) URL.revokeObjectURL(zipUrl);
  zipUrl = null;
  $("download").hidden = true;
  $("build").hidden = false;
  $("build").disabled = !romInfo;
}

// While a build runs, the ROM and the choices stay as they are.
let building = false;
function setBuilding(on) {
  building = on;
  $("rom").disabled = on || !manifest;
  $("want-cia").disabled = on;
  $("build").disabled = on || !romInfo;
}

// A worker for each build, ended after it: Python's memory (the size of a
// ROM or more) goes back to the browser, and a build that ran out of memory
// leaves nothing broken behind.
function endWorker() {
  if (worker) worker.terminate();
  worker = null;
}

function buildFailed(key, values, detail) {
  console.error(detail);
  endWorker();
  setBuilding(false);
  setTop("error", "top.error.title", key, values);
}

async function startBuild() {
  if (!romFile || !romInfo || !manifest || building) return;
  setBuilding(true);
  setTop("building", "top.building.title");
  $("meter-fill").style.width = "0%";
  let rom;
  try {
    rom = await romFile.arrayBuffer();
  } catch (e) {
    buildFailed("top.read.detail", { message: String(e.message || e) }, e);
    return;
  }
  endWorker();
  worker = new Worker(new URL("./worker.mjs", import.meta.url), { type: "module" });
  const pyodideUrl = testOption("pyodide") || `https://cdn.jsdelivr.net/pyodide/v${manifest.pyodide.version}/full/`;
  worker.onerror = (e) => buildFailed("top.error.detail", { message: e.message || "the builder didn't start" }, e);
  worker.onmessage = ({ data }) => {
    if (data.type === "progress") {
      topState.stage = data.stage;
      $("meter-fill").style.width = `${Math.round(data.fraction * 100)}%`;
      renderTop();
    } else if (data.type === "done") {
      zipUrl = URL.createObjectURL(data.zip);
      const link = $("download");
      link.href = zipUrl;
      link.download = data.fileName;
      link.hidden = false;
      $("build").hidden = true;
      endWorker();
      setBuilding(false);
      $("meter-fill").style.width = "100%";
      $("top").dataset.banner = data.banner || "none";
      if (data.banner === "flat") setTop("done", "top.done.title", "top.done.flat", { why: data.bannerWhy });
      else setTop("done", "top.done.title", "top.done.detail");
      link.focus();
    } else if (data.type === "error") {
      buildFailed(data.key, data.values, data.detail);
    }
  };
  worker.postMessage({
    type: "build", rom, wantCia: $("want-cia").checked,
    releaseUrl: new URL(RELEASE_URL, location.href).href, pyodideUrl, pycgfxUrl: testOption("pycgfx"),
  }, [rom]);
}

// Updating: is the player's data pack still good for this version?
let releases = [];
function renderUpdate() {
  const select = $("update-from");
  const answer = $("update-answer");
  const download = $("update-download");
  if (!manifest || releases.length < 2) return;
  if (!select.options.length) {
    for (const r of releases) {
      if (r.tag === manifest.releaseTag) continue;
      const option = document.createElement("option");
      option.value = r.tag;
      option.textContent = r.tag;
      select.append(option);
    }
  }
  const from = releases.find((r) => r.tag === select.value);
  const same = from && from.dataPack && from.dataPack.format === manifest.dataPack.format &&
    from.dataPack.buildId === manifest.dataPack.buildId;
  const version = manifest.releaseTag;
  answer.innerHTML = tHtml(same ? "update.same" : from && from.dataPack ? "update.changed" : "update.unknown", { version });
  download.hidden = !same;
  download.href = `${RELEASE_URL}pokeplatinum.3dsx`;
  download.setAttribute("download", "pokeplatinum.3dsx");
}

async function loadRelease() {
  try {
    const response = await fetch(`${RELEASE_URL}web-manifest.json`, { cache: "no-cache" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    manifest = await response.json();
    $("release").hidden = false;
    $("rom").disabled = false;               // (a ROM is checked against the manifest's list)
  } catch {
    manifest = null;
    $("release").hidden = false;
    $("release").textContent = t("hero.noRelease");
    $("rom").disabled = true;
    return;
  }
  try {
    const response = await fetch(`${RELEASE_URL}releases.json`, { cache: "no-cache" });
    if (response.ok) releases = (await response.json()).releases || [];
  } catch { /* no update helper */ }
  $("update-section").hidden = releases.filter((r) => r.tag !== manifest.releaseTag).length === 0;
}

function init() {
  lang = pickLanguage(params.get("lang") || storage((s) => s.getItem("lang")), navigator.languages);
  $("lang").addEventListener("click", () => {
    lang = Object.keys(STRINGS).find((l) => l !== lang);
    storage((s) => s.setItem("lang", lang));
    applyLanguage();
  });
  $("rom").addEventListener("change", (e) => chooseRom(e.target.files[0]));
  const drop = $("drop");
  drop.addEventListener("dragover", (e) => { e.preventDefault(); drop.classList.add("over"); });
  drop.addEventListener("dragleave", () => drop.classList.remove("over"));
  drop.addEventListener("drop", (e) => {
    e.preventDefault();
    drop.classList.remove("over");
    if (!$("rom").disabled) chooseRom(e.dataTransfer.files[0]);
  });
  $("build").addEventListener("click", startBuild);
  $("want-cia").addEventListener("change", resetDownload);
  $("update-from").addEventListener("change", renderUpdate);
  applyLanguage();
  loadRelease().then(applyLanguage);
}

init();
