// Top Gifter — tracks cumulative diamonds per gifter and spotlights the leader
// with their avatar (§17 userAvatar). Purely cosmetic: it does not credit
// performers, so gifts remain show coins unless another widget attributes them.
registerWidget({
  onLoad(ctx, saved) {
    this.label = ctx.config.label || "TOP GIFTER";
    saved = saved || {};
    this.gifters = saved.gifters || {};  // userId -> {diamonds,name}
    this.publish(ctx);
  },
  onStart() { this.gifters = {}; },  // reset each round
  onGift(ctx, gift) {
    const g = this.gifters[gift.userId] || { diamonds: 0, name: gift.userName };
    g.diamonds += gift.diamonds; g.name = gift.userName; g.id = gift.userId;
    this.gifters[gift.userId] = g;
    this.publish(ctx);
  },
  publish(ctx) {
    let top = null;
    for (const g of Object.values(this.gifters))
      if (!top || g.diamonds > top.diamonds) top = g;
    ctx.setState({
      label: this.label,
      top: top ? { name: top.name, diamonds: top.diamonds,
                   avatar: ctx.userAvatar(top.id) } : null,
    });
  },
  saveState() { return { gifters: this.gifters }; },
});
