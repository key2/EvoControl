// Score Battle — the reference battle widget (§2/§6.4).
//
// Authoritative game logic in QuickJS. Owns:
//   * gift->side routing map (rose->left, tiktok-gift->right), set by control.js
//   * per-side performer assignment (drag-drop or Detect Faces via intents)
//   * sticky routing: a gifter's subsequent gifts follow their last routed side
//     until a flip gift moves them
//   * a per-round countdown timer, round wins (race to target_wins)
//   * top-3 gifters per side (by diamonds this round)
//
// Diamonds (salary) go to the ledger via ctx.creditGift; round wins + bar fill
// are widget-local state persisted in saveState (§7).

registerWidget({
  onLoad(ctx, saved) {
    const c = ctx.config || {};
    this.roundSec = c.round_sec || 180;
    this.targetWins = c.target_wins || 10;
    // giftRouting: { giftId: "left"|"right" }
    this.routing = c.giftRouting || {};
    saved = saved || {};
    this.left = saved.left || null;      // performerId
    this.right = saved.right || null;
    this.wins = saved.wins || { left: 0, right: 0 };
    this.round = saved.round || { left: 0, right: 0 }; // diamonds this round
    this.sticky = saved.sticky || {};    // gifterId -> "left"|"right"
    this.gifters = saved.gifters || { left: {}, right: {} }; // id->{diamonds,name}
    ctx.timer.reset(this.roundSec);
    this.publish(ctx);
  },

  onStart(ctx) {
    // Each native Start begins a fresh round (§14): reset round-local state.
    this.round = { left: 0, right: 0 };
    this.gifters = { left: {}, right: {} };
    ctx.timer.start(this.roundSec);
    const self = this;
    ctx.timer.onExpire = function () { self.endRound(ctx); };
    this.publish(ctx);
  },

  onStop(ctx) { ctx.timer.stop(); this.publish(ctx); },

  onTick(ctx) {
    // Broadcast the ticking timer so the bar/countdown animate.
    this.publish(ctx);
  },

  onGift(ctx, gift) {
    // Decide the side. Explicit routing gift flips the gifter's sticky side;
    // otherwise the gifter's existing sticky side is used (§6.4 step 4).
    let side = this.routing[gift.giftId];
    if (side) {
      this.sticky[gift.userId] = side;         // flip / set sticky
    } else {
      side = this.sticky[gift.userId];          // follow sticky
    }
    if (!side) return;                          // unrouted: stays show coins
    const performerId = side === "left" ? this.left : this.right;
    if (!performerId) return;                    // no one assigned yet

    // Credit salary to the ledger (authoritative).
    ctx.creditGift(performerId, gift);

    // Widget-local score + top gifters.
    this.round[side] += gift.diamonds;
    const g = this.gifters[side][gift.userId] || { diamonds: 0, name: gift.userName };
    g.diamonds += gift.diamonds; g.name = gift.userName; g.id = gift.userId;
    this.gifters[side][gift.userId] = g;
    this.publish(ctx);
  },

  onIntent(ctx, msg) {
    if (!msg) return;
    if (msg.assign) {              // {assign:true, side, performerId}
      if (msg.side === "left") this.left = msg.performerId;
      else this.right = msg.performerId;
    }
    if (msg.route) {              // {route:true, giftId, side}
      this.routing[msg.giftId] = msg.side;
      ctx.config.giftRouting = this.routing;
    }
    if (msg.detectFaces && Array.isArray(msg.faces)) {
      // Map recognized faces to slots by x position: leftmost -> left (§15).
      const fs = msg.faces.filter((f) => f.performerId).sort((a, b) => a.x - b.x);
      if (fs[0]) this.left = fs[0].performerId;
      if (fs[1]) this.right = fs[1].performerId;
    }
    ctx.persist();
    this.publish(ctx);
  },

  endRound(ctx) {
    // Award the round win to the higher side; reset the bar.
    if (this.round.left > this.round.right) this.wins.left++;
    else if (this.round.right > this.round.left) this.wins.right++;
    ctx.timer.stop();
    ctx.persist();
    this.publish(ctx);
  },

  publish(ctx) {
    const top = (side) =>
      Object.values(this.gifters[side])
        .sort((a, b) => b.diamonds - a.diamonds)
        .slice(0, 3)
        .map((g) => ({ id: g.id, name: g.name, diamonds: g.diamonds,
                       avatar: ctx.userAvatar(g.id) }));
    ctx.setState({
      left: this.left, right: this.right,
      round: this.round, wins: this.wins,
      target: this.targetWins,
      timer: ctx.timer.remaining,
      topLeft: top("left"), topRight: top("right"),
    });
  },

  saveState() {
    return {
      left: this.left, right: this.right, wins: this.wins, round: this.round,
      sticky: this.sticky, gifters: this.gifters,
    };
  },
});
