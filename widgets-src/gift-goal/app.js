// Gift Goal — every gift counts toward a diamond goal and (optionally) credits
// one chosen performer (§7). Simplest way to see gifts flow into the ledger.
registerWidget({
  onLoad(ctx, saved) {
    this.title = ctx.config.title || "Goal";
    this.goal = ctx.config.goal || 5000;
    saved = saved || {};
    this.total = saved.total || 0;
    this.beneficiary = saved.beneficiary || null;  // performerId or null
    this.publish(ctx);
  },
  onIntent(ctx, msg) {
    if (!msg) return;
    if (msg.assign) this.beneficiary = msg.performerId;  // {assign, performerId}
    if (msg.reset) this.total = 0;
    if (msg.set && msg.set.goal) this.goal = msg.set.goal;
    ctx.persist();
    this.publish(ctx);
  },
  onGift(ctx, gift) {
    this.total += gift.diamonds;
    // Attribute to the beneficiary if one is set (salary); otherwise the gift
    // stays a show-level coin (the engine writes the NULL-performer row).
    if (this.beneficiary) ctx.creditGift(this.beneficiary, gift);
    this.publish(ctx);
  },
  publish(ctx) {
    ctx.setState({
      title: this.title, goal: this.goal, total: this.total,
      pct: Math.min(100, Math.round((this.total / this.goal) * 100)),
      beneficiary: this.beneficiary,
    });
  },
  saveState() { return { total: this.total, beneficiary: this.beneficiary }; },
});
