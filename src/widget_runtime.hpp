#pragma once

// WidgetRuntime — the authoritative game logic sandbox (§5/§9).
//
// Each placed widget instance has exactly one authoritative `app.js` running in
// its own QuickJS runtime+context (one per instance → isolation; per-context
// memory cap + per-callback interrupt budget so a runaway widget is killed
// without harming the engine). `app.js` has NO network/file/require access —
// only the injected host capability set (§4/§9):
//   ctx.config, ctx.performers(), ctx.giftGallery(), ctx.userAvatar(id),
//   ctx.faces(), ctx.creditGift(pid, gift), ctx.adjustPoints(pid, d, reason),
//   ctx.points(scope), ctx.emit(view, data), ctx.subscribe(topic),
//   ctx.timer.*, ctx.persist().
//
// The C++ side implements HostApi (typically the GameEngine): the runtime calls
// back into it for the capability functions and for the effects a widget
// requests (credit gift, adjust points, emit to views, subscribe, persist).
//
// All JS interchange is JSON strings, keeping the C binding surface tiny and
// the sandbox easy to reason about.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct JSRuntime;
struct JSContext;

namespace evo {

// The capabilities + effects the host (GameEngine) exposes to one instance.
// All payloads are JSON strings. `instanceId` identifies the calling widget.
struct HostApi {
    // Capabilities (return JSON):
    std::function<std::string()> performers;    // [{id,name,avatarUrl}]
    std::function<std::string()> giftGallery;   // [{giftId,name,diamonds,iconUrl}]
    std::function<std::string(int64_t)> userAvatar;  // "url"
    std::function<std::string()> faces;         // [{performerId,name,x,y,w,h,similarity}]
    std::function<std::string(const std::string&)> points;  // scope -> {pid:diamonds}

    // Effects (JSON in):
    // creditGift(performerId, giftJson) -> ledger (salary).
    std::function<void(int64_t, const std::string&)> creditGift;
    // adjustPoints(performerId, diamonds, reason) -> manual ledger row.
    std::function<void(int64_t, int64_t, const std::string&)> adjustPoints;
    // emit(view, dataJson): view = "obs"|"control"|"all" (this instance only).
    std::function<void(const std::string&, const std::string&)> emit;
    // subscribe(topic): shared plane ("points", ...).
    std::function<void(const std::string&)> subscribe;
    // persist(): request an immediate saveState snapshot.
    std::function<void()> persist;
};

class WidgetRuntime {
public:
    WidgetRuntime();
    ~WidgetRuntime();

    WidgetRuntime(const WidgetRuntime&) = delete;
    WidgetRuntime& operator=(const WidgetRuntime&) = delete;

    /// Create the instance's context, inject the host bridge + ctx, and run the
    /// widget's app.js source. `configJson` is the instance config;
    /// `savedStateJson` is null (empty) on a fresh scene load or the crash-
    /// resume snapshot. Returns false on a JS load error (message in error()).
    bool load(int64_t instanceId, const std::string& appJsSource,
              const std::string& configJson, const std::string& savedStateJson,
              const HostApi& host);

    // Lifecycle dispatch (all no-ops if the callback isn't defined by app.js).
    void onStart();
    void onStop();
    void onTick(double dtMs);
    void onGift(const std::string& giftJson);      // streak-deduped gift
    void onIntent(const std::string& intentJson);  // private control.js message
    void onShared(const std::string& topic, const std::string& dataJson);

    /// Ask app.js for its JSON state snapshot (crash recovery). "{}" if none.
    std::string saveState();

    /// The current published widget state to broadcast to views (set by app.js
    /// via ctx.setState / returned by callbacks). Empty if unchanged.
    std::string takePublishedState();

    bool loaded() const { return loaded_; }
    std::string error() const { return error_; }
    int64_t instanceId() const { return instanceId_; }

    // Exposed so the QuickJS C bridge functions (free functions in the .cpp)
    // can reach the instance via the context opaque pointer.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    bool loaded_ = false;
    std::string error_;
    int64_t instanceId_ = 0;
};

}  // namespace evo
