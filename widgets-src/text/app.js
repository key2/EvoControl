// Text widget — the degenerate case (§8.2). No point feeds; it just holds its
// text config and publishes it so the views render it. onLoad publishes once.
registerWidget({
  onLoad(ctx) {
    ctx.setState({
      text: ctx.config.text || "Text",
      size: ctx.config.size || 64,
      color: ctx.config.color || "#ffffff",
      align: ctx.config.align || "center",
    });
  },
  onIntent(ctx, msg) {
    // control.js can live-edit the text before Save.
    if (msg && msg.set) {
      Object.assign(ctx.config, msg.set);
      ctx.setState({
        text: ctx.config.text, size: ctx.config.size,
        color: ctx.config.color, align: ctx.config.align,
      });
    }
  },
  saveState(ctx) { return {}; },
});
