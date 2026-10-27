// The page: the language, the ROM check, the build (worker.mjs) and the
// update helper.
//
// The release's files are in release/ next to this page (the site's deploy,
// tools/build_site.py, puts the latest release's there); ?release=URL/,
// ?pyodide=URL/ and ?pycgfx=URL/ point elsewhere (the tests).
import { STRINGS, pickLanguage, translate } from "./i18n.mjs";

const params = new URLSearchParams(location.search);
const RELEASE_URL = params.get("release") || "release/";
const $ = (id) => document.getElementById(id);

let lang = "en";
let manifest = null;
let romFile = null;
let romInfo = null;          // { name } once the ROM checked out
let worker = null;
let zipUrl = null;
let topState = { state: "idle", title: "top.idle.title", detail: "top.idle.detail", values: {}, stage: "" };

const t = (key, values) => translate(lang, key, values);

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
  if (!file) return;
  romFile = file;
  romInfo = null;
  resetDownload();
  $("drop-hint").textContent = file.name;
  $("choose").textContent = t("bottom.change");
  $("build").disabled = true;
  setTop("checking", "top.checking.title");
  const digest = await sha1(await file.arrayBuffer());
  if (romFile !== file) return;            // another was chosen meanwhile
  const known = manifest?.supportedRoms.find((r) => r.sha1 === digest);
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
}

async function startBuild() {
  if (!romFile || !romInfo || !manifest) return;
  $("build").disabled = true;
  $("rom").disabled = true;
  setTop("building", "top.building.title");
  $("meter-fill").style.width = "0%";
  const rom = await romFile.arrayBuffer();
  if (!worker) worker = new Worker(new URL("./worker.mjs", import.meta.url), { type: "module" });
  const pyodideUrl = params.get("pyodide") || `https://cdn.jsdelivr.net/pyodide/v${manifest.pyodide.version}/full/`;
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
      $("rom").disabled = false;
      $("meter-fill").style.width = "100%";
      $("top").dataset.banner = data.banner || "none";
      if (data.banner === "flat") setTop("done", "top.done.title", "top.done.flat", { why: data.bannerWhy });
      else setTop("done", "top.done.title", "top.done.detail");
      link.focus();
    } else if (data.type === "error") {
      console.error(data.detail);
      setTop("error", "top.error.title", data.key, data.values);
      $("build").disabled = false;
      $("rom").disabled = false;
    }
  };
  worker.postMessage({
    type: "build", rom, wantCia: $("want-cia").checked, manifest,
    releaseUrl: new URL(RELEASE_URL, location.href).href, pyodideUrl, pycgfxUrl: params.get("pycgfx"),
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
  answer.innerHTML = t(same ? "update.same" : from && from.dataPack ? "update.changed" : "update.unknown", { version });
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
