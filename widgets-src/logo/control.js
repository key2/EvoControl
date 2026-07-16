import { LitElement, html, css } from "https://esm.run/lit@3";

class LogoControl extends LitElement {
  static properties = { config: {} };
  static styles = css`
    :host { display: block; font-family: system-ui, sans-serif; }
    label { display: block; margin: .5em 0 .1em; color: #aaa; font-size: 12px; }
    input, select { width: 100%; padding: .3em; background: #1c1c24; color: #eee;
                    border: 1px solid #33333f; border-radius: 4px; }
    img { max-width: 100%; margin-top: .5em; background: #000; }
  `;
  constructor() { super(); this.config = {}; }
  _upd(k, v) {
    this.config = { ...this.config, [k]: v };
    if (this.updateConfig) this.updateConfig(this.config);
    if (this.sendIntent) this.sendIntent({ set: { [k]: v } });
    this.requestUpdate();
  }
  render() {
    const c = this.config || {};
    return html`
      <label>Image URL</label>
      <input .value=${c.url || ""} @input=${(e) => this._upd("url", e.target.value)}>
      <label>Fit</label>
      <select @change=${(e) => this._upd("fit", e.target.value)}>
        ${["contain", "cover", "fill"].map((f) => html`<option ?selected=${c.fit === f}>${f}</option>`)}
      </select>
      ${c.url ? html`<img src=${c.url}>` : ""}`;
  }
}
customElements.define("evo-logo-control", LogoControl);
