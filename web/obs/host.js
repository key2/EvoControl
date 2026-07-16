// EvoControl OBS host (§8.3). Read-only renderer: instantiate one Lit element
// per placed widget instance on the 1080x1920 canvas, wire WS state to each.

import { EvoWs, api } from "/shared/ws.js";

const stage = document.getElementById("stage");

// Scale the fixed canvas to fill the browser source, preserving 9:16.
function fitStage() {
  const sx = window.innerWidth / 1080;
  const sy = window.innerHeight / 1920;
  const s = Math.min(sx, sy);
  stage.style.transform = `scale(${s})`;
}
window.addEventListener("resize", fitStage);
fitStage();

const instances = new Map(); // instanceId -> element

async function build() {
  stage.innerHTML = "";
  instances.clear();
  let widgets = [];
  try {
    widgets = await api("GET", "/api/scenes/current/widgets");
  } catch { widgets = []; }
  if (!Array.isArray(widgets)) widgets = [];

  for (const w of widgets) {
    const reg = await manifestFor(w.type);
    const ver = reg ? reg.version : "";
    try {
      await import(`/widgets/${w.type}/${ver}/obs.js`);
    } catch (e) {
      console.warn("failed to load obs.js for", w.type, e);
      continue;
    }
    const tag = `evo-${w.type}-obs`;
    const el = document.createElement(tag);
    el.setAttribute("instance-id", w.id);
    el.instanceId = w.id;
    el.config = w.config || {};
    el.style.left = w.x + "px";
    el.style.top = w.y + "px";
    el.style.width = w.w + "px";
    el.style.height = w.h + "px";
    el.style.zIndex = w.z || 0;
    stage.appendChild(el);
    instances.set(w.id, el);
  }
}

const _registry = new Map();
async function manifestFor(type) {
  if (_registry.size === 0) {
    try {
      const reg = await api("GET", "/api/widget-registry");
      for (const r of reg) _registry.set(r.type, r);
    } catch {}
  }
  return _registry.get(type);
}

const ws = new EvoWs("obs");

ws.addEventListener("state", (ev) => {
  const { instance, data } = ev.detail;
  const el = instances.get(instance);
  if (el && typeof el.applyState === "function") el.applyState(data);
});
ws.addEventListener("shared", (ev) => {
  for (const el of instances.values())
    if (typeof el.applyShared === "function") el.applyShared(ev.detail);
});
ws.addEventListener("snapshot", (ev) => {
  const snap = ev.detail;
  if (snap && Array.isArray(snap.instances)) {
    for (const s of snap.instances) {
      const el = instances.get(s.instance);
      if (el && typeof el.applyState === "function" && s.state)
        el.applyState(s.state);
    }
  }
});

build();
