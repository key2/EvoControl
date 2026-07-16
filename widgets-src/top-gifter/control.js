import { LitElement, html, css } from "https://esm.run/lit@3";

class TopGifterControl extends LitElement {
  static properties = { config: {}, state: { state: true } };
  static styles = css`
    :host { display: block; font-family: system-ui, sans-serif; color: #ddd; }
    label { display: block; margin: .5em 0 .1em; color: #aaa; font-size: 12px; }
    input { width: 100%; padding: .3em; background: #1c1c24; color: #eee;
            border: 1px solid #33333f; border-radius: 4px; }
  `;
  constructor() { super(); this.config = {}; this.state = {}; }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared() {}
  render() {
    const c = this.config || {}, s = this.state || {};
    return html`
      <label>Label</label>
      <input .value=${c.label || "TOP GIFTER"} @input=${(e) => {
        this.config = { ...this.config, label: e.target.value };
        this.updateConfig && this.updateConfig(this.config);
      }}>
      <p>Current: ${s.top ? `${s.top.name} (${s.top.diamonds})` : "—"}</p>`;
  }
}
customElements.define("evo-top-gifter-control", TopGifterControl);
