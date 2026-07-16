import { LitElement, html, css } from "https://esm.run/lit@3";

class TopGifterObs extends LitElement {
  static properties = { state: { state: true } };
  static styles = css`
    :host { display: block; width: 100%; height: 100%; font-family: system-ui, sans-serif;
            color: #fff; text-shadow: 0 2px 6px rgba(0,0,0,.8); }
    .card { width: 100%; height: 100%; display: flex; align-items: center; gap: 16px;
            background: rgba(0,0,0,.35); border-radius: 16px; padding: 0 18px;
            box-sizing: border-box; }
    img { width: 110px; height: 110px; border-radius: 50%; object-fit: cover;
          border: 3px solid #d8a; background: #333; }
    .lbl { font-size: 18px; letter-spacing: 2px; opacity: .85; }
    .name { font-size: 34px; font-weight: 800; }
    .dia { font-size: 24px; color: #ffd25a; }
  `;
  constructor() { super(); this.state = {}; }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared() {}
  render() {
    const s = this.state || {};
    if (!s.top) return html`<div class="card"><div><div class="lbl">${s.label || ""}</div>
      <div class="name">—</div></div></div>`;
    return html`<div class="card">
      <img src=${s.top.avatar || ""} alt="">
      <div><div class="lbl">${s.label || ""}</div>
        <div class="name">${s.top.name}</div>
        <div class="dia">${s.top.diamonds.toLocaleString()} 💎</div></div>
    </div>`;
  }
}
customElements.define("evo-top-gifter-obs", TopGifterObs);
