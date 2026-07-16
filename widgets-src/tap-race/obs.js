import { LitElement, html, css } from "https://esm.run/lit@3";

class TapRaceObs extends LitElement {
  static properties = { state: { state: true }, performers: { state: true } };
  static styles = css`
    :host { display: block; width: 100%; height: 100%; font-family: system-ui, sans-serif;
            color: #fff; text-shadow: 0 2px 6px rgba(0,0,0,.8); }
    .lane { position: relative; height: 46%; margin: 2% 0; background: rgba(0,0,0,.35);
            border-radius: 12px; overflow: hidden; }
    .fill { position: absolute; inset: 0; width: 0; transition: width .4s ease; }
    .a .fill { background: linear-gradient(90deg,#4bd6e5,#2aa7c4); }
    .b .fill { background: linear-gradient(90deg,#ff7ab6,#e5468f); }
    .name { position: absolute; left: 12px; top: 50%; transform: translateY(-50%);
            font-weight: 800; font-size: 28px; }
    .val { position: absolute; right: 12px; top: 50%; transform: translateY(-50%);
           font-weight: 800; font-size: 24px; }
    .win { position: absolute; inset: 0; display: flex; align-items: center;
           justify-content: center; font-size: 40px; font-weight: 900; }
  `;
  constructor() { super(); this.state = {}; this.performers = []; }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared(sh) { if (sh && sh.topic === "performers") { this.performers = sh.performers || []; this.requestUpdate(); } }
  _name(id) { const p = (this.performers || []).find((x) => x.id === id); return p ? p.name : "—"; }
  _lane(side, s) {
    const val = (s.progress && s.progress[side]) || 0;
    const pct = Math.min(100, (val / (s.finish || 1)) * 100);
    const id = side === "a" ? s.a : s.b;
    return html`<div class="lane ${side}">
      <div class="fill" style="width:${pct}%"></div>
      <div class="name">${this._name(id)}${s.active === side ? " ●" : ""}</div>
      <div class="val">${val}</div>
      ${s.winner === side ? html`<div class="win">WINNER 🏆</div>` : ""}
    </div>`;
  }
  render() {
    const s = this.state || {};
    return html`${this._lane("a", s)}${this._lane("b", s)}`;
  }
}
customElements.define("evo-tap-race-obs", TapRaceObs);
