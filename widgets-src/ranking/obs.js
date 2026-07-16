import { LitElement, html, css } from "https://esm.run/lit@3";

class RankingObs extends LitElement {
  static properties = { state: { state: true } };
  static styles = css`
    :host { display: block; width: 100%; height: 100%; font-family: system-ui, sans-serif; }
    .box { width: 100%; height: 100%; background: rgba(0,0,0,.35);
           border-radius: 12px; padding: 8px; color: #fff; box-sizing: border-box; }
    .row { display: flex; align-items: center; gap: 8px; padding: 3px 4px;
           font-weight: 700; }
    .rank { width: 20px; color: #ffd25a; }
    img { width: 34px; height: 34px; border-radius: 50%; object-fit: cover; background:#333; }
    .name { flex: 1; }
    .dia { color: #7be0ff; }
  `;
  constructor() { super(); this.state = {}; }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared() {}
  render() {
    const rows = (this.state && this.state.rows) || [];
    return html`<div class="box">
      ${rows.map((r, i) => html`
        <div class="row">
          <span class="rank">${i + 1}</span>
          <img src=${r.avatar || ""} alt="">
          <span class="name">${r.name}</span>
          <span class="dia">${r.diamonds.toLocaleString()}</span>
        </div>`)}
    </div>`;
  }
}
customElements.define("evo-ranking-obs", RankingObs);
