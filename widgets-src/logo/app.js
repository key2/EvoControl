// Logo/image widget — static image. No feeds; publishes its URL on load.
registerWidget({
  onLoad(ctx) {
    ctx.setState({ url: ctx.config.url || "", fit: ctx.config.fit || "contain" });
  },
  onIntent(ctx, msg) {
    if (msg && msg.set) {
      Object.assign(ctx.config, msg.set);
      ctx.setState({ url: ctx.config.url, fit: ctx.config.fit });
    }
  },
  saveState() { return {}; },
});
