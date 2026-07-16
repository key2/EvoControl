import { LitElement, html, css } from "https://esm.run/lit@3";

// Control UI for the text widget: edit text/size/color; live-preview via intent,
// and persist into the instance config (host saves with the scene).
class TextControl extends LitElement {
  static properties = { config: {} };
  static styles = css`
    :host { display: block; font-family: system-ui, sans-serif; }
    label { display: block; margin: .5em 0 .1em; color: #aaa; font-size: 12px; }
    input { width: 100%; padding: .3em; background: #1c1c24; color: #eee;
            border: 1px solid #33333f; border-radius: 4px; }
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
      <label>Text</label>
      <input .value=${c.text || ""} @input=${(e) => this._upd("text", e.target.value)}>
      <label>Font size</label>
      <input type="number" .value=${c.size || 64} @input=${(e) => this._upd("size", +e.target.value)}>
      <label>Color</label>
      <input type="color" .value=${c.color || "#ffffff"} @input=${(e) => this._upd("color", e.target.value)}>
      <label>Align</label>
      <select @change=${(e) => this._upd("align", e.target.value)}>
        ${["left", "center", "right"].map((a) => html`<option ?selected=${c.align === a}>${a}</option>`)}
      </select>`;
  }
}
customElements.define("evo-text-control", TextControl);
