// Tap Race — the fastest widget to demo: assign two performers, pick the
// "active" side, and every incoming gift pushes that side toward the finish
// line, crediting the active performer's salary (§7). First to `finish` wins.
registerWidget({
  onLoad(ctx, saved) {
    this.finish = ctx.config.finish || 1000;
    saved = saved || {};
    this.a = saved.a || null;
    this.b = saved.b || null;
    this.progress = saved.progress || { a: 0, b: 0 };
    this.active = saved.active || "a";  // which side receives the next gift
    this.winner = saved.winner || null;
    this.publish(ctx);
  },
  onStart(ctx) { this.progress = { a: 0, b: 0 }; this.winner = null; this.publish(ctx); },
  onIntent(ctx, msg) {
    if (!msg) return;
    if (msg.assign) this[msg.side] = msg.performerId;   // {assign, side:"a"|"b", performerId}
    if (msg.active) this.active = msg.active;            // {active:"a"|"b"}
    if (msg.reset) { this.progress = { a: 0, b: 0 }; this.winner = null; }
    ctx.persist();
    this.publish(ctx);
  },
  onGift(ctx, gift) {
    if (this.winner) return;
    const side = this.active;
    const performerId = side === "a" ? this.a : this.b;
    if (!performerId) return;
    ctx.creditGift(performerId, gift);
    this.progress[side] += gift.diamonds;
    if (this.progress[side] >= this.finish) this.winner = side;
    this.publish(ctx);
  },
  publish(ctx) {
    ctx.setState({
      finish: this.finish, a: this.a, b: this.b, active: this.active,
      progress: this.progress, winner: this.winner,
    });
  },
  saveState() {
    return { a: this.a, b: this.b, progress: this.progress,
             active: this.active, winner: this.winner };
  },
});
