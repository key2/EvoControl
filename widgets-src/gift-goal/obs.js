import { LitElement, html, css } from "https://esm.run/lit@3";

class GoalObs extends LitElement {
  static properties = { state: { state: true } };
  static styles = css`
    :host { display: block; width: 100%; height: 100%; font-family: system-ui, sans-serif;
            color: #fff; text-shadow: 0 2px 6px rgba(0,0,0,.8); }
    .wrap { width: 100%; height: 100%; display: flex; flex-direction: column;
            justify-content: center; }
    .top { display: flex; justify-content: space-between; font-weight: 800;
           font-size: 34px; margin-bottom: 8px; }
    .bar { height: 46px; border-radius: 23px; background: rgba(0,0,0,.4);
           overflow: hidden; }
    .fill { height: 100%; background: linear-gradient(90deg,#ffb347,#ff6a3d);
            transition: width .4s ease; }
  `;
  constructor() { super(); this.state = {}; }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared() {}
  render() {
    const s = this.state || {};
    return html`<div class="wrap">
      <div class="top"><span>${s.title || "Goal"}</span>
        <span>${(s.total || 0).toLocaleString()} / ${(s.goal || 0).toLocaleString()}</span></div>
      <div class="bar"><div class="fill" style="width:${s.pct || 0}%"></div></div>
    </div>`;
  }
}
customElements.define("evo-gift-goal-obs", GoalObs);
