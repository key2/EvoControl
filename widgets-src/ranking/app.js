// Ranking widget (§8.2): subscribes to the shared "points" plane and publishes
// a sorted performer→diamonds list. Defaults to per-scene points (§4); a config
// switch selects per-shift.
registerWidget({
  onLoad(ctx) {
    this.scope = ctx.config.scope || "scene_run";  // "scene_run" | "shift"
    this.limit = ctx.config.limit || 8;
    this.perf = ctx.performers();  // [{id,name,avatarUrl}]
    ctx.subscribe("points");
    this.emitFrom(ctx, ctx.points(this.scope));
  },
  onShared(ctx, topic, data) {
    if (topic !== "points" || !data) return;
    const pts = data[this.scope] || {};
    this.perf = ctx.performers();
    this.emitFrom(ctx, pts);
  },
  emitFrom(ctx, pts) {
    const rows = (this.perf || [])
      .map((p) => ({ id: p.id, name: p.name, avatar: p.avatarUrl,
                     diamonds: pts[String(p.id)] || 0 }))
      .filter((r) => r.diamonds > 0)
      .sort((a, b) => b.diamonds - a.diamonds)
      .slice(0, this.limit);
    ctx.setState({ rows });
  },
  saveState() { return {}; },
});
