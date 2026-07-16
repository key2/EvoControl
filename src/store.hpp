#pragma once

// Store — the EvoControl persistence layer over Db (§10, §12, §13).
//
// It owns the SQLite database and provides:
//   * CRUD for accounts, performers, scenes, scene widgets;
//   * the append-only gift ledger (exactly-once by (msg_id, performer_id));
//   * shift lifecycle (New Shift never deletes history);
//   * scene runs (a "load scene" = new run with fresh per-run scores);
//   * in-memory totals caches (per scene run / per shift) rebuilt from the
//     ledger on boot — a crash can never lose or double-count salary;
//   * widget-state snapshots for crash resume;
//   * the singleton runtime row and the settings table;
//   * the shift report JSON aggregation (§13).
//
// Threading: Store is used from the engine thread and the server threads. All
// public methods lock an internal mutex, so callers may share one Store.

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "db.hpp"
#include "model.hpp"

namespace evo {

class Store {
public:
    Store() = default;

    /// Open (create) the database file. Rebuilds totals caches from the ledger.
    bool open(const std::string& path);
    void close();
    bool isOpen();
    std::string error();

    // --- accounts ------------------------------------------------------
    std::vector<Account> accounts();
    std::optional<Account> account(int64_t id);
    std::optional<Account> accountByName(const std::string& name);
    /// Create (or return existing) account. Returns the row.
    std::optional<Account> createAccount(const std::string& name,
                                         const std::string& stream,
                                         const std::string& cookies = {});
    bool updateAccount(const Account& a);
    bool deleteAccount(int64_t id);

    // --- performers ----------------------------------------------------
    std::vector<Performer> performers(int64_t accountId);
    std::optional<Performer> performer(int64_t id);
    std::optional<Performer> createPerformer(int64_t accountId,
                                             const std::string& name);
    bool renamePerformer(int64_t id, const std::string& name);
    bool setPerformerAvatar(int64_t id, const std::string& path);
    bool setPerformerTiktokUser(int64_t id, const std::string& user);
    bool setPerformerFace(int64_t id, const std::vector<float>& emb,
                          float quality);
    bool resetPerformerFace(int64_t id);
    bool deletePerformer(int64_t id);

    // --- scenes / composition -----------------------------------------
    std::vector<Scene> scenes(int64_t accountId);
    std::optional<Scene> scene(int64_t id);
    std::optional<Scene> createScene(int64_t accountId,
                                     const std::string& name);
    bool renameScene(int64_t id, const std::string& name);
    bool deleteScene(int64_t id);
    std::optional<Scene> duplicateScene(int64_t id, const std::string& newName);

    std::vector<SceneWidget> sceneWidgets(int64_t sceneId);
    std::optional<SceneWidget> sceneWidget(int64_t id);
    std::optional<SceneWidget> addSceneWidget(const SceneWidget& w);
    bool updateSceneWidget(const SceneWidget& w);
    bool deleteSceneWidget(int64_t id);
    /// Bulk replace all widgets of a scene ("Save scene"). Ids are reassigned.
    bool saveSceneWidgets(int64_t sceneId, std::vector<SceneWidget>& widgets);

    // --- shifts --------------------------------------------------------
    std::optional<Shift> currentShift(int64_t accountId);
    /// End the current shift (if any) and open a new one. Returns the new row.
    std::optional<Shift> newShift(int64_t accountId);
    std::optional<Shift> shift(int64_t id);

    // --- scene runs ----------------------------------------------------
    /// Start a new scene run for (scene, shift): resets per-run scores to 0.
    std::optional<SceneRun> newSceneRun(int64_t sceneId, int64_t shiftId,
                                        const std::string& versionsJson);
    std::optional<SceneRun> sceneRun(int64_t id);
    bool markSceneRunStarted(int64_t id);
    bool markSceneRunEnded(int64_t id);

    // --- gift ledger (§7) ----------------------------------------------
    // Append a credited attribution (exactly-once). Returns true if a new row
    // was written (false when the (msg_id, performer_id) pair already exists —
    // idempotent replay after a crash/reconnect). Updates the totals caches.
    bool appendLedger(const LedgerRow& row);

    /// Diamonds credited to a performer in a scene run (0 if none). Cached.
    int64_t sceneRunTotal(int64_t sceneRunId, int64_t performerId);
    /// Diamonds credited to a performer in a shift. Cached.
    int64_t shiftTotal(int64_t shiftId, int64_t performerId);
    /// Total coins of a shift, INCLUDING unattributed rows (§16.1). Cached.
    int64_t shiftCoins(int64_t shiftId);
    /// All performers' per-scene-run totals: performer_id -> diamonds.
    std::map<int64_t, int64_t> sceneRunTotals(int64_t sceneRunId);
    std::map<int64_t, int64_t> shiftTotals(int64_t shiftId);

    // --- widget state snapshots (crash resume) -------------------------
    bool saveWidgetState(int64_t sceneRunId, int64_t sceneWidgetId,
                         const std::string& stateJson, int64_t seq);
    std::optional<std::string> loadWidgetState(int64_t sceneRunId,
                                               int64_t sceneWidgetId);

    // --- runtime singleton ---------------------------------------------
    RuntimeState runtime();
    bool setRuntime(const RuntimeState& r);

    // --- settings ------------------------------------------------------
    std::optional<std::string> getSetting(const std::string& key);
    bool setSetting(const std::string& key, const std::string& value);

    // --- widget bundles ------------------------------------------------
    std::vector<WidgetBundle> widgetBundles();
    bool upsertWidgetBundle(const WidgetBundle& b);
    std::optional<WidgetBundle> latestBundle(const std::string& type);

    // --- reports (§13) -------------------------------------------------
    /// Aggregate the ledger into the shift report JSON.
    std::string shiftReportJson(int64_t shiftId);

    Db& db() { return db_; }

private:
    void rebuildTotals();  // caller holds mutex_
    static int64_t nowMs();

    std::mutex mutex_;
    Db db_;

    // In-memory totals caches (rebuilt on boot; kept hot on append).
    // key: (runId/shiftId, performerId)  ->  diamonds
    std::map<std::pair<int64_t, int64_t>, int64_t> runTotals_;
    std::map<std::pair<int64_t, int64_t>, int64_t> shiftPerfTotals_;
    std::map<int64_t, int64_t> shiftCoins_;  // shiftId -> all diamonds
};

}  // namespace evo
