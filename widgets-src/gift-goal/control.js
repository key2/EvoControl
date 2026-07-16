import { LitElement, html, css } from "https://esm.run/lit@3";

class GoalControl extends LitElement {
  static properties = { config: {}, state: { state: true }, performers: { state: true } };
  static styles = css`
    :host { display: block; font-family: system-ui, sans-serif; color: #ddd; }
    label { display: block; margin: .5em 0 .1em; color: #aaa; font-size: 12px; }
    input, select { width: 100%; padding: .3em; background: #1c1c24; color: #eee;
                    border: 1px solid #33333f; border-radius: 4px; }
    button { margin-top: .5em; background: #2b4; color: #041; border: 0;
             border-radius: 4px; padding: .3em .7em; cursor: pointer; }
    .p { margin-top: .4em; }
  `;
  constructor() { super(); this.config = {}; this.state = {}; this.performers = []; }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared(sh) {
    if (sh && sh.topic === "performers") { this.performers = sh.performers || []; this.requestUpdate(); }
  }
  _upd(k, v) {
    this.config = { ...this.config, [k]: v };
    if (this.updateConfig) this.updateConfig(this.config);
    this.requestUpdate();
  }
  render() {
    const c = this.config || {}, s = this.state || {};
    return html`
      <label>Title</label>
      <input .value=${c.title || "Goal"} @input=${(e) => this._upd("title", e.target.value)}>
      <label>Diamond goal</label>
      <input type="number" .value=${c.goal || 5000}
        @input=${(e) => { this._upd("goal", +e.target.value); this.sendIntent && this.sendIntent({ set: { goal: +e.target.value } }); }}>
      <div class="p">Beneficiary:
        <select @change=${(e) => this.sendIntent && this.sendIntent({ assign: true, performerId: +e.target.value || null })}>
          <option value="">(show coins — nobody)</option>
          ${(this.performers || []).map((p) => html`<option value=${p.id} ?selected=${s.beneficiary === p.id}>${p.name}</option>`)}
        </select>
      </div>
      <button @click=${() => this.sendIntent && this.sendIntent({ reset: true })}>Reset progress</button>
      <div class="p">Progress: ${(s.total || 0)} / ${(s.goal || 0)} (${s.pct || 0}%)</div>`;
  }
}
customElements.define("evo-gift-goal-control", GoalControl);
