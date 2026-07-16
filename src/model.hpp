#pragma once

// EvoControl domain model (§12/§13). Plain structs mirroring the SQLite rows,
// used across the store, engine, and server.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace evo {

struct Account {
    int64_t id = 0;
    std::string name;
    std::string stream;   // @username (no '@')
    std::string cookies;  // optional per-account cookies
    int64_t createdAt = 0;
};

struct Performer {
    int64_t id = 0;
    int64_t accountId = 0;
    std::string name;
    std::string avatarPath;
    std::string tiktokUser;  // @username to fetch the profile picture from
    std::vector<float> faceEmbedding;  // recognition landmark (may be empty)
    float faceQuality = 0.0f;
    bool hasFace() const { return !faceEmbedding.empty(); }
};

struct Shift {
    int64_t id = 0;
    int64_t accountId = 0;
    int64_t startedAt = 0;
    std::optional<int64_t> endedAt;
};

struct Scene {
    int64_t id = 0;
    int64_t accountId = 0;
    std::string name;
    int64_t ordering = 0;
};

// A placed widget instance on the 1080x1920 canvas (§8.3).
struct SceneWidget {
    int64_t id = 0;
    int64_t sceneId = 0;
    std::string widgetType;
    double x = 0, y = 0, w = 0, h = 0;
    int64_t z = 0;
    std::string configJson;  // JSON object (timer, slots, gift map, text, ...)
};

struct SceneRun {
    int64_t id = 0;
    int64_t sceneId = 0;
    int64_t shiftId = 0;
    std::optional<int64_t> loadedAt;
    std::optional<int64_t> startedAt;
    std::optional<int64_t> endedAt;
    std::string widgetVersionsJson;
};

// One credited attribution written to the append-only ledger (§7).
struct LedgerRow {
    int64_t id = 0;
    int64_t ts = 0;
    int64_t accountId = 0;
    int64_t shiftId = 0;
    std::optional<int64_t> sceneRunId;
    std::optional<int64_t> msgId;   // TikTok id; null for manual corrections
    std::optional<int64_t> gifterId;
    std::string gifterName;
    std::optional<int64_t> giftId;
    std::string giftName;
    int64_t repeatCount = 0;
    int64_t diamonds = 0;
    std::optional<int64_t> performerId;  // null = show-level (unattributed)
    std::string source;  // 'widget:<id>' | 'manual'
};

// A widget bundle installed in the registry (§8.1).
struct WidgetBundle {
    std::string type;
    std::string version;
    std::string path;         // dir under widgets/
    std::string manifestJson;
    int64_t installedAt = 0;
};

struct RuntimeState {
    std::optional<int64_t> activeAccountId;
    std::optional<int64_t> activeShiftId;
    std::optional<int64_t> activeSceneRunId;
    bool running = false;
};

}  // namespace evo
