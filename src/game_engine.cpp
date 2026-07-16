#include "game_engine.hpp"

#include <algorithm>
#include <chrono>

#include <nlohmann/json.hpp>
#include "store.hpp"
#include "ttlive/client.hpp"
#include "ttlive/events.hpp"
#include "widget_registry.hpp"
#include "widget_runtime.hpp"

using nlohmann::json;

namespace evo {

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

GameEngine::GameEngine(Store& store, WidgetRegistry& registry)
    : store_(store), registry_(registry) {}

GameEngine::~GameEngine() { stop(); }

void GameEngine::start() {
    quit_ = false;
    thread_ = std::thread([this] { threadMain(); });
}

void GameEngine::stop() {
    if (!thread_.joinable()) return;
    quit_ = true;
    qcv_.notify_all();
    thread_.join();
}

void GameEngine::enqueue(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(qmutex_);
        queue_.push_back({std::move(fn)});
    }
    qcv_.notify_all();
}

void GameEngine::threadMain() {
    using clock = std::chrono::steady_clock;
    auto lastTick = clock::now();
    const auto tickInterval = std::chrono::milliseconds(100);  // ~10 Hz (§9)
    auto lastSnapshot = clock::now();
    while (!quit_) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(qmutex_);
            qcv_.wait_for(lk, std::chrono::milliseconds(20),
                          [this] { return quit_ || !queue_.empty(); });
            if (quit_) break;
            if (!queue_.empty()) {
                job = std::move(queue_.front().fn);
                queue_.pop_front();
            }
        }
        if (job) job();

        auto now = clock::now();
        if (roundRunning_ && now - lastTick >= tickInterval) {
            lastTick = now;
            tick();  // onTick passes a fixed ~100 ms dt to widgets
        } else if (!roundRunning_) {
            lastTick = now;
        }
        // Periodic widget-state snapshot for crash resume (§10.2).
        if (sceneRunId_ && now - lastSnapshot >= std::chrono::seconds(1)) {
            lastSnapshot = now;
            snapshotAll();
        }
    }
}

// ---------------------------------------------------------------------------
// Posting (thread-safe entry points)
// ---------------------------------------------------------------------------
void GameEngine::postLoadAccount(int64_t id) {
    enqueue([this, id] { loadAccountImpl(id); });
}
void GameEngine::postNewShift() {
    enqueue([this] { newShiftImpl(); });
}
void GameEngine::postLoadScene(int64_t sceneId, bool resume, int64_t runId) {
    enqueue([this, sceneId, resume, runId] {
        loadSceneImpl(sceneId, resume, runId);
    });
}
void GameEngine::postStart() {
    enqueue([this] { startImpl(); });
}
void GameEngine::postStop() {
    enqueue([this] { stopImpl(); });
}
void GameEngine::postIntent(int64_t instanceId, const std::string& intentJson) {
    enqueue([this, instanceId, intentJson] {
        intentImpl(instanceId, intentJson);
    });
}
void GameEngine::postFaces(const std::vector<EngineFace>& faces) {
    enqueue([this, faces] { facesImpl(faces); });
}
void GameEngine::postAdjustPoints(int64_t performerId, int64_t diamonds,
                                  const std::string& reason) {
    enqueue([this, performerId, diamonds, reason] {
        LedgerRow row;
        row.ts = nowMs();
        row.accountId = accountId_;
        row.shiftId = shiftId_;
        row.sceneRunId = sceneRunId_ ? std::optional<int64_t>(sceneRunId_)
                                     : std::nullopt;
        row.diamonds = diamonds;
        row.performerId = performerId;
        row.giftName = reason;
        row.source = "manual";
        if (store_.appendLedger(row)) notifyPointsChanged();
    });
}
void GameEngine::postEvent(const ttlive::Event& e) {
    auto copy = std::make_shared<ttlive::Event>(e);
    enqueue([this, copy] { eventImpl(copy); });
}

void GameEngine::postGiftGallery(const std::vector<ttlive::GiftInfo>& gifts) {
    // Copy into a plain vector to move into the closure.
    std::vector<std::tuple<int64_t, int32_t, std::string, std::string>> g;
    g.reserve(gifts.size());
    for (const auto& gi : gifts)
        g.emplace_back(gi.id, gi.diamond_count, gi.name, gi.icon_url);
    enqueue([this, g] {
        std::lock_guard<std::mutex> lk(shared_);
        for (const auto& [id, dia, name, icon] : g) {
            giftPrices_[id] = dia;
            giftNames_[id] = name;
            giftIcons_[id] = icon;
        }
    });
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void GameEngine::teardownRun() {
    if (sceneRunId_) snapshotAll();
    if (sceneRunId_) store_.markSceneRunEnded(sceneRunId_);
    instances_.clear();
    instanceMeta_.clear();
    instanceSeq_.clear();
    lastState_.clear();
    subscriptions_.clear();
    finalizedMsgs_.clear();
    sceneRunId_ = 0;
    sceneId_ = 0;
    roundRunning_ = false;
}

void GameEngine::loadAccountImpl(int64_t accountId) {
    teardownRun();
    accountId_ = accountId;
    // Ensure an open shift.
    auto sh = store_.currentShift(accountId);
    if (!sh) sh = store_.newShift(accountId);
    shiftId_ = sh ? sh->id : 0;
    // Load roster into the shared plane.
    {
        std::lock_guard<std::mutex> lk(shared_);
        performers_ = store_.performers(accountId);
    }
    RuntimeState rt;
    rt.activeAccountId = accountId_;
    rt.activeShiftId = shiftId_;
    rt.running = false;
    store_.setRuntime(rt);
    if (broadcast_) broadcast_("all", 0, "shared", performersJson());
}

void GameEngine::newShiftImpl() {
    if (!accountId_) return;
    auto sh = store_.newShift(accountId_);
    shiftId_ = sh ? sh->id : shiftId_;
    RuntimeState rt = store_.runtime();
    rt.activeShiftId = shiftId_;
    store_.setRuntime(rt);
    notifyPointsChanged();
}

void GameEngine::loadSceneImpl(int64_t sceneId, bool resume,
                               int64_t resumeRunId) {
    teardownRun();
    sceneId_ = sceneId;
    // Resolve widget versions at load (§12 note) and record them.
    auto widgets = store_.sceneWidgets(sceneId);
    json versions = json::object();
    for (const auto& w : widgets) {
        auto b = registry_.manifest(w.widgetType);
        if (b) versions[w.widgetType] = b->version;
    }

    SceneRun run;
    if (resume && resumeRunId) {
        auto r = store_.sceneRun(resumeRunId);
        if (r) run = *r;
    } else {
        auto r = store_.newSceneRun(sceneId, shiftId_, versions.dump());
        if (r) run = *r;
    }
    sceneRunId_ = run.id;

    // Instantiate app.js per widget.
    for (const auto& w : widgets) {
        std::string appJs;
        {
            auto bytes = registry_.readBundleFile(w.widgetType, "", "app.js");
            appJs.assign(bytes.begin(), bytes.end());
        }
        auto rt = std::make_unique<WidgetRuntime>();
        HostApi host;
        int64_t inst = w.id;
        host.performers = [this] { return performersJson(); };
        host.giftGallery = [this] { return giftGalleryJson(); };
        host.userAvatar = [this](int64_t id) { return userAvatarUrl(id); };
        host.faces = [this] { return facesJson(); };
        host.points = [this](const std::string& s) { return pointsJson(s); };
        host.creditGift = [this, inst](int64_t pid,
                                       const std::string& giftJson) {
            // Widget-authoritative salary credit.
            json g = json::parse(giftJson, nullptr, false);
            LedgerRow row;
            row.ts = nowMs();
            row.accountId = accountId_;
            row.shiftId = shiftId_;
            row.sceneRunId = sceneRunId_;
            if (g.is_object()) {
                if (g.contains("msgId") && g["msgId"].is_number())
                    row.msgId = g["msgId"].get<int64_t>();
                if (g.contains("userId") && g["userId"].is_number()) {
                    row.gifterId = g["userId"].get<int64_t>();
                }
                row.gifterName = g.value("userName", "");
                if (g.contains("giftId") && g["giftId"].is_number())
                    row.giftId = g["giftId"].get<int64_t>();
                row.giftName = g.value("name", "");
                row.repeatCount = g.value("repeat", (int64_t)1);
                row.diamonds = g.value("diamonds", (int64_t)0);
            }
            row.performerId = pid;
            row.source = "widget:" + std::to_string(inst);
            if (store_.appendLedger(row)) notifyPointsChanged();
        };
        host.adjustPoints = [this, inst](int64_t pid, int64_t d,
                                         const std::string& reason) {
            LedgerRow row;
            row.ts = nowMs();
            row.accountId = accountId_;
            row.shiftId = shiftId_;
            row.sceneRunId = sceneRunId_;
            row.diamonds = d;
            row.performerId = pid;
            row.giftName = reason;
            row.source = "manual";
            if (store_.appendLedger(row)) notifyPointsChanged();
        };
        host.emit = [this, inst](const std::string& view,
                                 const std::string& dataJson) {
            if (broadcast_) broadcast_(view, inst, "state", dataJson);
        };
        host.subscribe = [this, inst](const std::string& topic) {
            subscriptions_[inst].insert(topic);
        };
        host.persist = [this, inst] {
            if (!sceneRunId_) return;
            auto it = instances_.find(inst);
            if (it == instances_.end()) return;
            store_.saveWidgetState(sceneRunId_, inst, it->second->saveState(),
                                   ++instanceSeq_[inst]);
        };

        std::string cfg = w.configJson.empty() ? "{}" : w.configJson;
        std::string saved;
        if (resume) {
            auto s = store_.loadWidgetState(sceneRunId_, w.id);
            if (s) saved = *s;
        }
        rt->load(inst, appJs, cfg, saved, host);
        instances_[inst] = std::move(rt);
        instanceMeta_[inst] = w;
        instanceSeq_[inst] = 0;
        publishInstance(inst);
    }

    RuntimeState rtst = store_.runtime();
    rtst.activeSceneRunId = sceneRunId_;
    rtst.running = false;
    store_.setRuntime(rtst);

    // Tell all pages to (re)load the fresh host page (§14: widget updates take
    // effect only at scene load; both views load a fresh HTML page).
    if (broadcast_) {
        json nav = {{"obs", "/obs"}, {"control", "/control"}};
        broadcast_("all", 0, "navigate", nav.dump());
    }
    notifyPointsChanged();
}

void GameEngine::startImpl() {
    if (!sceneRunId_) return;
    store_.markSceneRunStarted(sceneRunId_);
    roundRunning_ = true;
    RuntimeState rt = store_.runtime();
    rt.running = true;
    store_.setRuntime(rt);
    for (auto& [id, inst] : instances_) {
        inst->onStart();
        publishInstance(id);
    }
}

void GameEngine::stopImpl() {
    roundRunning_ = false;
    for (auto& [id, inst] : instances_) {
        inst->onStop();
        publishInstance(id);
    }
    snapshotAll();
    RuntimeState rt = store_.runtime();
    rt.running = false;
    store_.setRuntime(rt);
}

void GameEngine::tick() {
    for (auto& [id, inst] : instances_) {
        inst->onTick(100.0);
        publishInstance(id);
    }
}

// ---------------------------------------------------------------------------
// Events + streak dedup + ledger (§7)
// ---------------------------------------------------------------------------
void GameEngine::eventImpl(std::shared_ptr<ttlive::Event> e) {
    if (e->type != ttlive::EventType::Gift) return;

    // Streak dedup: a streakable combo is delivered many times while
    // gift_streaking is true; we only finalize once (when the streak ends or
    // for non-streakable gifts). Idempotency by msg_id also guards restarts.
    bool streakable = e->gift_type == 1;
    if (streakable && e->gift_streaking) {
        // Combo in progress → update the monitor row live but don't credit yet.
        {
            std::lock_guard<std::mutex> lk(monMutex_);
            GiftMonitorRow row;
            row.ts = nowMs();
            row.gifterId = e->user.id;
            row.gifterName = e->user.nickname.empty() ? e->user.unique_id
                                                      : e->user.nickname;
            row.giftId = e->gift_id;
            row.giftName = e->gift_name;
            row.giftIconUrl = e->gift_icon_url;
            row.amount = e->repeat_count;
            row.diamonds = e->diamond_count * e->repeat_count;
            row.performerId = 0;
            if (!monitor_.empty() && monitor_.front().gifterId == row.gifterId &&
                monitor_.front().giftId == row.giftId &&
                monitor_.front().performerId == 0)
                monitor_.front() = row;  // update live
            else
                monitor_.push_front(row);
            if (monitor_.size() > 500) monitor_.pop_back();
        }
        return;
    }

    // Finalize (once). Guard by msg_id.
    if (e->msg_id && finalizedMsgs_.count(e->msg_id)) return;
    if (e->msg_id) finalizedMsgs_.insert(e->msg_id);

    int64_t diamonds = (int64_t)e->diamond_count * std::max(1, e->repeat_count);
    if (diamonds == 0 && e->gift_id) {
        std::lock_guard<std::mutex> lk(shared_);
        auto it = giftPrices_.find(e->gift_id);
        if (it != giftPrices_.end())
            diamonds = it->second * std::max(1, e->repeat_count);
    }

    // Build the gift object handed to widgets (§9 onGift).
    json gift;
    gift["msgId"] = e->msg_id;
    gift["userId"] = e->user.id;
    gift["userName"] =
        e->user.nickname.empty() ? e->user.unique_id : e->user.nickname;
    gift["giftId"] = e->gift_id;
    gift["name"] = e->gift_name;
    gift["repeat"] = std::max(1, e->repeat_count);
    gift["diamonds"] = diamonds;
    // Remember the gifter's avatar for the shared plane.
    if (e->user.id && !e->user.avatar_url.empty()) {
        std::lock_guard<std::mutex> lk(shared_);
        userAvatars_[e->user.id] = e->user.avatar_url;
    }

    // Route to every instance that subscribes to "gift". A widget decides
    // attribution and calls ctx.creditGift → ledger (which writes an attributed
    // row). We track whether ANY attribution happened for this msg_id.
    bool anyRunning = roundRunning_ && !instances_.empty();
    size_t before = 0;
    // (We can't easily know per-widget credit without inspecting the ledger;
    // instead we check whether the ledger gained an attributed row for msg_id.)
    if (anyRunning) {
        for (auto& [id, inst] : instances_) {
            inst->onGift(gift.dump());
            publishInstance(id);
        }
    }

    // Determine attribution + performer for the monitor row by querying the
    // ledger for this msg_id (attributed rows have performer_id NOT NULL).
    int64_t attributedPerformer = 0;
    std::string attributedName;
    {
        auto s = store_.db().prepare(
            "SELECT performer_id FROM gift_ledger WHERE msg_id=? AND "
            "performer_id IS NOT NULL LIMIT 1");
        s.bind(1, (int64_t)e->msg_id);
        if (e->msg_id && s.step() && !s.columnIsNull(0)) {
            attributedPerformer = s.columnInt(0);
        }
    }
    if (attributedPerformer) {
        std::lock_guard<std::mutex> lk(shared_);
        for (const auto& p : performers_)
            if (p.id == attributedPerformer) attributedName = p.name;
    }

    // Unattributed gifts are first-class show coins (§7/§16.1): if no widget
    // credited a performer, write a performer_id NULL row so the show total
    // includes it. Exactly-once by (msg_id, NULL).
    if (!attributedPerformer) {
        LedgerRow row;
        row.ts = nowMs();
        row.accountId = accountId_;
        row.shiftId = shiftId_;
        row.sceneRunId = sceneRunId_ ? std::optional<int64_t>(sceneRunId_)
                                     : std::nullopt;
        row.msgId = e->msg_id ? std::optional<int64_t>(e->msg_id)
                              : std::nullopt;
        row.gifterId = e->user.id ? std::optional<int64_t>(e->user.id)
                                  : std::nullopt;
        row.gifterName =
            e->user.nickname.empty() ? e->user.unique_id : e->user.nickname;
        row.giftId = e->gift_id ? std::optional<int64_t>(e->gift_id)
                                : std::nullopt;
        row.giftName = e->gift_name;
        row.repeatCount = std::max(1, e->repeat_count);
        row.diamonds = diamonds;
        row.performerId = std::nullopt;
        row.source = "show";
        if (store_.appendLedger(row)) notifyPointsChanged();
    }

    // Monitor row (finalized).
    {
        std::lock_guard<std::mutex> lk(monMutex_);
        GiftMonitorRow row;
        row.ts = nowMs();
        row.gifterId = e->user.id;
        row.gifterName =
            e->user.nickname.empty() ? e->user.unique_id : e->user.nickname;
        row.giftId = e->gift_id;
        row.giftName = e->gift_name;
        row.giftIconUrl = e->gift_icon_url;
        row.amount = std::max(1, e->repeat_count);
        row.diamonds = diamonds;
        row.performerId = attributedPerformer;
        row.performerName = attributedName;
        // Replace a matching in-progress row if present.
        if (!monitor_.empty() && monitor_.front().gifterId == row.gifterId &&
            monitor_.front().giftId == row.giftId &&
            monitor_.front().performerId == 0 && row.performerId == 0)
            monitor_.front() = row;
        else
            monitor_.push_front(row);
        if (monitor_.size() > 500) monitor_.pop_back();
    }
    (void)before;
}

void GameEngine::facesImpl(std::vector<EngineFace> faces) {
    std::lock_guard<std::mutex> lk(shared_);
    // Resolve performerId by name when the caller only supplied a name.
    for (auto& f : faces) {
        if (f.performerId == 0 && !f.name.empty()) {
            for (const auto& p : performers_)
                if (p.name == f.name) {
                    f.performerId = p.id;
                    break;
                }
        }
    }
    faces_ = std::move(faces);
}

void GameEngine::intentImpl(int64_t instanceId, std::string intentJson) {
    auto it = instances_.find(instanceId);
    if (it == instances_.end()) return;
    it->second->onIntent(intentJson);
    publishInstance(instanceId);
}

// ---------------------------------------------------------------------------
// Shared plane + broadcasting
// ---------------------------------------------------------------------------
std::string GameEngine::performersJson() {
    std::lock_guard<std::mutex> lk(shared_);
    json arr = json::array();
    for (const auto& p : performers_) {
        json e;
        e["id"] = p.id;
        e["name"] = p.name;
        e["avatarUrl"] =
            p.avatarPath.empty() ? "" : ("/api/performers/" +
                                         std::to_string(p.id) + "/avatar");
        arr.push_back(e);
    }
    return json({{"topic", "performers"}, {"performers", arr}}).dump();
}

std::string GameEngine::giftGalleryJson() {
    std::lock_guard<std::mutex> lk(shared_);
    json arr = json::array();
    for (const auto& [id, dia] : giftPrices_) {
        json e;
        e["giftId"] = id;
        e["name"] = giftNames_.count(id) ? giftNames_[id] : "";
        e["diamonds"] = dia;
        e["iconUrl"] = giftIcons_.count(id) ? giftIcons_[id] : "";
        arr.push_back(e);
    }
    return arr.dump();
}

std::string GameEngine::pointsJson(const std::string& scope) {
    json obj = json::object();
    if (scope == "shift") {
        auto totals = store_.shiftTotals(shiftId_);
        for (const auto& [pid, d] : totals) obj[std::to_string(pid)] = d;
    } else {
        auto totals = store_.sceneRunTotals(sceneRunId_);
        for (const auto& [pid, d] : totals) obj[std::to_string(pid)] = d;
    }
    return obj.dump();
}

std::string GameEngine::userAvatarUrl(int64_t userId) {
    std::lock_guard<std::mutex> lk(shared_);
    auto it = userAvatars_.find(userId);
    if (it != userAvatars_.end())
        return "/api/users/" + std::to_string(userId) + "/avatar";
    return "";
}

std::string GameEngine::facesJson() {
    std::lock_guard<std::mutex> lk(shared_);
    json arr = json::array();
    for (const auto& f : faces_) {
        json e;
        e["performerId"] = f.performerId;
        e["name"] = f.name;
        e["x"] = f.x;
        e["y"] = f.y;
        e["w"] = f.w;
        e["h"] = f.h;
        e["similarity"] = f.similarity;
        arr.push_back(e);
    }
    return arr.dump();
}

void GameEngine::publishInstance(int64_t instanceId) {
    auto it = instances_.find(instanceId);
    if (it == instances_.end()) return;
    std::string published = it->second->takePublishedState();
    if (published.empty()) return;
    int64_t seq = ++instanceSeq_[instanceId];
    lastState_[instanceId] = published;
    json env;
    env["seq"] = seq;
    env["state"] = json::parse(published, nullptr, false);
    if (broadcast_) broadcast_("all", instanceId, "state", env.dump());
}

void GameEngine::notifyPointsChanged() {
    // Shared plane "points" broadcast + push onShared to subscribed widgets.
    std::string sceneRunPts = pointsJson("scene_run");
    json shared;
    shared["topic"] = "points";
    shared["scene_run"] = json::parse(sceneRunPts, nullptr, false);
    shared["shift"] = json::parse(pointsJson("shift"), nullptr, false);
    if (broadcast_) broadcast_("all", 0, "shared", shared.dump());
    for (auto& [id, topics] : subscriptions_) {
        if (topics.count("points")) {
            auto it = instances_.find(id);
            if (it != instances_.end()) {
                it->second->onShared("points", shared.dump());
                publishInstance(id);
            }
        }
    }
}

void GameEngine::snapshotAll() {
    if (!sceneRunId_) return;
    for (auto& [id, inst] : instances_)
        store_.saveWidgetState(sceneRunId_, id, inst->saveState(),
                               ++instanceSeq_[id]);
}

// ---------------------------------------------------------------------------
// Queries (thread-safe)
// ---------------------------------------------------------------------------
std::string GameEngine::snapshotJson() {
    // Assemble on the engine thread would be cleaner, but lastState_ is only
    // written on the engine thread and read here; for the server path we build
    // best-effort from lastState_. Wrap under qmutex_ ordering via a synchronous
    // post would be ideal; the snapshot is advisory (views also get live state).
    json instances = json::array();
    for (const auto& [id, meta] : instanceMeta_) {
        json e;
        e["instance"] = id;
        e["type"] = meta.widgetType;
        auto it = lastState_.find(id);
        e["state"] = (it != lastState_.end() && !it->second.empty())
                         ? json::parse(it->second, nullptr, false)
                         : json::object();
        auto sit = instanceSeq_.find(id);
        e["seq"] = sit != instanceSeq_.end() ? sit->second : 0;
        instances.push_back(e);
    }
    json out;
    out["instances"] = instances;
    out["points"] = {
        {"scene_run", json::parse(pointsJson("scene_run"), nullptr, false)},
        {"shift", json::parse(pointsJson("shift"), nullptr, false)}};
    out["performers"] = json::parse(performersJson(), nullptr, false);
    return out.dump();
}

std::string GameEngine::giftGalleryJsonArray() { return giftGalleryJson(); }
std::string GameEngine::facesJsonArray() { return facesJson(); }

std::vector<GiftMonitorRow> GameEngine::giftMonitor() {
    std::lock_guard<std::mutex> lk(monMutex_);
    return std::vector<GiftMonitorRow>(monitor_.begin(), monitor_.end());
}

int64_t GameEngine::shiftCoins() { return store_.shiftCoins(shiftId_); }
int64_t GameEngine::activeSceneRunId() { return sceneRunId_; }
bool GameEngine::running() { return roundRunning_.load(); }

}  // namespace evo
