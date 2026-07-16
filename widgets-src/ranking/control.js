import { LitElement, html, css } from "https://esm.run/lit@3";

// Ranking control: choose per-scene vs per-shift scope (§8.2) and row count.
class RankingControl extends LitElement {
  static properties = { config: {}, state: { state: true } };
  static styles = css`
    :host { display: block; font-family: system-ui, sans-serif; color: #ddd; }
    label { display: block; margin: .5em 0 .1em; color: #aaa; font-size: 12px; }
    select, input { width: 100%; padding: .3em; background: #1c1c24; color: #eee;
                    border: 1px solid #33333f; border-radius: 4px; }
    .row { display: flex; justify-content: space-between; padding: 2px 0; }
  `;
  constructor() { super(); this.config = {}; this.state = {}; }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared() {}
  _upd(k, v) {
    this.config = { ...this.config, [k]: v };
    if (this.updateConfig) this.updateConfig(this.config);
    this.requestUpdate();
  }
  render() {
    const c = this.config || {};
    const rows = (this.state && this.state.rows) || [];
    return html`
      <label>Scope (takes effect on next scene load)</label>
      <select @change=${(e) => this._upd("scope", e.target.value)}>
        <option value="scene_run" ?selected=${c.scope !== "shift"}>Per scene</option>
        <option value="shift" ?selected=${c.scope === "shift"}>Per shift</option>
      </select>
      <label>Rows</label>
      <input type="number" .value=${c.limit || 8} @input=${(e) => this._upd("limit", +e.target.value)}>
      <label>Live</label>
      ${rows.map((r, i) => html`<div class="row"><span>${i + 1}. ${r.name}</span><span>${r.diamonds}</span></div>`)}`;
  }
}
customElements.define("evo-ranking-control", RankingControl);
