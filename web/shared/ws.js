// EvoControl WS envelope client (§11.2). One connection per page.
//
// Envelope: { v:1, scope, instance, topic, seq, data }
// Topics (server → page): session, snapshot, state, shared, navigate.
// Topics (page → server): hello, takeback, intent.
//
// The client dispatches per-instance `state`, global `shared`, `snapshot`,
// `navigate`, and `session` to registered listeners. It also drops stale
// per-instance `state` updates using the monotonic `seq`.

export class EvoWs extends EventTarget {
  constructor(scope, auth = "") {
    super();
    this.scope = scope;
    this.auth = auth;
    this.sessionId = 0;
    this.superseded = false;
    this._seq = new Map(); // instance -> last seq seen
    this._connect();
  }

  _connect() {
    const proto = location.protocol === "https:" ? "wss" : "ws";
    this.ws = new WebSocket(`${proto}://${location.host}/ws`);
    this.ws.onopen = () => {
      this.send({ topic: "hello", data: { scope: this.scope, auth: this.auth } });
    };
    this.ws.onmessage = (ev) => this._onMessage(ev.data);
    this.ws.onclose = () => {
      // Reconnect after a short delay (seq guards stale broadcasts).
      setTimeout(() => this._connect(), 1000);
    };
  }

  send(obj) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN)
      this.ws.send(JSON.stringify({ v: 1, scope: this.scope, ...obj }));
  }

  // control.js → app.js private intent for one instance.
  intent(instance, data) {
    this.send({ topic: "intent", instance, data });
  }

  takeback() {
    this.superseded = false;
    this.send({ topic: "takeback" });
  }

  _onMessage(text) {
    let m;
    try { m = JSON.parse(text); } catch { return; }
    if (!m || !m.topic) return;
    const { topic, instance, data, seq } = m;

    if (topic === "session") {
      if (data && data.id) this.sessionId = data.id;
      if (data && data.status === "superseded") {
        this.superseded = true;
        this.dispatchEvent(new CustomEvent("superseded"));
      } else if (data && data.status === "active") {
        this.superseded = false;
        this.dispatchEvent(new CustomEvent("active"));
      } else if (data && data.status === "unauthorized") {
        this.dispatchEvent(new CustomEvent("unauthorized"));
      }
      return;
    }
    if (topic === "navigate") {
      // Fresh HTML load picks up new widget bundles (§14).
      const url = this.scope === "obs" ? data.obs : data.control;
      if (url) location.href = url;
      return;
    }
    if (topic === "snapshot") {
      this.dispatchEvent(new CustomEvent("snapshot", { detail: data }));
      return;
    }
    if (topic === "shared") {
      this.dispatchEvent(new CustomEvent("shared", { detail: data }));
      return;
    }
    if (topic === "state") {
      // Drop stale per-instance updates after reconnects.
      const s = data && data.seq != null ? data.seq : seq;
      if (instance != null && s != null) {
        const prev = this._seq.get(instance) || 0;
        if (s < prev) return;
        this._seq.set(instance, s);
      }
      this.dispatchEvent(
        new CustomEvent("state", { detail: { instance, data: data && data.state ? data.state : data } })
      );
      return;
    }
  }
}

// REST helper with the optional control-view auth header.
export async function api(method, path, body, auth) {
  const headers = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  if (auth) headers["X-Evo-Auth"] = auth;
  const res = await fetch(path, {
    method,
    headers,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  const ct = res.headers.get("content-type") || "";
  return ct.includes("application/json") ? res.json() : res.text();
}
