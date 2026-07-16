# EvoControl — Architecture & Implementation Plan

> EvoControl is a control application for interactive **TikTok LIVE** shows,
> built by modifying **DearTT** (this repository). It reuses DearTT's ImGui
> shell, its `ttlive-cpp` event client, its embedded HTTP/WebSocket server, its
> QuickJS engine, its FFmpeg video pipeline, and its face-recognition stack.
>
> Revision 4 — final. All questions from design rounds 1–3 are folded in;
> settled items are in **§4 Resolved decisions**. No blocking questions remain;
> **§22** lists the details deferred to implementation time.

---

## 1. Product summary

On a stage, 3–7 (occasionally up to ~30) **performers** play games. **Viewers**
send gifts to their favourite performers. EvoControl:

1. Connects to the room's TikTok LIVE protobuf event stream **reliably** (WS +
   long-poll, deduped) and independently pulls the room's **video** (HLS/FLV)
   for the operator view and face recognition.
2. Renders **games** ("widgets") onto:
   - an **OBS browser overlay** (the "OBS view", 1080×1920, 9:16, transparent
     background, composited by OBS over the camera), and
   - an **in-browser control panel** (the "Control view").
3. Lets the operator **compose scenes** (place widgets on the 9:16 canvas),
   **configure games**, assign performers, and run the show live.
4. **Attributes gifts to performers** (via per-widget game logic), keeps
   per-scene-run and per-shift totals, and streams updates to both views in
   real time.
5. **Never loses accounting**: every credited gift is written to an
   append-only ledger — per-performer gift totals are **salary data** and must
   survive any crash or restart.

**One account is live at a time** (never concurrent); the operator switches
accounts between shifts. EvoControl is **never headless** — the native app is
the master console (load account/scene, **Start**, "New Shift", future
audio-clip buttons). The Control view is **always used on localhost**; the OBS
view may be watched by anyone on the LAN.

### 1.1 Where the video comes from (important)

EvoControl does **not** capture from OBS or the camera. It pulls the **same
video a viewer sees** — TikTok's HLS/FLV stream — decoded with FFmpeg (exactly
what DearTT does today). That decoded video is:

- the operator's monitor inside the native app (situational awareness, chat),
- the input to **face recognition** ("Detect Faces"), and
- (future) the far end of the **latency tool**: flash a QR/timestamp on the OBS
  overlay, detect it in the delayed TikTok stream, diff the timestamps.

```
                          EvoControl (native app)
                          ┌───────────────────────────────────┐
 TikTok LIVE ──HLS/FLV───►│ FFmpeg decode ─► operator view    │
 (viewer video)           │               └► FaceTracker      │
                          │                                   │
 TikTok LIVE ──WS+poll───►│ ttlive-cpp ─► Game Engine ─► ledger│
 (events/gifts)           └───────────────────────────────────┘
                                        │ http + ws (localhost / LAN)
                    ┌───────────────────┴───────────────────┐
              OBS view (browser source              Control view (operator tab,
              in OBS, transparent bg)               always localhost)
                    │
   USB capture ─► OBS Studio ─► beautify/filter ─► on-air
   (webview overlay composited on top)
```

---

## 2. Reference scene (the target to reproduce)

A real production frame ("Rich Girls — Street Style Battle") decomposes into
five widget instances on the 1080×1920 canvas:

```
┌────────────────────────────────────────────┐
│              [ LOGO widget ]               │  image/logo widget
│         "10 wins - solo dance!"            │  simple text widget
│ ┌────────────────────────────────────────┐ │
│ │🌹  (top    Alexa   2:09   Adel  (top 🎵│ │  battle score-bar widget:
│ │    gifter)  ————————█████████——————     │ │   • gift→side map (rose | tiktok)
│ │  1         pink bar │ cyan bar        1 │ │   • performer per side
│ ├───────────── [PK Win 6] ────────────────┤ │   • countdown timer (round)
│ │  "10000 coins MC Morgan dance"          │ │   • top-gifter avatars per side
│ │                    │                    │ │   • round-wins score (1 | 1)
│ │                    │  ◄─ full-height    │ │   • full-height separator bar
│ │     Alexa          │     separator      │ │
│ │    (camera)        │     (same widget)  │ │  another text widget
│ │                    │        Adel        │ │  (operator announcement)
│ │                    │      (camera)      │ │
│ │                    │   ┌──────────────┐ │ │
│ │                    │   │1 Alexa 13,004│ │ │  ranking widget:
│ │                    │   │2 Adel   5,050│ │ │   performer avatars + diamond
│ │                    │   │3 Morgan 1,453│ │ │   totals, sorted, live
│ │                    │   │4 Keit   1,199│ │ │
│ └────────────────────┴───┴──────────────┴─┘ │
└────────────────────────────────────────────┘
```

Observations this frame locks in:

- **Two kinds of points coexist** (§7): the battle shows **round wins** (1 | 1,
  race to 10 → "solo dance"), while the ranking shows **diamonds** (Alexa
  13,004). Diamonds are the authoritative accounting; round wins are
  widget-local game state.
- **Widgets can be full-canvas**: the separator bar belongs to the battle
  widget and spans the whole height. Widget rects may be any size, overlap,
  and are z-ordered; backgrounds are transparent.
- **Top-gifter avatars** render inside a widget from gifter user-ids (§17).
- The battle is **round-based with rotation**: every native **Start** begins a
  new 3-minute round; the round's loser leaves the stage and a performer from
  outside takes the slot. The operator re-assigns the side by dragging the new
  performer onto the score bar — or just presses **"Detect Faces"** and the
  recognized face fills the slot (§15). Morgan and Keit hold diamonds because
  they battled in **earlier rounds of the same scene run** — per-scene totals
  accumulate across rounds while performers rotate through the slots.
- Text widgets are trivially simple (no subscriptions) — the widget model must
  scale *down* to them.

---

## 3. What already exists in the fork (DearTT) — reuse inventory

| DearTT component | File(s) | EvoControl reuse |
|---|---|---|
| TikTok LIVE client (events, gifts, stream URLs) | `ttlive-cpp/` (`client.hpp`, `events.hpp`) | Core event source. |
| **Reliable dual transport** (WS + poll, dedup by `msg_id`) | `ClientOptions{use_websocket,use_polling}` (`client.hpp:14-29`), `Event::msg_id` (`events.hpp:96`) | Enable **both**; `msg_id` is also the ledger's exactly-once key. |
| Gift list + diamond prices | `GiftInfo` (`events.hpp:52-59`), `fetch_gift_list()` (`client.hpp:114-119`) | Gift gallery for widgets (drag a gift onto a side). |
| QuickJS engine (vendored, built) | `ttlive-cpp/src/qjs_signer.*`, `third_party/quickjs` | Add the **widget runtime** beside the existing signer use. |
| HTTP + WebSocket server | `src/event_server.*` (civetweb) | Grows REST routes + the multiplexed WS (§11). |
| Event → JSON (reflective protobuf decode) | `src/event_json.cpp` | Raw monitor feed; debugging. |
| Per-gift/per-gifter accounting + **streak dedup** | `src/stats.*` (`StatsCollector`) | The "count a combo once" rule feeds the ledger; the stats panel itself stays on the native main page (§16.1). |
| **FFmpeg video** + **miniaudio** | `src/video_player.*`, `audio_output.*` | **Kept** (operator view, face frames, latency tool, future audio clips). |
| STT (voxtral) | `src/stt.*` | Kept behind its existing disable toggle; removable later. |
| Face detect/recognize/track + click-to-correct | `src/face_*.cpp`, `models/*.onnx`, `main.cpp:1263` | "Detect Faces" + roster maintenance. |
| Roster: 1 embedding + avatar per person | `src/face_gallery.*` | Becomes **performers** (embeddings → SQLite, §12). |
| Avatar fetch (file/URL/@user) + drag-drop | `src/avatar_fetch.*` | Performer pictures. |
| Icon/avatar cache (download+decode+GL) | `src/icon_cache.*` | Backs the user-avatar API (§17) + the gift monitor icons (§16.2). |
| Profile = name + stream | `src/profile.*` | Becomes **account** in SQLite. |
| Resource resolution | `event_server.cpp` | Locate `web/`, widgets dir, DB, models. |

---

## 4. Resolved decisions (rounds 1–3)

| Topic | Decision |
|---|---|
| Rename | **DearTT → EvoControl** everywhere (`deartt`→`evocontrol`, `DEARTT_*`→`EVOCONTROL_*`). |
| Concurrency | **One account/stream at a time**; switch between shifts; never concurrent. |
| Headless | **Never.** Native ImGui app is the master console (accounts, scenes, Start/Stop, New Shift, settings; future audio clips). |
| Video | **Keep FFmpeg + miniaudio** (operator view, face frames, latency tool). STT stays behind its toggle for now. |
| Face frames | From the **TikTok HLS stream decoded by FFmpeg** (viewer's view) — not OBS/camera. |
| Gift attribution | **Widget logic decides.** Operator drags gifts onto slots in `control.js`; the map is a private intent to `app.js`; `app.js` credits performers (incl. stateful rules like sticky routing). |
| `app.js` sandbox | **No network / no file access** — only the injected host API. |
| Control coherency | **Single active control session.** Opening a new Control view supersedes the old one, which blacks out with "A new control session was opened" and dies. No multi-control sync needed. OBS views: any number (read-only). |
| Rendering | **Lit** custom elements for both views; one host HTML per view instantiates all widgets at canvas positions — **no iframes**. |
| Widget scale | ≤ ~5 widgets/scene; independent; cross-widget data flows only via EvoControl's shared plane. |
| Face storage | Performers (+ embeddings + avatars) in the **account's SQLite**; loaded to memory once on account load; click-to-correct re-persists. |
| Canvas | Fixed logical **1080×1920 (9:16)**; OBS view background transparent. |
| Shifts | **Manual.** Operator presses **"New Shift"** → running totals reset to 0. **All history is kept** (ledger + shift rows) for accounting/salary. |
| Scene load | **Scene load = new game.** Per-scene-run scores start at 0; shift totals keep accumulating scene after scene until New Shift. |
| Scene creation | Native "New scene" → Control view is **forced to the new scene's editor**, OBS view goes **blank**. Composition is not done live (it *can* be, but viewers would see it). Widget menu + drag/drop/move + **Save** live in the Control view. |
| Widget updates | Take effect **only at scene load**. Loading a scene forces **both views to load a fresh HTML page** (new bundles picked up there). |
| Crash safety | **Append-only gift ledger** (SQLite WAL) written as gifts arrive; per-performer totals are derived from it and rebuilt on restart. Chosen as "whatever is the safest" — this is salary data. |
| Auth | **OBS view: no password** (anyone on LAN may view). **Control view: optional password** set in EvoControl settings. Control is **always localhost**. |
| Widget trust | **No signing.** Installed widgets are fully trusted. Keep it simple. |
| Widget install | Drag-drop a `.zip` (`.evw`). Bundle must include an **icon** now (future widget store). |
| Host API | Minimum capability set decided: **user avatars, gift gallery, performer list, live face-detector positions per performer**. Exact signatures frozen while building the reference widgets (§9). |
| User avatars | EvoControl offers APIs so widgets can request any user's profile picture by id (§17). |
| Rounds & rotation | Every native **Start** begins a new round (e.g. 3 min). The loser leaves; a new performer enters; the operator re-assigns the slot by **drag-drop** in the Control view or via **"Detect Faces"**. Per-scene totals accumulate across rounds. |
| Control takeover | Superseded control page shows a black screen with a **"Take back control"** button; pressing it re-supersedes, sending the *other* page to the same black "control lost" screen. |
| Ranking default | The ranking widget shows **per-scene** points by default (per-shift available as config). |
| Shift export | `GET /api/shifts/:id/report` returns **JSON**: total won per performer on the shift + a per-scene(-run) breakdown of how much was won in each scene played (§13). |
| Control chrome | The Control view has a **fixed part identical for every scene**, present even when the scene is empty: lock/unlock widget displacement, the widget palette (icons) for drag-drop placement, delete widget, Save scene (§8.3). |
| Native statistics | DearTT's stats stay — they tell the operator whether the show has enough activity. **Total coins of the shift in a big font** is the headline; USD conversion not needed (§16.1). |
| Unattributed gifts | Gifts arriving while no game runs (or unrouted) still count as **show coins**: ledger rows with `performer_id NULL`, **included in the total-coins display** (§7, §16.1). |
| Chat defaults | Chat starts in **Overlay** mode with **"chat only"** checked (§16.3). |
| Gift monitor | The native main page shows a real-time gift table: @user, gift icon, gift name, amount (streaks), diamond worth, receiving performer (icon + name) when attributed (§16.2). |

---

## 5. Runtime topology

Three execution contexts run **the same widget, three ways**:

1. **`app.js`** — authoritative game logic, in **QuickJS** inside EvoControl.
   Owns widget state and point math. No network/file access.
2. **`obs.js`** — **Lit** element in the OBS browser: renders the overlay.
3. **`control.js`** — **Lit** element in the Control browser: renders the
   widget + its right-side controls.

```
┌──────────────────────────── EvoControl (native ImGui app) ────────────────────┐
│                                                                               │
│  ttlive-cpp ──gift/like/comment──►┐        ┌───────────────────────────────┐  │
│  (WS + poll, msg_id dedup)        │        │  Game Engine (single thread)  │  │
│                                   ├──feed──►  • streak dedup               │  │
│  FFmpeg video ─► FaceTracker ─faces────────►  • routes events to widgets   │  │
│      │                            │        │  • credits → gift ledger      │  │
│      └─► operator view (ImGui)    │        │  ┌──────── QuickJS ─────────┐ │  │
│                                   │        │  │ app.js │ app.js │ app.js │ │  │
│  Native console:                  │        │  │ (1 context per instance) │ │  │
│   accounts / scenes / Start /     │        │  └──────────────────────────┘ │  │
│   New Shift / settings ───────────┘        └───────────────┬───────────────┘  │
│                                                            │                  │
│   SQLite (WAL): accounts, performers, scenes, layout,      │ append + emit    │
│   shifts, scene runs, GIFT LEDGER, widget state  ◄─────────┘                  │
│                                                            │                  │
│            civetweb: REST (/api/*) + one multiplexed WS (/ws) per page        │
└──────────────────────────────────────────────────┬────────────────────────────┘
                                                   │
                     ┌─────────────────────────────┴──────────────────────┐
              OBS view page(s) (Lit host,                  Control view page (Lit host,
              transparent, read-only,                      SINGLE active session —
              any number, no auth)                         new session supersedes old)
```

---

## 6. The authoritative game model

### 6.1 Single source of truth: `app.js`

Each placed widget has exactly **one authoritative instance**: its `app.js` in
QuickJS. Browsers never own game state — they render what `app.js` publishes
and send **intents** back.

- **Intent** (Control page → engine → `app.js`): "map rose→left", "assign
  Alice→left", "set timer 180 s", "override Alice = 50".
- `app.js` validates, mutates its state, and **publishes** the confirmed state.
- **Broadcast** (`app.js` → engine → all views of that instance): OBS pages and
  the control page all render the same confirmed state. The Control UI is
  **not optimistic**: a change shows once `app.js` confirms it (single control
  session makes latency irrelevant on localhost).

### 6.2 Single active control session (from Q-C)

Multiple open control tabs confuse the operator, so EvoControl enforces **one**:

1. Every Control page, on connect, sends `hello {scope:"control"}` and receives
   a **session id**.
2. The server **revokes the previous control session**: it sends
   `session {status:"superseded"}` to the old page, which replaces its UI with
   a black screen — *"A new control session was opened."* — and stops sending
   intents (the server also rejects intents from stale session ids).
3. The black screen offers a **"Take back control"** button: pressing it makes
   *this* page the active session and sends the other page to the same black
   "control lost" screen (symmetric takeover; last click wins).
4. OBS pages (`scope:"obs"`) are read-only renderers: **any number** may
   connect, no takeover, no auth.

Late joiners (including the fresh page after a scene-load reload) receive a
**snapshot** of every widget instance's current state, so a mid-game (re)open
renders correctly immediately. Every `state` broadcast carries a monotonic
`seq` per instance; views drop stale updates after reconnects.

### 6.3 Two message planes

- **Widget-private plane** — `control.js ↔ app.js ↔ obs.js` of one instance
  (gift→slot maps, per-widget config/state). Routed by `instance` id; never
  visible to other widgets.
- **Shared plane** — owned by EvoControl: performer roster, **authoritative
  diamond totals** (per scene run / per shift), gift gallery, user avatars.
  Widgets subscribe (the ranking widget subscribes to `points`). Cross-widget
  effects are always indirect: `app.js` credits EvoControl → EvoControl
  updates shared totals → subscribers are notified. No widget-to-widget calls.

### 6.4 Worked example — the reference battle scene (§2)

1. Operator (native app) loads the "Battle" scene → both views reload → the
   control page shows the battle widget; clicking it opens its controls.
2. `control.js` shows the **gift gallery** (from EvoControl); the operator
   drags **rose → left**, **TikTok-logo gift → right**; drags **Alexa → left**,
   **Adel → right** (or presses **"Detect Faces"**, §15). Each drop is a
   private intent; `app.js` confirms; the OBS view now shows rose/Alexa left,
   tiktok/Adel right.
3. Operator presses **Start** (native). `app.js` starts the 3-minute round
   timer and broadcasts ticks.
4. A viewer sends a rose → engine dedups the streak, delivers to
   `app.js.onGift` → widget rule: rose→left → **credit Alexa** with the
   diamonds (§7). *Sticky routing* (this widget's choice): that gifter's
   subsequent gifts also go to Alexa until a TikTok-logo gift flips them to
   Adel — so rose (1) + heart (100) = Alexa 101.
5. The score bar animates from the widget's `state` broadcasts; top-3 gifter
   avatars per side load via the user-avatar API (§17).
6. Round ends → `app.js` awards a **round win** (1 | 1 on screen) and resets
   the bar; first to 10 round wins → "solo dance".
7. **Rotation:** the loser leaves the stage; a performer from outside comes
   in. The operator drags the newcomer's profile onto that side of the score
   bar — or checks in the native app that the faces are recognized and presses
   **"Detect Faces"** so the right performer fills the slot automatically —
   then presses **Start** for the next 3-minute round.
8. The **ranking widget** (subscribed to shared `points`) re-sorts per-scene
   diamond totals live — Morgan and Keit still rank from the rounds they
   played earlier in this scene.
9. Meanwhile every credit was appended to the **gift ledger**; a crash at any
   point loses nothing (§10).

---

## 7. Two kinds of points + the gift ledger

The reference frame shows both at once; they are different things:

| | Diamonds (salary) | Game points (score) |
|---|---|---|
| Example | Alexa 13,004 | Round wins 1 \| 1, race to 10 |
| Owner | **EvoControl** (authoritative) | The **widget** (`app.js` state) |
| Storage | **Append-only `gift_ledger`** (SQLite) | `widget_state` snapshots |
| Reset | Per **scene run** (view) & per **shift** (New Shift); history never deleted | Whatever the widget's rules say |
| Purpose | Accounting/salary, ranking widgets | Gameplay, win conditions |

**The ledger is the source of truth.** One row per credited attribution
(post streak-dedup), written the moment it happens:

- Per-**scene-run** totals = `SUM(diamonds) WHERE scene_run_id = current`.
- Per-**shift** totals = `SUM(diamonds) WHERE shift_id = current`.
- Totals are cached in memory for speed and **rebuilt from the ledger on
  boot** — a crash can never lose or double-count salary (dedup by TikTok
  `msg_id`).
- "New Shift" just opens a new `shift` row: displays reset to 0, **nothing is
  deleted** — full history remains for the shift report export (§13).
- Manual operator corrections (override points) are ledger rows too
  (`source:"manual"`, positive or negative), so the audit trail is complete.
- **Unattributed gifts are first-class, not an error case:** a gift sent while
  no game is running (or that no widget routes) is still coins for the show —
  logged with `performer_id NULL` and **included in the show/shift total
  coins** shown big in the native app (§16.1). It just doesn't credit any
  performer's salary.

---

## 8. Widget model

### 8.1 Bundle layout

```
score-battle.evw                (zip; drag-drop onto EvoControl installs it)
  manifest.json
  icon.png        # required — widget menu now, widget store later
  app.js          # QuickJS: authoritative game logic (no network/file)
  obs.js          # browser: Lit element for the OBS overlay
  control.js      # browser: Lit element for the control view + controls
  assets/…        # images/fonts/sounds used by obs/control
```

### 8.2 `manifest.json`

```json
{
  "type": "score-battle",
  "version": "1.2.0",
  "apiVersion": 1,
  "name": "Score Battle",
  "description": "Two-side gift battle: gift→side map, rounds, top gifters.",
  "icon": "icon.png",
  "defaultSize": { "w": 1080, "h": 1920 },
  "config": [
    { "key": "round_sec", "type": "int", "default": 180, "label": "Round timer" },
    { "key": "target_wins", "type": "int", "default": 10, "label": "Wins to victory" },
    { "key": "slots", "type": "performer-slots", "count": 2, "labels": ["Left", "Right"] },
    { "key": "giftRouting", "type": "gift-slot-map" }
  ],
  "subscribes": ["gift"],
  "capabilities": ["gift-gallery", "user-avatar"]
}
```

- The **text widget** is the degenerate case: `subscribes: []`, no `app.js`
  logic beyond holding its text config — it never receives point feeds.
- The **ranking widget** declares `subscribes: ["points"]` and shows
  **per-scene** points by default (decided); per-shift totals remain available
  as a config option.

### 8.3 Lit host pages (no iframes)

Each view is **one HTML page** + a host script:

1. `GET /api/scenes/current/widgets` → instances `{id, type, version, x, y, w,
   h, z, config}` on the 1080×1920 canvas.
2. `import()` each widget's `obs.js` / `control.js` (each defines a Lit custom
   element).
3. Instantiate one element per instance, absolutely positioned + z-ordered on
   the canvas, `instanceId` attribute wired to the WS envelope.
4. **OBS host:** transparent body (OBS composites it over the camera); no
   interaction.
5. **Control host:** the 9:16 canvas mirror on the left, plus a **fixed
   chrome that is identical for every scene and already present when the
   scene is empty**:
   - a **scene toolbar** — **lock/unlock widget displacement** (so widgets
     can't be nudged mid-show), **delete selected widget**, **Save scene**;
   - the **widget palette** — every installed widget with its icon (from
     `GET /api/widget-registry`), dragged-and-dropped onto the 1080p canvas
     to place it;
   - the **selected-widget panel** — clicking a widget on the canvas loads
     that widget's `control.js` UI here (its game-specific controls).

Widget rects may be **any size up to full canvas and may overlap** (the battle
widget's separator spans full height). The editor selects the topmost on
click and offers a layer list for occluded widgets.

---

## 9. `app.js` sandbox & host API

`app.js` runs in a QuickJS context with **no** `fetch`/sockets/filesystem/
`require`. The **capability set is decided** (round 3): user avatars, the gift
gallery, the **performer list**, and the **live face-detector positions per
performer**; exact signatures are frozen while building the reference widgets
(§22). Injected surface:

```js
// Lifecycle
onLoad(ctx, savedState)   // scene load (savedState = null) or crash resume
onStart(ctx) / onStop(ctx)
onTick(ctx, dtMs)         // engine-driven (~10 Hz)
onGift(ctx, gift)         // streak-deduped: {userId, giftId, name, diamonds, …}
onIntent(ctx, msg)        // private message from control.js
onShared(ctx, topic, data)// subscribed shared-plane events
saveState(ctx) -> object  // JSON snapshot for crash recovery

// ctx capabilities (the widget's whole world)
ctx.config                 // instance config (saved with the scene)
ctx.performers()           // [{id, name, avatarUrl}]
ctx.giftGallery()          // [{giftId, name, diamonds, iconUrl}]
ctx.userAvatar(userId)     // local cached URL for any TikTok user id
ctx.faces()                // live face-detector output: [{performerId, name,
                           //   x, y, w, h, similarity}] — normalized [0..1]
                           //   frame coords (map directly onto the canvas)
ctx.creditGift(performerId, gift)      // -> gift ledger (salary, authoritative)
ctx.adjustPoints(performerId, diamonds, reason)  // manual/correction ledger row
ctx.points(scope)          // shared totals: "scene_run" | "shift"
ctx.emit(view, data)       // view: "obs" | "control" | "all" (this instance only)
ctx.subscribe(topic)       // shared plane: "points", …
ctx.timer                  // engine-backed countdown helpers
ctx.persist()              // request an immediate saveState snapshot
```

Note the split mirroring §7: `creditGift`/`adjustPoints` write **diamonds**
(ledger); game points (round wins) are just widget state inside
`saveState()`.

`ctx.faces()` (and `GET /api/faces` for `control.js`) reads the FaceTracker's
latest results; positions come from the **TikTok stream**, which runs a few
seconds behind reality — performers should be in place briefly before "Detect
Faces" is pressed.

**Isolation:** all instances on one engine thread; one QuickJS context per
instance; per-callback interrupt budget + per-context memory cap so a runaway
widget is killed without harming the engine. ≤5 widgets ⇒ no perf concern.

---

## 10. Persistence, crash safety, restart

**Requirement: per-performer gift accounting is salary — losing it is not an
option; save gifts as they come.**

1. **Gift ledger (append-only, complete).** Every finalized gift is appended
   **exactly once**: attributed rows when a widget credits performers
   (`ctx.creditGift`), one `performer_id NULL` row otherwise (no game running
   / unrouted) — *inside a SQLite WAL transaction* before being broadcast.
   `msg_id` uniqueness makes this exactly-once across restarts even mid-combo,
   and the big total-coins display (§16.1) is simply the SUM of **all** rows.
2. **Widget state snapshots.** `saveState()` JSON into `widget_state` every
   ~1 s and after significant transitions (round end, config change). Restores
   sticky routing, round wins, timer remaining.
3. **Runtime row.** The single `runtime` row records active account, shift,
   scene run, and running flag.
4. **Restart flow.** On boot: open DB → if `runtime.running`, reload the same
   scene run, rebuild all totals by `SUM` over the ledger, `onLoad(ctx,
   savedState)` each widget, resume. Worst case lost: ≤ the snapshot interval
   of *widget-local* state; **zero** ledger/salary loss.
5. **Operator scene load ≠ crash resume.** A deliberate scene load starts a
   **new scene run** (fresh scores, `onLoad(ctx, null)`, pages force-reloaded);
   crash resume restores the same run.

---

## 11. Server (civetweb): static, WS protocol, REST

### 11.1 Static

- `GET /obs` → OBS host page (transparent). No auth (LAN-viewable).
- `GET /control` → Control host page. Optional password (settings, §18).
- `GET /widgets/<type>/<version>/…` → bundle files (`obs.js`, `control.js`,
  `icon.png`, `assets/…`).

### 11.2 WebSocket `/ws` (one connection per page)

Envelope: `{ v:1, scope:"obs"|"control", instance:<id|null>, topic, seq, data }`

| Topic | Direction | Purpose |
|---|---|---|
| `hello` | page → server | `{scope, auth?}` — registers the page; control pages get the single-session check |
| `session` | server → page | `{id}` granted, or `{status:"superseded"}` → black screen |
| `snapshot` | server → page | full state of every instance of the current scene (on join/reload) |
| `state` | server → page | authoritative widget state broadcast (`seq`-guarded) |
| `shared` | server → page | shared-plane events (`points`, roster changes) |
| `intent` | control → server | private message routed to the instance's `app.js` (stale sessions rejected) |
| `navigate` | server → page | scene (re)load: page must load the given fresh URL — this is when widget updates take effect |

Inbound frames are parsed in `wsData` (a no-op today,
`event_server.cpp:158`).

### 11.3 REST (illustrative)

```
# Accounts / performers / shifts
GET/POST   /api/accounts                        {name, stream, cookies?}
POST       /api/accounts/:id/load
GET/POST   /api/accounts/:id/performers         {name}
PUT/DELETE /api/performers/:id
POST       /api/performers/:id/avatar           (file | {url} | {tiktok})
POST       /api/performers/:id/face/reset
POST       /api/shifts/new                      ("New Shift" — native button)
GET        /api/shifts/:id/report               (accounting JSON — §13)

# Scenes / composition
GET/POST   /api/accounts/:id/scenes             {name}
POST       /api/scenes/:id/duplicate
PUT/DELETE /api/scenes/:id
GET        /api/scenes/:id/widgets
PUT        /api/scenes/:id/widgets              (bulk "Save scene")
POST       /api/scenes/:id/widgets              {type, x,y,w,h,z, config}
PUT/DELETE /api/widgets/:id

# Runtime (driven by the native app)
POST       /api/scenes/:id/load                 (compose or play → views navigate)
POST       /api/scenes/:id/start | /stop
POST       /api/performers/:id/points/adjust    {diamonds, reason}

# Widget data services
GET        /api/gift-gallery
GET        /api/users/:id/avatar                (cached proxy — §17)
GET        /api/faces                           ("Detect Faces" — §15)
GET        /api/widget-registry
POST       /api/widgets/install                 (.zip upload / drag-drop)
```

Game **intents go over the WS only** (they need the live session id); REST
covers CRUD + native-app actions.

---

## 12. Data model (SQLite, WAL)

```sql
CREATE TABLE account (
  id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL,
  stream TEXT NOT NULL, cookies TEXT, created_at INTEGER);

CREATE TABLE performer (
  id INTEGER PRIMARY KEY,
  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
  name TEXT NOT NULL, avatar_path TEXT,
  face_embedding BLOB, face_quality REAL,        -- recognition "landmark"
  UNIQUE(account_id, name));

CREATE TABLE shift (                             -- never deleted (accounting)
  id INTEGER PRIMARY KEY,
  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
  started_at INTEGER NOT NULL, ended_at INTEGER);

CREATE TABLE scene (
  id INTEGER PRIMARY KEY,
  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
  name TEXT NOT NULL, ordering INTEGER, UNIQUE(account_id, name));

CREATE TABLE scene_widget (                      -- saved composition
  id INTEGER PRIMARY KEY,
  scene_id INTEGER NOT NULL REFERENCES scene(id) ON DELETE CASCADE,
  widget_type TEXT NOT NULL,
  x REAL, y REAL, w REAL, h REAL, z INTEGER,
  config_json TEXT);                             -- timer, slots, gift map, text…

CREATE TABLE scene_run (                         -- one row per "load scene"
  id INTEGER PRIMARY KEY,
  scene_id INTEGER NOT NULL REFERENCES scene(id),
  shift_id INTEGER NOT NULL REFERENCES shift(id),
  loaded_at INTEGER, started_at INTEGER, ended_at INTEGER,
  widget_versions_json TEXT);                    -- versions resolved at load

CREATE TABLE gift_ledger (                       -- SOURCE OF TRUTH (salary)
  id INTEGER PRIMARY KEY,
  ts INTEGER NOT NULL,
  account_id INTEGER NOT NULL, shift_id INTEGER NOT NULL,
  scene_run_id INTEGER,                          -- NULL if nothing running
  msg_id INTEGER,                                -- TikTok id; NULL for manual
  gifter_id INTEGER, gifter_name TEXT,
  gift_id INTEGER, gift_name TEXT, repeat_count INTEGER,
  diamonds INTEGER NOT NULL,                     -- credited value (post-dedup)
  performer_id INTEGER,                          -- NULL = show-level: no game
                                                 -- running / unrouted; counts
                                                 -- in show totals (§16.1)
  source TEXT,                                   -- 'widget:<id>'|'manual'
  UNIQUE(msg_id, performer_id));                 -- exactly-once per gift+performer

CREATE INDEX ledger_shift ON gift_ledger(shift_id, performer_id);
CREATE INDEX ledger_run   ON gift_ledger(scene_run_id, performer_id);

CREATE TABLE widget_state (                      -- crash-resume snapshots
  scene_run_id INTEGER NOT NULL,
  scene_widget_id INTEGER NOT NULL,
  state_json TEXT, seq INTEGER, updated_at INTEGER,
  PRIMARY KEY (scene_run_id, scene_widget_id));

CREATE TABLE runtime (                           -- singleton
  id INTEGER PRIMARY KEY CHECK (id=1),
  active_account_id INTEGER, active_shift_id INTEGER,
  active_scene_run_id INTEGER, running INTEGER DEFAULT 0);

CREATE TABLE widget_bundle (
  type TEXT, version TEXT, path TEXT, manifest_json TEXT, installed_at INTEGER,
  PRIMARY KEY (type, version));

CREATE TABLE setting (key TEXT PRIMARY KEY, value TEXT);
-- keys: control_password_hash (optional), http_port, stt_enabled, …
```

Notes:

- **Totals are views over `gift_ledger`** (per run, per shift, per day if ever
  needed); in-memory caches serve the hot path and are rebuilt on boot.
- `scene_widget.config_json` is restored on **every** load (composition is
  durable); `widget_state` belongs to a **run** (game progress) and is only
  used for crash resume.
- Widget versions are resolved to the **latest installed** at scene load and
  recorded in `scene_run.widget_versions_json` (Q-E: updates apply only then).

---

## 13. Accounts, performers, shifts

- **Account** = evolved Profile (name + `@stream` + optional cookies) with
  performers, scenes, shifts. Loading an account tears down and re-creates the
  ttlive client + engine; performers/embeddings are loaded into memory once
  (no per-frame DB reads).
- **Performer** management (add / delete / rename / avatar / face landmark /
  reset) — verbs already exist in `face_gallery`/`face_tracker`; exposed over
  REST; persisted in SQLite.
- **Shift**: "New Shift" (native button) ends the current `shift` row and
  opens a new one; all running displays reset to 0 because current-shift SUMs
  now cover a new empty range. History stays forever.
- **Shift report (decided)**: `GET /api/shifts/:id/report` (also exportable to
  a file from the native app) aggregates the ledger into JSON — the shift
  total per performer plus the detail of how much was won in **each scene
  played**:

  ```json
  {
    "shift":  { "id": 12, "started_at": "…", "ended_at": "…" },
    "totals": [ { "performer": "Alexa", "diamonds": 13004 },
                { "performer": "Adel",  "diamonds": 5050 } ],
    "scenes": [
      { "scene": "Battle 1v1", "run_id": 88, "loaded_at": "…",
        "per_performer": [ { "performer": "Alexa", "diamonds": 9100 },
                           { "performer": "Adel",  "diamonds": 3900 } ] },
      { "scene": "Gift Race 6", "run_id": 89, "loaded_at": "…",
        "per_performer": [ { "performer": "Alexa", "diamonds": 3904 } ] }
    ]
  }
  ```

---

## 14. Scene lifecycle (compose → save → load → start)

```
   native "New scene" ──► Control view navigates to the empty scene editor
                          OBS view navigates to blank (transparent)
   operator composes  ──► drag widgets from menu, move/resize, configure
   "Save scene"       ──► PUT /api/scenes/:id/widgets  (layout+config → SQLite)
   native "Load scene"──► ends current run; creates scene_run;
                          `navigate` pushed to ALL pages (fresh HTML: new
                          widget versions take effect); snapshot on reconnect;
                          per-run scores = 0; onLoad(ctx, null)
   native "Start"     ──► onStart(ctx) in each app.js — begins a ROUND
                          (e.g. 3 min); timers run; gifts credit
   (between rounds)   ──► loser out, newcomer in; operator re-assigns the slot
                          (drag-drop or "Detect Faces"), presses Start again —
                          repeatable any number of times within the same run
   native "Stop"      ──► onStop; run marked ended
   (crash)            ──► §10 restart flow resumes the same run
```

**Start is round-scoped, not scene-scoped:** within one scene run the operator
presses Start once per round; per-scene diamond totals keep accumulating across
rounds (that is how rotated-out performers keep their ranking), while
round-local state (bar fill, timer) is widget state that each `onStart` resets.

Scene creation is normally done **before** going live (a freshly created scene
forces the OBS view blank, which viewers would see). Editing a *live* scene is
possible but intentionally not hidden from viewers — the operator's choice.

---

## 15. Face recognition & "Detect Faces"

Pipeline unchanged from DearTT (SCRFD detect + ArcFace embed + temporal vote +
click-to-correct), fed by the FFmpeg-decoded TikTok stream.

- On account load, roster + embeddings load from SQLite into the in-memory
  gallery; recognition never touches the DB per frame; corrections persist
  back.
- The native app continuously shows a box + best-guess identity over each
  face; the operator can verify at a glance that faces are detected correctly
  (and click-to-correct if not) **before** relying on Detect Faces.
- **"Detect Faces"** (Control view button, e.g. between battle rounds):
  `control.js`/`app.js` reads the tracked faces (`GET /api/faces` /
  `ctx.faces()`) → `[{performerId, name, x, y, w, h, similarity}]` from
  `FaceTracker::faces()`. The widget maps the recognized performers to slots
  by screen position (leftmost → left slot, …) and confirms the assignment
  through the normal intent path — same result as dragging the profile
  manually, just automatic.
- Positions come from the delayed TikTok stream (a few seconds behind), so
  newcomers should be in place briefly before pressing the button.

---

## 16. Native app main page: statistics, gift monitor, chat

The native window keeps DearTT's layout (video + chat + stats) and sharpens it
for show operation. Everything here is operator-facing; nothing of it is
streamed.

### 16.1 Statistics (activity health)

`StatsCollector` + ImPlot stay: viewers over time, **diamonds/min**,
likes/comments/joins per minute, per-gift breakdown, top gifters. Their job is
to tell the operator whether the live show has enough activity.

- **Headline number: TOTAL COINS of the current shift, in a big font.** It is
  the shift SUM over the gift ledger **including unattributed gifts** — coins
  sent while no game was running still belong to the show.
- **USD/currency conversion is deliberately not shown** (not important).
- Secondary counters (viewers, likes, per-scene totals) render normally.

### 16.2 Live gift monitor (main-page table)

A real-time table of gifts as they arrive (newest on top), fed by the same
post-dedup stream that feeds the ledger — what the operator sees always
matches accounting:

| @user | gift | name | amount | diamonds | → performer |
|---|---|---|---|---|---|
| @big.fan | (icon) | Rose | ×12 | 12 | (avatar) Alexa |
| @whale01 | (icon) | Lion | ×1 | 29,999 | (avatar) Adel |
| @newguy | (icon) | Raccoon | ×1 | 349 | — (show) |

- **amount** = streak/repeat count (streakable gifts arrive several at once);
  a combo still in progress updates its row live, and the **diamonds** value
  settles when the streak finalizes (the ledger value).
- **→ performer** shows the receiving performer's icon + name when a game
  attributed the gift; **— (show)** when unattributed (e.g. the game was not
  started).
- Gift icons come from the gift gallery (`IconCache`); user and performer
  pictures from the avatar caches.

### 16.3 Chat & defaults

DearTT's chat pane stays. EvoControl defaults: chat mode **Overlay** and
**"chat only"** checked (comments only; joins/likes/shares filtered out).
Both adjustable in the settings menu (§18).

---

## 17. User/gifter avatar API

Gift events carry `user.id` + `user.avatar_url` (`events.hpp:43-49`), but
widgets must not hit TikTok CDNs (CORS, rate limits, URL churn). EvoControl
serves:

- `GET /api/users/:id/avatar` — download-once, cache, serve locally (backed by
  `IconCache`/`AvatarFetcher` infrastructure). Used by `obs.js`/`control.js`
  for top-gifter thumbnails (reference frame shows them beside the timer).
- `ctx.userAvatar(userId)` — same cache, for `app.js`.

---

## 18. Settings & authentication

- `setting` table + a native **settings menu**: HTTP port, **optional
  Control-view password**, STT toggle, per-account cookies, chat defaults
  (mode **Overlay**, **"chat only"** on — §16.3).
- **OBS view + widget assets: no auth** (LAN-viewable by design).
- **Control view + REST mutations + control WS `hello`: password-gated when a
  password is set** (session cookie after a login form). Control is always
  used from localhost (Q-J), so this is defense-in-depth, not perimeter
  security.
- Server binds all interfaces (so LAN devices can render the OBS view); no
  TLS (LAN/localhost only).

---

## 19. Build-system impact

- **Add SQLite** (vendor `sqlite3.c` amalgamation, like civetweb is vendored).
- **Add `miniz`** (or similar) for `.evw`/zip widget install.
- **Expose QuickJS to the app target** — today it is linked PRIVATE inside
  `ttlive` (`ttlive-cpp/CMakeLists.txt:153`); link the `quickjs` target into
  the app too (same static lib, no duplication).
- **Keep** FFmpeg, miniaudio, ONNX Runtime, curl-impersonate, protobuf,
  civetweb, ImGui/ImPlot/GLFW/FreeType; STT stays toggleable.
- **Rename** `deartt`→`evocontrol`, `DEARTT_*`→`EVOCONTROL_*`; packaging
  scripts follow.
- `web/` grows into `web/obs/` + `web/control/` host pages (plain Lit ES
  modules served as-is; no bundler required initially) and a `widgets/`
  registry directory next to the executable.

---

## 20. Reliability & future latency tool

- **Dual transport on** (`use_websocket = use_polling = true`), dedup by
  `msg_id` — the reliability requirement, already implemented in ttlive-cpp.
- **curl-impersonate** stays (TikTok WAF requires Chrome TLS fingerprints).
- **Cookies per account** (`ttwid` required today; `sessionid` for restricted
  rooms) — stored on the account, injected into `ClientOptions.cookies`.
- Engine survives ttlive reconnects (ledger + `msg_id` idempotency make
  re-delivery harmless).
- **Latency/QR tool (future)**: the OBS host reserves a system overlay layer
  above widgets; EvoControl flashes a timestamp/QR there, finds it in the
  decoded TikTok stream, and reports glass-to-glass delay.

---

## 21. Implementation phases

1. **Foundation** — rename; vendor SQLite (+WAL); schema §12; migrate
   Profile→account, roster→performer; native console keeps video+faces.
2. **Ledger + shifts** — gift ledger writes from the (existing) event path,
   streak dedup, unattributed rows, New Shift, totals rebuild on boot; native
   **gift monitor table** + big **total-coins** display (§16). *Salary safety
   first.*
3. **Server** — REST CRUD; WS envelope + `hello`/`session` takeover +
   `snapshot`/`state`/`intent`/`navigate`; optional control password.
4. **Hosts + composition** — `web/obs` + `web/control` Lit hosts; widget
   registry + `.zip` drag-drop install; canvas editor (drag/move/z/Save);
   scene lifecycle (§14).
5. **Widget runtime** — QuickJS contexts + host API `ctx` (freeze the exact
   signatures here, against the real widgets); widget-private plane; snapshots.
6. **Reference widgets** — text, logo/image, **score-battle** (gift→side map,
   sticky routing, rounds + rotation, top gifters, separator), **ranking**
   (per-scene points subscription). Reproduce the §2 frame end-to-end.
7. **Detect Faces** — `GET /api/faces` / `ctx.faces()` → slot mapping in the
   battle widget.
8. **Hardening** — crash-resume drills, shift report JSON export, then audio
   clips / latency tool / STT removal as desired.

---

## 22. Design status & implementation-time details

**All design questions from rounds 1–3 are resolved** (§4). For the record,
the round-3 answers:

- **Q-D → resolved.** Host API capability set: user avatars, gift gallery,
  performer list, face-detector positions per performer (`ctx.faces()`, §9).
- **Q-K → resolved.** Shift report = JSON with the shift total per performer
  plus per-scene detail (§13).
- **Q-L → resolved.** Superseded control page gets a **"Take back control"**
  button; using it sends the other page to the same black "control lost"
  screen (§6.2).
- **Q-M → resolved.** Ranking widget defaults to **per-scene** points (§8.2).
- **Q-N → resolved.** No mystery attribution: battles are **rounds with
  rotation** — Start begins each round, the loser leaves, the operator
  re-assigns the freed slot (drag-drop or "Detect Faces"). Performers keep the
  diamonds they earned in earlier rounds of the scene (§2, §14).

Small items deliberately deferred to implementation (non-blocking):

1. **Exact host-API signatures** — payload field names/shapes for `ctx.*`,
   the gift object, and timer helpers are frozen while building the reference
   widgets (§21 phase 5).
2. **Unrouted-gift handling inside a widget** — e.g. a gift arriving before
   the gifter ever sent a routing gift: each widget decides (ignore → the
   gift stays a show-level `performer_id NULL` row, hold, or split). Either
   way the ledger records it and the show total includes it (§7, §16.1).
3. **Report file naming/location** when exported from the native app (e.g.
   `reports/<account>/<shift-date>.json`).
4. **`seq`/reconnect edge cases** (WS snapshot vs in-flight broadcasts) —
   standard engineering, settled in phase 3.
