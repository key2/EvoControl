#include "store.hpp"

#include <chrono>
#include <cstring>

#include <nlohmann/json.hpp>

namespace evo {

using nlohmann::json;

int64_t Store::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool Store::open(const std::string& path) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_.open(path)) return false;
    rebuildTotals();
    return true;
}

void Store::close() {
    std::lock_guard<std::mutex> lk(mutex_);
    db_.close();
    runTotals_.clear();
    shiftPerfTotals_.clear();
    shiftCoins_.clear();
}

bool Store::isOpen() {
    std::lock_guard<std::mutex> lk(mutex_);
    return db_.isOpen();
}

std::string Store::error() {
    std::lock_guard<std::mutex> lk(mutex_);
    return db_.error();
}

// ---------------------------------------------------------------------------
// Accounts
// ---------------------------------------------------------------------------
static Account readAccount(Stmt& s) {
    Account a;
    a.id = s.columnInt(0);
    a.name = s.columnText(1);
    a.stream = s.columnText(2);
    a.cookies = s.columnText(3);
    a.createdAt = s.columnInt(4);
    return a;
}

std::vector<Account> Store::accounts() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<Account> out;
    auto s = db_.prepare(
        "SELECT id,name,stream,cookies,created_at FROM account ORDER BY name");
    while (s.step()) out.push_back(readAccount(s));
    return out;
}

std::optional<Account> Store::account(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "SELECT id,name,stream,cookies,created_at FROM account WHERE id=?");
    s.bind(1, id);
    if (s.step()) return readAccount(s);
    return std::nullopt;
}

std::optional<Account> Store::accountByName(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "SELECT id,name,stream,cookies,created_at FROM account WHERE name=?");
    s.bind(1, name);
    if (s.step()) return readAccount(s);
    return std::nullopt;
}

std::optional<Account> Store::createAccount(const std::string& name,
                                            const std::string& stream,
                                            const std::string& cookies) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "INSERT OR IGNORE INTO account(name,stream,cookies,created_at) "
        "VALUES(?,?,?,?)");
    s.bind(1, name).bind(2, stream).bind(3, cookies).bind(4, nowMs());
    s.step();
    auto g = db_.prepare(
        "SELECT id,name,stream,cookies,created_at FROM account WHERE name=?");
    g.bind(1, name);
    if (g.step()) return readAccount(g);
    return std::nullopt;
}

bool Store::updateAccount(const Account& a) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "UPDATE account SET name=?,stream=?,cookies=? WHERE id=?");
    s.bind(1, a.name).bind(2, a.stream).bind(3, a.cookies).bind(4, a.id);
    return s.step() || true;
}

bool Store::deleteAccount(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare("DELETE FROM account WHERE id=?");
    s.bind(1, id);
    s.step();
    return true;
}

// ---------------------------------------------------------------------------
// Performers
// ---------------------------------------------------------------------------
static Performer readPerformer(Stmt& s) {
    Performer p;
    p.id = s.columnInt(0);
    p.accountId = s.columnInt(1);
    p.name = s.columnText(2);
    p.avatarPath = s.columnText(3);
    auto blob = s.columnBlob(4);
    if (!blob.empty() && blob.size() % sizeof(float) == 0) {
        p.faceEmbedding.resize(blob.size() / sizeof(float));
        std::memcpy(p.faceEmbedding.data(), blob.data(), blob.size());
    }
    p.faceQuality = (float)s.columnDouble(5);
    p.tiktokUser = s.columnText(6);
    return p;
}

std::vector<Performer> Store::performers(int64_t accountId) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<Performer> out;
    auto s = db_.prepare(
        "SELECT id,account_id,name,avatar_path,face_embedding,face_quality,"
        "tiktok_user FROM performer WHERE account_id=? ORDER BY name");
    s.bind(1, accountId);
    while (s.step()) out.push_back(readPerformer(s));
    return out;
}

std::optional<Performer> Store::performer(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "SELECT id,account_id,name,avatar_path,face_embedding,face_quality,"
        "tiktok_user FROM performer WHERE id=?");
    s.bind(1, id);
    if (s.step()) return readPerformer(s);
    return std::nullopt;
}

std::optional<Performer> Store::createPerformer(int64_t accountId,
                                                const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "INSERT OR IGNORE INTO performer(account_id,name) VALUES(?,?)");
    s.bind(1, accountId).bind(2, name);
    s.step();
    auto g = db_.prepare(
        "SELECT id,account_id,name,avatar_path,face_embedding,face_quality,"
        "tiktok_user FROM performer WHERE account_id=? AND name=?");
    g.bind(1, accountId).bind(2, name);
    if (g.step()) return readPerformer(g);
    return std::nullopt;
}

bool Store::renamePerformer(int64_t id, const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare("UPDATE performer SET name=? WHERE id=?");
    s.bind(1, name).bind(2, id);
    s.step();
    return true;
}

bool Store::setPerformerAvatar(int64_t id, const std::string& path) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare("UPDATE performer SET avatar_path=? WHERE id=?");
    s.bind(1, path).bind(2, id);
    s.step();
    return true;
}

bool Store::setPerformerTiktokUser(int64_t id, const std::string& user) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare("UPDATE performer SET tiktok_user=? WHERE id=?");
    s.bind(1, user).bind(2, id);
    s.step();
    return true;
}

bool Store::setPerformerFace(int64_t id, const std::vector<float>& emb,
                             float quality) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "UPDATE performer SET face_embedding=?,face_quality=? WHERE id=?");
    s.bindBlob(1, emb.data(), emb.size() * sizeof(float))
        .bind(2, (double)quality)
        .bind(3, id);
    s.step();
    return true;
}

bool Store::resetPerformerFace(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "UPDATE performer SET face_embedding=NULL,face_quality=NULL WHERE id=?");
    s.bind(1, id);
    s.step();
    return true;
}

bool Store::deletePerformer(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare("DELETE FROM performer WHERE id=?");
    s.bind(1, id);
    s.step();
    return true;
}

// ---------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------
static Scene readScene(Stmt& s) {
    Scene sc;
    sc.id = s.columnInt(0);
    sc.accountId = s.columnInt(1);
    sc.name = s.columnText(2);
    sc.ordering = s.columnInt(3);
    return sc;
}

std::vector<Scene> Store::scenes(int64_t accountId) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<Scene> out;
    auto s = db_.prepare(
        "SELECT id,account_id,name,ordering FROM scene WHERE account_id=? "
        "ORDER BY ordering,name");
    s.bind(1, accountId);
    while (s.step()) out.push_back(readScene(s));
    return out;
}

std::optional<Scene> Store::scene(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "SELECT id,account_id,name,ordering FROM scene WHERE id=?");
    s.bind(1, id);
    if (s.step()) return readScene(s);
    return std::nullopt;
}

std::optional<Scene> Store::createScene(int64_t accountId,
                                        const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "INSERT OR IGNORE INTO scene(account_id,name,ordering) VALUES(?,?,"
        "(SELECT COALESCE(MAX(ordering),0)+1 FROM scene WHERE account_id=?))");
    s.bind(1, accountId).bind(2, name).bind(3, accountId);
    s.step();
    auto g = db_.prepare(
        "SELECT id,account_id,name,ordering FROM scene WHERE account_id=? AND "
        "name=?");
    g.bind(1, accountId).bind(2, name);
    if (g.step()) return readScene(g);
    return std::nullopt;
}

bool Store::renameScene(int64_t id, const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare("UPDATE scene SET name=? WHERE id=?");
    s.bind(1, name).bind(2, id);
    s.step();
    return true;
}

bool Store::deleteScene(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare("DELETE FROM scene WHERE id=?");
    s.bind(1, id);
    s.step();
    return true;
}

static SceneWidget readSceneWidget(Stmt& s) {
    SceneWidget w;
    w.id = s.columnInt(0);
    w.sceneId = s.columnInt(1);
    w.widgetType = s.columnText(2);
    w.x = s.columnDouble(3);
    w.y = s.columnDouble(4);
    w.w = s.columnDouble(5);
    w.h = s.columnDouble(6);
    w.z = s.columnInt(7);
    w.configJson = s.columnText(8);
    return w;
}

std::optional<Scene> Store::duplicateScene(int64_t id,
                                           const std::string& newName) {
    // createScene + copy widgets. Reuse locked methods carefully: unlock first.
    std::optional<Scene> src = scene(id);
    if (!src) return std::nullopt;
    auto dst = createScene(src->accountId, newName);
    if (!dst) return std::nullopt;
    auto widgets = sceneWidgets(id);
    for (auto w : widgets) {
        w.sceneId = dst->id;
        w.id = 0;
        addSceneWidget(w);
    }
    return dst;
}

std::vector<SceneWidget> Store::sceneWidgets(int64_t sceneId) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<SceneWidget> out;
    auto s = db_.prepare(
        "SELECT id,scene_id,widget_type,x,y,w,h,z,config_json FROM "
        "scene_widget WHERE scene_id=? ORDER BY z,id");
    s.bind(1, sceneId);
    while (s.step()) out.push_back(readSceneWidget(s));
    return out;
}

std::optional<SceneWidget> Store::sceneWidget(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "SELECT id,scene_id,widget_type,x,y,w,h,z,config_json FROM "
        "scene_widget WHERE id=?");
    s.bind(1, id);
    if (s.step()) return readSceneWidget(s);
    return std::nullopt;
}

std::optional<SceneWidget> Store::addSceneWidget(const SceneWidget& w) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "INSERT INTO scene_widget(scene_id,widget_type,x,y,w,h,z,config_json) "
        "VALUES(?,?,?,?,?,?,?,?)");
    s.bind(1, w.sceneId)
        .bind(2, w.widgetType)
        .bind(3, w.x)
        .bind(4, w.y)
        .bind(5, w.w)
        .bind(6, w.h)
        .bind(7, w.z)
        .bind(8, w.configJson.empty() ? std::string("{}") : w.configJson);
    s.step();
    SceneWidget out = w;
    out.id = db_.lastInsertRowId();
    return out;
}

bool Store::updateSceneWidget(const SceneWidget& w) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "UPDATE scene_widget SET widget_type=?,x=?,y=?,w=?,h=?,z=?,config_json=?"
        " WHERE id=?");
    s.bind(1, w.widgetType)
        .bind(2, w.x)
        .bind(3, w.y)
        .bind(4, w.w)
        .bind(5, w.h)
        .bind(6, w.z)
        .bind(7, w.configJson.empty() ? std::string("{}") : w.configJson)
        .bind(8, w.id);
    s.step();
    return true;
}

bool Store::deleteSceneWidget(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare("DELETE FROM scene_widget WHERE id=?");
    s.bind(1, id);
    s.step();
    return true;
}

bool Store::saveSceneWidgets(int64_t sceneId,
                             std::vector<SceneWidget>& widgets) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!db_.begin()) return false;
    {
        auto del = db_.prepare("DELETE FROM scene_widget WHERE scene_id=?");
        del.bind(1, sceneId);
        del.step();
        for (auto& w : widgets) {
            auto s = db_.prepare(
                "INSERT INTO scene_widget(scene_id,widget_type,x,y,w,h,z,"
                "config_json) VALUES(?,?,?,?,?,?,?,?)");
            s.bind(1, sceneId)
                .bind(2, w.widgetType)
                .bind(3, w.x)
                .bind(4, w.y)
                .bind(5, w.w)
                .bind(6, w.h)
                .bind(7, w.z)
                .bind(8, w.configJson.empty() ? std::string("{}")
                                              : w.configJson);
            s.step();
            w.id = db_.lastInsertRowId();
            w.sceneId = sceneId;
        }
    }
    return db_.commit();
}

// ---------------------------------------------------------------------------
// Shifts
// ---------------------------------------------------------------------------
static Shift readShift(Stmt& s) {
    Shift sh;
    sh.id = s.columnInt(0);
    sh.accountId = s.columnInt(1);
    sh.startedAt = s.columnInt(2);
    sh.endedAt = s.columnOptInt(3);
    return sh;
}

std::optional<Shift> Store::currentShift(int64_t accountId) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "SELECT id,account_id,started_at,ended_at FROM shift WHERE "
        "account_id=? AND ended_at IS NULL ORDER BY started_at DESC LIMIT 1");
    s.bind(1, accountId);
    if (s.step()) return readShift(s);
    return std::nullopt;
}

std::optional<Shift> Store::newShift(int64_t accountId) {
    std::lock_guard<std::mutex> lk(mutex_);
    int64_t now = nowMs();
    {
        auto e = db_.prepare(
            "UPDATE shift SET ended_at=? WHERE account_id=? AND ended_at IS "
            "NULL");
        e.bind(1, now).bind(2, accountId);
        e.step();
    }
    {
        auto s = db_.prepare(
            "INSERT INTO shift(account_id,started_at) VALUES(?,?)");
        s.bind(1, accountId).bind(2, now);
        s.step();
    }
    int64_t id = db_.lastInsertRowId();
    auto g = db_.prepare(
        "SELECT id,account_id,started_at,ended_at FROM shift WHERE id=?");
    g.bind(1, id);
    if (g.step()) return readShift(g);
    return std::nullopt;
}

std::optional<Shift> Store::shift(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "SELECT id,account_id,started_at,ended_at FROM shift WHERE id=?");
    s.bind(1, id);
    if (s.step()) return readShift(s);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Scene runs
// ---------------------------------------------------------------------------
static SceneRun readSceneRun(Stmt& s) {
    SceneRun r;
    r.id = s.columnInt(0);
    r.sceneId = s.columnInt(1);
    r.shiftId = s.columnInt(2);
    r.loadedAt = s.columnOptInt(3);
    r.startedAt = s.columnOptInt(4);
    r.endedAt = s.columnOptInt(5);
    r.widgetVersionsJson = s.columnText(6);
    return r;
}

std::optional<SceneRun> Store::newSceneRun(int64_t sceneId, int64_t shiftId,
                                           const std::string& versionsJson) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "INSERT INTO scene_run(scene_id,shift_id,loaded_at,widget_versions_json)"
        " VALUES(?,?,?,?)");
    s.bind(1, sceneId).bind(2, shiftId).bind(3, nowMs()).bind(4, versionsJson);
    s.step();
    int64_t id = db_.lastInsertRowId();
    auto g = db_.prepare(
        "SELECT id,scene_id,shift_id,loaded_at,started_at,ended_at,"
        "widget_versions_json FROM scene_run WHERE id=?");
    g.bind(1, id);
    if (g.step()) return readSceneRun(g);
    return std::nullopt;
}

std::optional<SceneRun> Store::sceneRun(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "SELECT id,scene_id,shift_id,loaded_at,started_at,ended_at,"
        "widget_versions_json FROM scene_run WHERE id=?");
    s.bind(1, id);
    if (s.step()) return readSceneRun(s);
    return std::nullopt;
}

bool Store::markSceneRunStarted(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "UPDATE scene_run SET started_at=COALESCE(started_at,?) WHERE id=?");
    s.bind(1, nowMs()).bind(2, id);
    s.step();
    return true;
}

bool Store::markSceneRunEnded(int64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare("UPDATE scene_run SET ended_at=? WHERE id=?");
    s.bind(1, nowMs()).bind(2, id);
    s.step();
    return true;
}

// ---------------------------------------------------------------------------
// Gift ledger (§7) — the source of truth for salary.
// ---------------------------------------------------------------------------
bool Store::appendLedger(const LedgerRow& row) {
    std::lock_guard<std::mutex> lk(mutex_);
    // Exactly-once: (msg_id, performer_id) UNIQUE. INSERT OR IGNORE and detect
    // whether a row was actually written (changes()). Manual rows have NULL
    // msg_id; the UNIQUE constraint treats NULLs as distinct, so manual
    // corrections always insert.
    auto s = db_.prepare(
        "INSERT OR IGNORE INTO gift_ledger(ts,account_id,shift_id,"
        "scene_run_id,msg_id,gifter_id,gifter_name,gift_id,gift_name,"
        "repeat_count,diamonds,performer_id,source) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, row.ts ? row.ts : nowMs())
        .bind(2, row.accountId)
        .bind(3, row.shiftId)
        .bind(4, row.sceneRunId)
        .bind(5, row.msgId)
        .bind(6, row.gifterId)
        .bind(7, row.gifterName)
        .bind(8, row.giftId)
        .bind(9, row.giftName)
        .bind(10, row.repeatCount)
        .bind(11, row.diamonds)
        .bind(12, row.performerId)
        .bind(13, row.source);
    s.step();
    // Determine if a new row was inserted.
    auto ch = db_.prepare("SELECT changes()");
    bool inserted = ch.step() && ch.columnInt(0) > 0;
    if (!inserted) return false;

    // Update caches.
    int64_t pid = row.performerId.value_or(0);
    if (row.performerId) {
        if (row.sceneRunId)
            runTotals_[{*row.sceneRunId, pid}] += row.diamonds;
        shiftPerfTotals_[{row.shiftId, pid}] += row.diamonds;
    }
    shiftCoins_[row.shiftId] += row.diamonds;
    return true;
}

int64_t Store::sceneRunTotal(int64_t sceneRunId, int64_t performerId) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = runTotals_.find({sceneRunId, performerId});
    return it == runTotals_.end() ? 0 : it->second;
}

int64_t Store::shiftTotal(int64_t shiftId, int64_t performerId) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = shiftPerfTotals_.find({shiftId, performerId});
    return it == shiftPerfTotals_.end() ? 0 : it->second;
}

int64_t Store::shiftCoins(int64_t shiftId) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = shiftCoins_.find(shiftId);
    return it == shiftCoins_.end() ? 0 : it->second;
}

std::map<int64_t, int64_t> Store::sceneRunTotals(int64_t sceneRunId) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::map<int64_t, int64_t> out;
    for (const auto& [k, v] : runTotals_)
        if (k.first == sceneRunId && v != 0) out[k.second] = v;
    return out;
}

std::map<int64_t, int64_t> Store::shiftTotals(int64_t shiftId) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::map<int64_t, int64_t> out;
    for (const auto& [k, v] : shiftPerfTotals_)
        if (k.first == shiftId && v != 0) out[k.second] = v;
    return out;
}

void Store::rebuildTotals() {
    // Caller holds mutex_. Rebuild all caches from the ledger (§10.4).
    runTotals_.clear();
    shiftPerfTotals_.clear();
    shiftCoins_.clear();
    {
        auto s = db_.prepare(
            "SELECT scene_run_id,performer_id,SUM(diamonds) FROM gift_ledger "
            "WHERE scene_run_id IS NOT NULL AND performer_id IS NOT NULL "
            "GROUP BY scene_run_id,performer_id");
        while (s.step())
            runTotals_[{s.columnInt(0), s.columnInt(1)}] = s.columnInt(2);
    }
    {
        auto s = db_.prepare(
            "SELECT shift_id,performer_id,SUM(diamonds) FROM gift_ledger "
            "WHERE performer_id IS NOT NULL GROUP BY shift_id,performer_id");
        while (s.step())
            shiftPerfTotals_[{s.columnInt(0), s.columnInt(1)}] =
                s.columnInt(2);
    }
    {
        auto s = db_.prepare(
            "SELECT shift_id,SUM(diamonds) FROM gift_ledger GROUP BY shift_id");
        while (s.step()) shiftCoins_[s.columnInt(0)] = s.columnInt(1);
    }
}

// ---------------------------------------------------------------------------
// Widget state snapshots
// ---------------------------------------------------------------------------
bool Store::saveWidgetState(int64_t sceneRunId, int64_t sceneWidgetId,
                            const std::string& stateJson, int64_t seq) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "INSERT INTO widget_state(scene_run_id,scene_widget_id,state_json,seq,"
        "updated_at) VALUES(?,?,?,?,?) ON CONFLICT(scene_run_id,"
        "scene_widget_id) DO UPDATE SET state_json=excluded.state_json,"
        "seq=excluded.seq,updated_at=excluded.updated_at");
    s.bind(1, sceneRunId)
        .bind(2, sceneWidgetId)
        .bind(3, stateJson)
        .bind(4, seq)
        .bind(5, nowMs());
    s.step();
    return true;
}

std::optional<std::string> Store::loadWidgetState(int64_t sceneRunId,
                                                  int64_t sceneWidgetId) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "SELECT state_json FROM widget_state WHERE scene_run_id=? AND "
        "scene_widget_id=?");
    s.bind(1, sceneRunId).bind(2, sceneWidgetId);
    if (s.step() && !s.columnIsNull(0)) return s.columnText(0);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Runtime singleton
// ---------------------------------------------------------------------------
RuntimeState Store::runtime() {
    std::lock_guard<std::mutex> lk(mutex_);
    RuntimeState r;
    auto s = db_.prepare(
        "SELECT active_account_id,active_shift_id,active_scene_run_id,running "
        "FROM runtime WHERE id=1");
    if (s.step()) {
        r.activeAccountId = s.columnOptInt(0);
        r.activeShiftId = s.columnOptInt(1);
        r.activeSceneRunId = s.columnOptInt(2);
        r.running = s.columnInt(3) != 0;
    }
    return r;
}

bool Store::setRuntime(const RuntimeState& r) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "UPDATE runtime SET active_account_id=?,active_shift_id=?,"
        "active_scene_run_id=?,running=? WHERE id=1");
    s.bind(1, r.activeAccountId)
        .bind(2, r.activeShiftId)
        .bind(3, r.activeSceneRunId)
        .bind(4, (int64_t)(r.running ? 1 : 0));
    s.step();
    return true;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
std::optional<std::string> Store::getSetting(const std::string& key) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare("SELECT value FROM setting WHERE key=?");
    s.bind(1, key);
    if (s.step()) return s.columnText(0);
    return std::nullopt;
}

bool Store::setSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "INSERT INTO setting(key,value) VALUES(?,?) ON CONFLICT(key) DO "
        "UPDATE SET value=excluded.value");
    s.bind(1, key).bind(2, value);
    s.step();
    return true;
}

// ---------------------------------------------------------------------------
// Widget bundles
// ---------------------------------------------------------------------------
std::vector<WidgetBundle> Store::widgetBundles() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<WidgetBundle> out;
    auto s = db_.prepare(
        "SELECT type,version,path,manifest_json,installed_at FROM widget_bundle "
        "ORDER BY type,version");
    while (s.step()) {
        WidgetBundle b;
        b.type = s.columnText(0);
        b.version = s.columnText(1);
        b.path = s.columnText(2);
        b.manifestJson = s.columnText(3);
        b.installedAt = s.columnInt(4);
        out.push_back(b);
    }
    return out;
}

bool Store::upsertWidgetBundle(const WidgetBundle& b) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto s = db_.prepare(
        "INSERT INTO widget_bundle(type,version,path,manifest_json,installed_at)"
        " VALUES(?,?,?,?,?) ON CONFLICT(type,version) DO UPDATE SET "
        "path=excluded.path,manifest_json=excluded.manifest_json,"
        "installed_at=excluded.installed_at");
    s.bind(1, b.type)
        .bind(2, b.version)
        .bind(3, b.path)
        .bind(4, b.manifestJson)
        .bind(5, b.installedAt ? b.installedAt : nowMs());
    s.step();
    return true;
}

std::optional<WidgetBundle> Store::latestBundle(const std::string& type) {
    std::lock_guard<std::mutex> lk(mutex_);
    // "Latest installed" (§12 note): most recently installed version wins.
    auto s = db_.prepare(
        "SELECT type,version,path,manifest_json,installed_at FROM widget_bundle "
        "WHERE type=? ORDER BY installed_at DESC LIMIT 1");
    s.bind(1, type);
    if (s.step()) {
        WidgetBundle b;
        b.type = s.columnText(0);
        b.version = s.columnText(1);
        b.path = s.columnText(2);
        b.manifestJson = s.columnText(3);
        b.installedAt = s.columnInt(4);
        return b;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Shift report (§13)
// ---------------------------------------------------------------------------
std::string Store::shiftReportJson(int64_t shiftId) {
    std::lock_guard<std::mutex> lk(mutex_);
    json out;
    // shift meta
    {
        auto s = db_.prepare(
            "SELECT id,started_at,ended_at FROM shift WHERE id=?");
        s.bind(1, shiftId);
        if (s.step()) {
            out["shift"] = {{"id", s.columnInt(0)},
                            {"started_at", s.columnInt(1)},
                            {"ended_at", s.columnIsNull(2)
                                             ? json(nullptr)
                                             : json(s.columnInt(2))}};
        } else {
            out["shift"] = {{"id", shiftId}};
        }
    }
    // totals per performer for the whole shift
    {
        json totals = json::array();
        auto s = db_.prepare(
            "SELECT p.name, SUM(g.diamonds) FROM gift_ledger g JOIN performer p "
            "ON p.id=g.performer_id WHERE g.shift_id=? AND g.performer_id IS "
            "NOT NULL GROUP BY g.performer_id ORDER BY SUM(g.diamonds) DESC");
        s.bind(1, shiftId);
        while (s.step())
            totals.push_back(
                {{"performer", s.columnText(0)}, {"diamonds", s.columnInt(1)}});
        out["totals"] = totals;
    }
    // per-scene(-run) breakdown
    {
        json scenes = json::array();
        auto runs = db_.prepare(
            "SELECT r.id, s.name, r.loaded_at FROM scene_run r JOIN scene s "
            "ON s.id=r.scene_id WHERE r.shift_id=? ORDER BY r.loaded_at");
        runs.bind(1, shiftId);
        while (runs.step()) {
            int64_t runId = runs.columnInt(0);
            json sc;
            sc["scene"] = runs.columnText(1);
            sc["run_id"] = runId;
            sc["loaded_at"] = runs.columnIsNull(2) ? json(nullptr)
                                                   : json(runs.columnInt(2));
            json pp = json::array();
            auto s = db_.prepare(
                "SELECT p.name, SUM(g.diamonds) FROM gift_ledger g JOIN "
                "performer p ON p.id=g.performer_id WHERE g.scene_run_id=? AND "
                "g.performer_id IS NOT NULL GROUP BY g.performer_id ORDER BY "
                "SUM(g.diamonds) DESC");
            s.bind(1, runId);
            while (s.step())
                pp.push_back({{"performer", s.columnText(0)},
                              {"diamonds", s.columnInt(1)}});
            sc["per_performer"] = pp;
            scenes.push_back(sc);
        }
        out["scenes"] = scenes;
    }
    return out.dump(2);
}

}  // namespace evo
