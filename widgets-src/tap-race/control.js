import { LitElement, html, css } from "https://esm.run/lit@3";

// Tap Race control: assign A/B performers, pick the active side (which gets the
// next gifts), reset. Zero gift-routing config — great for a quick live test.
class TapRaceControl extends LitElement {
  static properties = { config: {}, state: { state: true }, performers: { state: true } };
  static styles = css`
    :host { display: block; font-family: system-ui, sans-serif; color: #ddd; }
    .row { display: flex; align-items: center; gap: .5em; margin: .4em 0; }
    select { flex: 1; padding: .3em; background: #1c1c24; color: #eee;
             border: 1px solid #33333f; border-radius: 4px; }
    button { background: #22222b; color: #ccc; border: 1px solid #33333f;
             border-radius: 4px; padding: .3em .8em; cursor: pointer; }
    button.on { background: #2b4; color: #041; border-color: #2b4; }
  `;
  constructor() { super(); this.config = {}; this.state = {}; this.performers = []; }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared(sh) { if (sh && sh.topic === "performers") { this.performers = sh.performers || []; this.requestUpdate(); } }
  _assign(side, pid) { this.sendIntent && this.sendIntent({ assign: true, side, performerId: +pid || null }); }
  _active(side) { this.sendIntent && this.sendIntent({ active: side }); }
  _sel(side, cur) {
    return html`<select @change=${(e) => this._assign(side, e.target.value)}>
      <option value="">—</option>
      ${(this.performers || []).map((p) => html`<option value=${p.id} ?selected=${cur === p.id}>${p.name}</option>`)}
    </select>`;
  }
  render() {
    const s = this.state || {};
    return html`
      <div class="row">A: ${this._sel("a", s.a)}
        <button class=${s.active === "a" ? "on" : ""} @click=${() => this._active("a")}>Active</button></div>
      <div class="row">B: ${this._sel("b", s.b)}
        <button class=${s.active === "b" ? "on" : ""} @click=${() => this._active("b")}>Active</button></div>
      <div class="row"><button @click=${() => this.sendIntent && this.sendIntent({ reset: true })}>Reset race</button></div>
      <div>A ${(s.progress && s.progress.a) || 0} · B ${(s.progress && s.progress.b) || 0} · finish ${s.finish || 0}${s.winner ? ` · winner: ${s.winner}` : ""}</div>`;
  }
}
customElements.define("evo-tap-race-control", TapRaceControl);
