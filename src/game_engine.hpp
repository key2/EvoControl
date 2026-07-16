#pragma once

// GameEngine — the authoritative single-threaded game core (§5/§6/§7).
//
// It owns one WidgetRuntime (app.js) per placed widget instance of the current
// scene run, routes deduped gift/tick/intent/shared events to them, credits the
// gift ledger (salary source of truth), maintains the shared plane
// (authoritative diamond totals + roster/gallery/avatars), and publishes each
// instance's confirmed state to the views via a broadcast callback.
//
// Everything runs on a single engine thread pumped by run() from a dedicated
// std::thread; external callers post work through thread-safe post* methods
// which enqueue onto the engine queue. This keeps all QuickJS contexts and
// game state single-threaded (§9 isolation).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "model.hpp"

namespace ttlive {
struct Event;
struct GiftInfo;
}

namespace evo {

class Store;
class WidgetRegistry;
class WidgetRuntime;

// A face reported by the tracker, mapped to a performer (§9/§15).
struct EngineFace {
    int64_t performerId = 0;
    std::string name;
    double x = 0, y = 0, w = 0, h = 0;  // normalized [0..1]
    double similarity = 0;
};

// A gift monitor row emitted for the native table (§16.2).
struct GiftMonitorRow {
    int64_t ts = 0;
    int64_t gifterId = 0;
    std::string gifterName;
    int64_t giftId = 0;
    std::string giftName;
    std::string giftIconUrl;  // from the event (fallback icon source)
    int64_t amount = 0;    // streak/repeat count
    int64_t diamonds = 0;  // credited value
    int64_t performerId = 0;  // 0 = show-level (unattributed)
    std::string performerName;
};

class GameEngine {
public:
    GameEngine(Store& store, WidgetRegistry& registry);
    ~GameEngine();

    GameEngine(const GameEngine&) = delete;
    GameEngine& operator=(const GameEngine&) = delete;

    // Broadcast a WS envelope to views. Set before start(). Called on the
    // engine thread. Args: scope filter ("all"|"obs"|"control"), instanceId
    // (0 = shared), topic, dataJson.
    using Broadcaster =
        std::function<void(const std::string& scope, int64_t instanceId,
                           const std::string& topic, const std::string& dataJson)>;
    void setBroadcaster(Broadcaster b) { broadcast_ = std::move(b); }

    void start();  // launch engine thread
    void stop();

    // --- account / shift / scene lifecycle (posted from server/native) -----
    // Load an account: set active account + shift, load roster/gallery into the
    // shared plane. Tears down any active run.
    void postLoadAccount(int64_t accountId);
    void postNewShift();
    // Load a scene → new scene run (fresh scores, onLoad(ctx,null)); instructs
    // views to navigate. `resume` true = crash resume of an existing run.
    void postLoadScene(int64_t sceneId, bool resume, int64_t resumeRunId = 0);
    void postStart();  // native Start → begins a round (onStart)
    void postStop();

    // --- events -------------------------------------------------------------
    // Feed a raw ttlive event (from the client thread). The engine dedups
    // streaks by msg_id and routes finalized gifts to widgets + ledger.
    void postEvent(const ttlive::Event& e);
    // Update the gift gallery (diamond prices) for the shared plane.
    void postGiftGallery(const std::vector<ttlive::GiftInfo>& gifts);
    // Update face positions (from the FaceTracker, main thread).
    void postFaces(const std::vector<EngineFace>& faces);

    // --- intents (from control.js over WS) ---------------------------------
    void postIntent(int64_t instanceId, const std::string& intentJson);

    // Manual operator correction → ledger row (source:"manual"), §7.
    void postAdjustPoints(int64_t performerId, int64_t diamonds,
                          const std::string& reason);

    // --- snapshot for late joiners (§6.2) ----------------------------------
    // Returns the full snapshot JSON of every instance's current state.
    std::string snapshotJson();

    // Public shared-plane accessors for the REST layer (§17, gift gallery).
    std::string giftGalleryJsonArray();  // [{giftId,name,diamonds,iconUrl}]
    std::string facesJsonArray();        // [{performerId,name,x,y,w,h,similarity}]

    // --- native views ------------------------------------------------------
    // Recent gift monitor rows (newest first), for the native table (§16.2).
    std::vector<GiftMonitorRow> giftMonitor();
    // Total coins of the current shift (§16.1).
    int64_t shiftCoins();

    int64_t activeSceneRunId();
    bool running();

private:
    struct Task { std::function<void()> fn; };
    void enqueue(std::function<void()> fn);
    void threadMain();

    // Engine-thread implementations.
    void loadAccountImpl(int64_t accountId);
    void newShiftImpl();
    void loadSceneImpl(int64_t sceneId, bool resume, int64_t resumeRunId);
    void startImpl();
    void stopImpl();
    void eventImpl(std::shared_ptr<ttlive::Event> e);
    void facesImpl(std::vector<EngineFace> faces);
    void intentImpl(int64_t instanceId, std::string intentJson);
    void tick();

    // Shared plane helpers.
    std::string performersJson();
    std::string giftGalleryJson();
    std::string pointsJson(const std::string& scope);
    std::string userAvatarUrl(int64_t userId);
    std::string facesJson();
    void publishInstance(int64_t instanceId);  // broadcast state
    void notifyPointsChanged();                 // shared plane "points"
    void snapshotAll();                         // save widget_state
    void teardownRun();

    Store& store_;
    WidgetRegistry& registry_;
    Broadcaster broadcast_;

    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::mutex qmutex_;
    std::condition_variable qcv_;
    std::deque<Task> queue_;

    // Active runtime state (engine thread only unless noted).
    int64_t accountId_ = 0;
    int64_t shiftId_ = 0;
    int64_t sceneId_ = 0;
    int64_t sceneRunId_ = 0;
    std::atomic<bool> roundRunning_{false};

    // Placed instances of the current run: sceneWidgetId -> runtime.
    std::map<int64_t, std::unique_ptr<WidgetRuntime>> instances_;
    std::map<int64_t, SceneWidget> instanceMeta_;
    std::map<int64_t, int64_t> instanceSeq_;  // per-instance monotonic seq
    std::map<int64_t, std::string> lastState_;  // last published state json
    std::map<int64_t, std::set<std::string>> subscriptions_;  // topics

    // Shared plane data (guarded by shared_ for cross-thread reads).
    std::mutex shared_;
    std::vector<Performer> performers_;
    std::map<int64_t, int64_t> giftPrices_;         // giftId -> diamonds
    std::map<int64_t, std::string> giftIcons_;      // giftId -> url
    std::map<int64_t, std::string> giftNames_;      // giftId -> name
    std::map<int64_t, std::string> userAvatars_;    // userId -> url
    std::vector<EngineFace> faces_;

    // Streak dedup: last seen repeat_count per (msg_id) to count a combo once.
    std::set<int64_t> finalizedMsgs_;

    // Gift monitor ring (newest first).
    std::mutex monMutex_;
    std::deque<GiftMonitorRow> monitor_;
};

}  // namespace evo
