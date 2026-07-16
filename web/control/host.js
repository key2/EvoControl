// EvoControl Control host (§6.2/§8.3/§14). Single active session with takeover;
// fixed chrome (toolbar, palette, selected-widget panel); canvas editor with
// drag/move/z + Save; each placed widget's control.js UI in the panel.

import { EvoWs, api } from "/shared/ws.js";

const SCALE = 405 / 1080; // canvas mirror scale
const canvas = document.getElementById("canvas");
const palette = document.getElementById("palette");
const toolbar = document.getElementById("toolbar");
const selPanel = document.getElementById("selPanel");
const lost = document.getElementById("lost");
const auth = localStorage.getItem("evo_control_auth") || "";

let registry = [];
let widgets = [];       // current scene composition (editable)
let sceneId = 0;        // scene under edit (from current run)
let selected = -1;      // index into widgets
let locked = false;     // lock widget displacement
const instEls = new Map(); // instanceId -> control element

const ws = new EvoWs("control", auth);
ws.addEventListener("superseded", () => showLost("A new control session was opened."));
ws.addEventListener("active", () => (lost.className = ""));
ws.addEventListener("unauthorized", () => {
  const pw = prompt("Control view password:");
  if (pw != null) { localStorage.setItem("evo_control_auth", pw); location.reload(); }
});
ws.addEventListener("snapshot", (ev) => onSnapshot(ev.detail));
ws.addEventListener("state", (ev) => {
  const el = instEls.get(ev.detail.instance);
  if (el && typeof el.applyState === "function") el.applyState(ev.detail.data);
});
ws.addEventListener("shared", (ev) => {
  for (const el of instEls.values())
    if (typeof el.applyShared === "function") el.applyShared(ev.detail);
});

document.getElementById("takeback").onclick = () => { ws.takeback(); lost.className = ""; };
function showLost(msg) {
  document.getElementById("lostMsg").textContent = msg;
  lost.className = "show";
}

// --- fixed chrome: toolbar (§8.3) ---
function renderToolbar() {
  toolbar.innerHTML = "";
  const lockBtn = mkBtn(locked ? "Unlock" : "Lock", () => { locked = !locked; renderToolbar(); render(); });
  const delBtn = mkBtn("Delete", () => {
    if (selected >= 0) { widgets.splice(selected, 1); selected = -1; render(); renderSel(); }
  });
  const saveBtn = mkBtn("Save scene", saveScene);
  saveBtn.classList.add("on");
  toolbar.append(lockBtn, delBtn, saveBtn);
}
function mkBtn(label, fn) { const b = document.createElement("button"); b.textContent = label; b.onclick = fn; return b; }

async function loadRegistry() {
  try { registry = await api("GET", "/api/widget-registry"); } catch { registry = []; }
  palette.innerHTML = "";
  for (const r of registry) {
    const p = document.createElement("div");
    p.className = "p"; p.draggable = true; p.title = r.description || r.name;
    p.innerHTML = `<img src="${r.icon}" alt=""><div>${r.name}</div>`;
    p.ondragstart = (e) => e.dataTransfer.setData("text/plain", r.type);
    palette.appendChild(p);
  }
}

// Drop a palette widget onto the canvas.
canvas.addEventListener("dragover", (e) => e.preventDefault());
canvas.addEventListener("drop", (e) => {
  e.preventDefault();
  const type = e.dataTransfer.getData("text/plain");
  const r = registry.find((x) => x.type === type);
  if (!r) return;
  const rect = canvas.getBoundingClientRect();
  const x = (e.clientX - rect.left) / SCALE;
  const y = (e.clientY - rect.top) / SCALE;
  widgets.push({
    id: -(Date.now()), type, x, y,
    w: (r.defaultSize && r.defaultSize.w) || 400,
    h: (r.defaultSize && r.defaultSize.h) || 300,
    z: widgets.length, config: {},
  });
  selected = widgets.length - 1;
  render(); renderSel();
});

function render() {
  canvas.innerHTML = "";
  instEls.clear();
  widgets.forEach((w, i) => {
    const d = document.createElement("div");
    d.className = "inst" + (i === selected ? " sel" : "");
    d.style.left = w.x * SCALE + "px";
    d.style.top = w.y * SCALE + "px";
    d.style.width = w.w * SCALE + "px";
    d.style.height = w.h * SCALE + "px";
    d.style.zIndex = w.z || 0;
    d.onmousedown = (e) => beginDrag(e, i, d);
    canvas.appendChild(d);
  });
}

function beginDrag(e, i, d) {
  selected = i; renderSel(); render();
  if (locked) return;
  const w = widgets[i];
  const startX = e.clientX, startY = e.clientY;
  const ox = w.x, oy = w.y;
  const move = (ev) => {
    w.x = ox + (ev.clientX - startX) / SCALE;
    w.y = oy + (ev.clientY - startY) / SCALE;
    render();
  };
  const up = () => { document.removeEventListener("mousemove", move); document.removeEventListener("mouseup", up); };
  document.addEventListener("mousemove", move);
  document.addEventListener("mouseup", up);
}

// Selected-widget panel: load the widget's control.js element (§8.3).
async function renderSel() {
  selPanel.innerHTML = "";
  if (selected < 0) { selPanel.innerHTML = "<i>Select a widget on the canvas.</i>"; return; }
  const w = widgets[selected];
  const r = registry.find((x) => x.type === w.type);
  const ver = r ? r.version : "";
  const info = document.createElement("div");
  info.innerHTML = `<b>${w.type}</b>`;
  selPanel.appendChild(info);
  try {
    await import(`/widgets/${w.type}/${ver}/control.js`);
    const el = document.createElement(`evo-${w.type}-control`);
    el.instanceId = w.id;
    el.config = w.config || {};
    // control.js sends private intents via this callback.
    el.sendIntent = (data) => ws.intent(w.id, data);
    el.updateConfig = (cfg) => { w.config = cfg; };
    selPanel.appendChild(el);
    instEls.set(w.id, el);
  } catch (e) {
    const p = document.createElement("p");
    p.textContent = "This widget has no control UI (or it failed to load).";
    selPanel.appendChild(p);
  }
}

async function saveScene() {
  if (!sceneId) { alert("No scene loaded to save."); return; }
  const payload = widgets.map((w) => ({
    type: w.type, x: w.x, y: w.y, w: w.w, h: w.h, z: w.z, config: w.config || {},
  }));
  await api("PUT", `/api/scenes/${sceneId}/widgets`, payload, auth);
}

async function onSnapshot(snap) {
  // Determine the current scene under edit from the active run.
  try {
    const cur = await api("GET", "/api/scenes/current");
    sceneId = cur && cur.sceneId ? cur.sceneId : 0;
  } catch { sceneId = 0; }
  try {
    widgets = await api("GET", "/api/scenes/current/widgets");
  } catch { widgets = []; }
  if (!Array.isArray(widgets)) widgets = [];
  render();
  renderSel();
  // Apply snapshot state to loaded control elements.
  if (snap && Array.isArray(snap.instances)) {
    for (const s of snap.instances) {
      const el = instEls.get(s.instance);
      if (el && typeof el.applyState === "function" && s.state) el.applyState(s.state);
    }
  }
}

renderToolbar();
loadRegistry();
onSnapshot(null);
