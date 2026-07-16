import { LitElement, html, css } from "https://esm.run/lit@3";
import { api } from "/shared/ws.js";

// Control UI for the score battle (§6.4): drag gifts onto sides, assign
// performers to sides (drag or Detect Faces), see the live score. Every action
// is a private intent to app.js which confirms (non-optimistic, §6.1).
class BattleControl extends LitElement {
  static properties = { config: {}, state: { state: true },
                        performers: { state: true }, gallery: { state: true } };
  static styles = css`
    :host { display: block; font-family: system-ui, sans-serif; color: #ddd; }
    .sides { display: flex; gap: .5em; margin: .5em 0; }
    .side { flex: 1; border: 1px solid #33333f; border-radius: 6px; padding: .4em;
            min-height: 60px; }
    .side h4 { margin: 0 0 .3em; }
    .side.drop { outline: 2px dashed #6cf; }
    .chip { background: #22222b; border: 1px solid #33333f; border-radius: 12px;
            padding: .1em .5em; margin: .1em; display: inline-block; font-size: 12px; }
    .gallery { display: flex; flex-wrap: wrap; gap: .3em; max-height: 140px;
               overflow: auto; }
    .gallery .g { width: 54px; text-align: center; cursor: grab; font-size: 10px; }
    .gallery .g img { width: 40px; height: 40px; object-fit: contain; background:#000;
                      border-radius: 6px; }
    .perf { display: flex; flex-wrap: wrap; gap: .3em; margin: .3em 0; }
    .perf .p { cursor: grab; }
    button { background: #2b4; color: #041; border: 0; border-radius: 4px;
             padding: .3em .7em; cursor: pointer; margin-top: .4em; }
    .score { font-weight: 700; margin-top: .4em; }
  `;
  constructor() {
    super();
    this.state = {}; this.performers = []; this.gallery = [];
    this._load();
  }
  async _load() {
    try { this.performers = await api("GET", "/api/scenes/current"); } catch {}
    try {
      const perf = await api("GET", "/api/gift-gallery");
      this.gallery = Array.isArray(perf) ? perf : [];
    } catch {}
    // performers come from the snapshot shared plane; fall back to REST.
    this.requestUpdate();
  }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared(sh) {
    if (sh && sh.topic === "performers" && Array.isArray(sh.performers)) {
      this.performers = sh.performers; this.requestUpdate();
    }
    if (sh && sh.topic === "detect-faces") this._detect();
  }
  _assign(side, performerId) {
    this.sendIntent && this.sendIntent({ assign: true, side, performerId });
  }
  _route(side, giftId) {
    this.sendIntent && this.sendIntent({ route: true, side, giftId });
  }
  async _detect() {
    let faces = [];
    try { faces = await api("GET", "/api/faces"); } catch {}
    this.sendIntent && this.sendIntent({ detectFaces: true, faces });
  }
  _dropSide(e, side) {
    e.preventDefault();
    const gid = e.dataTransfer.getData("gift");
    const pid = e.dataTransfer.getData("perf");
    if (gid) this._route(side, +gid);
    if (pid) this._assign(side, +pid);
    e.currentTarget.classList.remove("drop");
  }
  render() {
    const s = this.state || {};
    const nameOf = (id) => { const p = (this.performers || []).find((x) => x.id === id); return p ? p.name : "—"; };
    return html`
      <div class="perf">Performers:
        ${(this.performers || []).map((p) => html`
          <span class="chip p" draggable="true"
            @dragstart=${(e) => e.dataTransfer.setData("perf", p.id)}>${p.name}</span>`)}
      </div>
      <div class="sides">
        ${["left", "right"].map((side) => html`
          <div class="side" @dragover=${(e) => { e.preventDefault(); e.currentTarget.classList.add("drop"); }}
               @dragleave=${(e) => e.currentTarget.classList.remove("drop")}
               @drop=${(e) => this._dropSide(e, side)}>
            <h4>${side === "left" ? "Left" : "Right"}</h4>
            <div>${nameOf(side === "left" ? s.left : s.right)}</div>
          </div>`)}
      </div>
      <button @click=${() => this._detect()}>Detect Faces</button>
      <div>Gift routing (drag a gift onto a side):</div>
      <div class="gallery">
        ${(this.gallery || []).map((g) => html`
          <div class="g" draggable="true"
               @dragstart=${(e) => e.dataTransfer.setData("gift", g.giftId)}>
            <img src=${g.iconUrl || ""} alt=""><div>${g.name}</div>
          </div>`)}
      </div>
      <div class="score">Round ${(s.round && s.round.left) || 0} : ${(s.round && s.round.right) || 0}
        &nbsp; Wins ${(s.wins && s.wins.left) || 0} | ${(s.wins && s.wins.right) || 0}</div>`;
  }
}
customElements.define("evo-score-battle-control", BattleControl);
