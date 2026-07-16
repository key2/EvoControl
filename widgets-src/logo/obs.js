import { LitElement, html, css } from "https://esm.run/lit@3";

class LogoObs extends LitElement {
  static properties = { state: { state: true } };
  static styles = css`
    :host { display: block; width: 100%; height: 100%; }
    img { width: 100%; height: 100%; }
  `;
  constructor() { super(); this.state = {}; }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared() {}
  render() {
    const s = this.state || {};
    if (!s.url) return html``;
    return html`<img src=${s.url} style="object-fit:${s.fit || "contain"}">`;
  }
}
customElements.define("evo-logo-obs", LogoObs);
