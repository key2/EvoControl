import { LitElement, html, css } from "https://esm.run/lit@3";

// OBS renderer for the text widget: transparent, shadowed for legibility over
// video, sized/positioned by the host.
class TextObs extends LitElement {
  static properties = { state: { state: true } };
  static styles = css`
    :host { display: block; width: 100%; height: 100%; }
    .t {
      width: 100%; height: 100%; display: flex; align-items: center;
      color: #fff; font-family: system-ui, sans-serif; font-weight: 800;
      text-shadow: 0 2px 6px rgba(0,0,0,.8), 0 0 2px rgba(0,0,0,.9);
      line-height: 1.05;
    }
  `;
  constructor() { super(); this.state = {}; }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared() {}
  render() {
    const s = this.state || {};
    const style = `font-size:${s.size || 64}px;color:${s.color || "#fff"};` +
      `justify-content:${s.align === "left" ? "flex-start" : s.align === "right" ? "flex-end" : "center"};` +
      `text-align:${s.align || "center"};`;
    return html`<div class="t" style="${style}">${s.text || ""}</div>`;
  }
}
customElements.define("evo-text-obs", TextObs);
