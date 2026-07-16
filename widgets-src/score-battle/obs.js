import { LitElement, html, css } from "https://esm.run/lit@3";

// OBS overlay for the score battle: a two-color fill bar with the round
// countdown in the middle, top-gifter avatars per side, round-wins (1|1), and
// a full-height separator (the widget can span the whole canvas, §2).
class BattleObs extends LitElement {
  static properties = { state: { state: true }, performers: { state: true } };
  static styles = css`
    :host { display: block; width: 100%; height: 100%; position: relative;
            font-family: system-ui, sans-serif; color: #fff;
            text-shadow: 0 2px 6px rgba(0,0,0,.8); }
    .bar { position: absolute; top: 20px; left: 4%; right: 4%; height: 54px;
           border-radius: 27px; overflow: hidden; display: flex;
           box-shadow: 0 2px 10px rgba(0,0,0,.5); }
    .l { background: linear-gradient(#ff7ab6, #e5468f); }
    .r { background: linear-gradient(#4bd6e5, #2aa7c4); }
    .timer { position: absolute; top: 26px; left: 50%; transform: translateX(-50%);
             font-weight: 800; font-size: 30px; }
    .wins { position: absolute; top: 84px; width: 100%; text-align: center;
            font-weight: 800; font-size: 22px; }
    .sep { position: absolute; top: 90px; bottom: 0; left: 50%; width: 4px;
           transform: translateX(-2px); background: rgba(255,255,255,.35); }
    .top { position: absolute; top: 24px; display: flex; gap: 6px; }
    .top.left { left: 6%; } .top.right { right: 6%; flex-direction: row-reverse; }
    .top img { width: 44px; height: 44px; border-radius: 50%; border: 2px solid #fff;
               object-fit: cover; background: #333; }
  `;
  constructor() { super(); this.state = {}; }
  applyState(s) { this.state = { ...this.state, ...s }; }
  applyShared() {}
  render() {
    const s = this.state || {};
    const l = (s.round && s.round.left) || 0;
    const r = (s.round && s.round.right) || 0;
    const total = l + r || 1;
    const lp = Math.max(4, (l / total) * 100);
    const wins = s.wins || { left: 0, right: 0 };
    const mmss = (sec) => {
      sec = Math.max(0, Math.round(sec || 0));
      return `${Math.floor(sec / 60)}:${String(sec % 60).padStart(2, "0")}`;
    };
    return html`
      <div class="bar">
        <div class="l" style="width:${lp}%"></div>
        <div class="r" style="width:${100 - lp}%"></div>
      </div>
      <div class="timer">${mmss(s.timer)}</div>
      <div class="wins">${wins.left} | ${wins.right}</div>
      <div class="sep"></div>
      <div class="top left">
        ${(s.topLeft || []).map((g) => html`<img src=${g.avatar || ""} title=${g.name}>`)}
      </div>
      <div class="top right">
        ${(s.topRight || []).map((g) => html`<img src=${g.avatar || ""} title=${g.name}>`)}
      </div>`;
  }
}
customElements.define("evo-score-battle-obs", BattleObs);
