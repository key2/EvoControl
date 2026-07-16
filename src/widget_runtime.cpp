#include "widget_runtime.hpp"

#include <chrono>
#include <cstring>
#include <string>

#include "quickjs.h"

namespace evo {

namespace {

// Interrupt budget: kill a callback that runs away. We count interrupt ticks
// and abort after a threshold reset before each dispatch.
struct Budget {
    int64_t ticks = 0;
    int64_t limit = 2'000'000;  // generous; ≤5 trivial widgets (§9)
};

int interruptHandler(JSRuntime*, void* opaque) {
    auto* b = static_cast<Budget*>(opaque);
    if (++b->ticks > b->limit) return 1;  // interrupt (throws)
    return 0;
}

std::string jsToStr(JSContext* ctx, JSValue v) {
    const char* s = JS_ToCString(ctx, v);
    std::string out = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    return out;
}

std::string exceptionText(JSContext* ctx) {
    JSValue ex = JS_GetException(ctx);
    std::string msg = jsToStr(ctx, ex);
    // Include stack if present.
    JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
    if (!JS_IsUndefined(stack)) {
        std::string st = jsToStr(ctx, stack);
        if (!st.empty()) msg += "\n" + st;
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, ex);
    return msg;
}

}  // namespace

struct WidgetRuntime::Impl {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    Budget budget;
    HostApi host;
    int64_t instanceId = 0;
    std::string published;  // last state published by app.js

    ~Impl() {
        if (ctx) JS_FreeContext(ctx);
        if (rt) JS_FreeRuntime(rt);
    }
};

// ---------------------------------------------------------------------------
// Host C functions exposed as globalThis.__host.<name>. Each reads its args
// (numbers / JSON strings) and calls into HostApi, returning JSON strings.
// The instance's Impl is stored as the context opaque.
// ---------------------------------------------------------------------------
static WidgetRuntime::Impl* self(JSContext* ctx) {
    return static_cast<WidgetRuntime::Impl*>(JS_GetContextOpaque(ctx));
}

static JSValue hostPerformers(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = self(ctx);
    std::string j = (s && s->host.performers) ? s->host.performers() : "[]";
    return JS_ParseJSON(ctx, j.c_str(), j.size(), "<performers>");
}

static JSValue hostGiftGallery(JSContext* ctx, JSValueConst, int,
                               JSValueConst*) {
    auto* s = self(ctx);
    std::string j = (s && s->host.giftGallery) ? s->host.giftGallery() : "[]";
    return JS_ParseJSON(ctx, j.c_str(), j.size(), "<giftGallery>");
}

static JSValue hostUserAvatar(JSContext* ctx, JSValueConst, int argc,
                              JSValueConst* argv) {
    auto* s = self(ctx);
    int64_t id = 0;
    if (argc > 0) JS_ToInt64(ctx, &id, argv[0]);
    std::string url = (s && s->host.userAvatar) ? s->host.userAvatar(id) : "";
    return JS_NewString(ctx, url.c_str());
}

static JSValue hostFaces(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = self(ctx);
    std::string j = (s && s->host.faces) ? s->host.faces() : "[]";
    return JS_ParseJSON(ctx, j.c_str(), j.size(), "<faces>");
}

static JSValue hostPoints(JSContext* ctx, JSValueConst, int argc,
                          JSValueConst* argv) {
    auto* s = self(ctx);
    std::string scope = argc > 0 ? jsToStr(ctx, argv[0]) : "scene_run";
    std::string j = (s && s->host.points) ? s->host.points(scope) : "{}";
    return JS_ParseJSON(ctx, j.c_str(), j.size(), "<points>");
}

static JSValue hostCreditGift(JSContext* ctx, JSValueConst, int argc,
                              JSValueConst* argv) {
    auto* s = self(ctx);
    if (s && s->host.creditGift && argc >= 2) {
        int64_t pid = 0;
        JS_ToInt64(ctx, &pid, argv[0]);
        s->host.creditGift(pid, jsToStr(ctx, argv[1]));
    }
    return JS_UNDEFINED;
}

static JSValue hostAdjustPoints(JSContext* ctx, JSValueConst, int argc,
                                JSValueConst* argv) {
    auto* s = self(ctx);
    if (s && s->host.adjustPoints && argc >= 3) {
        int64_t pid = 0, d = 0;
        JS_ToInt64(ctx, &pid, argv[0]);
        JS_ToInt64(ctx, &d, argv[1]);
        s->host.adjustPoints(pid, d, jsToStr(ctx, argv[2]));
    }
    return JS_UNDEFINED;
}

static JSValue hostEmit(JSContext* ctx, JSValueConst, int argc,
                        JSValueConst* argv) {
    auto* s = self(ctx);
    if (s && s->host.emit && argc >= 2)
        s->host.emit(jsToStr(ctx, argv[0]), jsToStr(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue hostSubscribe(JSContext* ctx, JSValueConst, int argc,
                             JSValueConst* argv) {
    auto* s = self(ctx);
    if (s && s->host.subscribe && argc >= 1)
        s->host.subscribe(jsToStr(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue hostPersist(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* s = self(ctx);
    if (s && s->host.persist) s->host.persist();
    return JS_UNDEFINED;
}

// setState(json): app.js publishes its confirmed state to be broadcast.
static JSValue hostSetState(JSContext* ctx, JSValueConst, int argc,
                            JSValueConst* argv) {
    auto* s = self(ctx);
    if (s && argc >= 1) s->published = jsToStr(ctx, argv[0]);
    return JS_UNDEFINED;
}

// nowMs(): monotonic-ish clock for timers.
static JSValue hostNow(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    double ms = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
    return JS_NewFloat64(ctx, ms);
}

// JS bootstrap: builds `ctx` from `__host`, defines a widget registration
// hook, timer helpers, and dispatch functions the C++ side calls.
static const char* kBootstrap = R"JS(
(function(){
  const H = globalThis.__host;
  const timer = {
    _remainingMs: 0, _running: false,
    start(sec){ this._remainingMs = (sec||0)*1000; this._running = true; },
    stop(){ this._running = false; },
    reset(sec){ this._remainingMs = (sec||0)*1000; },
    get remaining(){ return Math.max(0, this._remainingMs/1000); },
    get running(){ return this._running; },
    _tick(dt){ if(this._running){ this._remainingMs -= dt;
      if(this._remainingMs<=0){ this._remainingMs=0; this._running=false;
        if(this.onExpire) this.onExpire(); } } }
  };
  const ctx = {
    config: {},
    performers(){ return H.performers(); },
    giftGallery(){ return H.giftGallery(); },
    userAvatar(id){ return H.userAvatar(id); },
    faces(){ return H.faces(); },
    points(scope){ return H.points(scope||"scene_run"); },
    creditGift(pid, gift){ H.creditGift(pid, JSON.stringify(gift||{})); },
    adjustPoints(pid, d, reason){ H.adjustPoints(pid, d, reason||""); },
    emit(view, data){ H.emit(view, JSON.stringify(data||{})); },
    subscribe(topic){ H.subscribe(topic); },
    setState(s){ H.setState(JSON.stringify(s||{})); },
    persist(){ H.persist(); },
    timer,
    now(){ return H.now(); }
  };
  globalThis.__ctx = ctx;
  // Widgets call registerWidget({onLoad,onStart,...}) OR define a default
  // export-like object; we support a simple global `widget`.
  globalThis.registerWidget = function(w){ globalThis.__widget = w || {}; };
  globalThis.__dispatch = function(name, arg1, arg2){
    const w = globalThis.__widget; if(!w) return undefined;
    switch(name){
      case 'onLoad':   if(w.onLoad)   w.onLoad(ctx, arg1);        break;
      case 'onStart':  if(w.onStart)  w.onStart(ctx);             break;
      case 'onStop':   if(w.onStop)   w.onStop(ctx);              break;
      case 'onTick':   timer._tick(arg1); if(w.onTick) w.onTick(ctx, arg1); break;
      case 'onGift':   if(w.onGift)   w.onGift(ctx, arg1);        break;
      case 'onIntent': if(w.onIntent) w.onIntent(ctx, arg1);      break;
      case 'onShared': if(w.onShared) w.onShared(ctx, arg1, arg2);break;
      case 'saveState':return w.saveState ? w.saveState(ctx) : {};
    }
    return undefined;
  };
})();
)JS";

WidgetRuntime::WidgetRuntime() : impl_(std::make_unique<Impl>()) {}
WidgetRuntime::~WidgetRuntime() = default;

bool WidgetRuntime::load(int64_t instanceId, const std::string& appJsSource,
                         const std::string& configJson,
                         const std::string& savedStateJson,
                         const HostApi& host) {
    instanceId_ = instanceId;
    impl_->instanceId = instanceId;
    impl_->host = host;

    impl_->rt = JS_NewRuntime();
    if (!impl_->rt) {
        error_ = "JS_NewRuntime failed";
        return false;
    }
    // Per-context memory cap (§9). 16 MB is ample for tiny widgets.
    JS_SetMemoryLimit(impl_->rt, 16 * 1024 * 1024);
    JS_SetInterruptHandler(impl_->rt, interruptHandler, &impl_->budget);

    impl_->ctx = JS_NewContext(impl_->rt);
    if (!impl_->ctx) {
        error_ = "JS_NewContext failed";
        return false;
    }
    JS_SetContextOpaque(impl_->ctx, impl_.get());
    JSContext* c = impl_->ctx;

    // Build globalThis.__host with the C bridge functions. No fetch, no file,
    // no require, no module loader are ever installed → the sandbox has only
    // this surface.
    JSValue global = JS_GetGlobalObject(c);
    JSValue h = JS_NewObject(c);
    auto fn = [&](const char* name, JSCFunction* f, int argc) {
        JS_SetPropertyStr(c, h, name, JS_NewCFunction(c, f, name, argc));
    };
    fn("performers", hostPerformers, 0);
    fn("giftGallery", hostGiftGallery, 0);
    fn("userAvatar", hostUserAvatar, 1);
    fn("faces", hostFaces, 0);
    fn("points", hostPoints, 1);
    fn("creditGift", hostCreditGift, 2);
    fn("adjustPoints", hostAdjustPoints, 3);
    fn("emit", hostEmit, 2);
    fn("subscribe", hostSubscribe, 1);
    fn("setState", hostSetState, 1);
    fn("persist", hostPersist, 0);
    fn("now", hostNow, 0);
    JS_SetPropertyStr(c, global, "__host", h);
    JS_FreeValue(c, global);

    // Run the bootstrap.
    {
        JSValue r = JS_Eval(c, kBootstrap, std::strlen(kBootstrap),
                            "<bootstrap>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            error_ = "bootstrap: " + exceptionText(c);
            JS_FreeValue(c, r);
            return false;
        }
        JS_FreeValue(c, r);
    }

    // Run the widget's app.js (which should call registerWidget(...)).
    impl_->budget.ticks = 0;
    {
        JSValue r = JS_Eval(c, appJsSource.c_str(), appJsSource.size(),
                            "app.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            error_ = "app.js: " + exceptionText(c);
            JS_FreeValue(c, r);
            return false;
        }
        JS_FreeValue(c, r);
    }

    // Set ctx.config = configJson.
    {
        std::string cfg = configJson.empty() ? "{}" : configJson;
        std::string js = "__ctx.config = " + cfg + ";";
        JSValue r = JS_Eval(c, js.c_str(), js.size(), "<config>",
                            JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            error_ = "config: " + exceptionText(c);
            JS_FreeValue(c, r);
            return false;
        }
        JS_FreeValue(c, r);
    }

    // Dispatch onLoad(ctx, savedState).
    {
        std::string saved =
            savedStateJson.empty() ? "null" : savedStateJson;
        std::string js = "__dispatch('onLoad', " + saved + ");";
        impl_->budget.ticks = 0;
        JSValue r = JS_Eval(c, js.c_str(), js.size(), "<onLoad>",
                            JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            error_ = "onLoad: " + exceptionText(c);
            JS_FreeValue(c, r);
            return false;
        }
        JS_FreeValue(c, r);
    }

    loaded_ = true;
    return true;
}

// Helper: dispatch a lifecycle callback whose args are already JS-literal
// strings ("null", a number, or a JSON literal).
static void dispatch(WidgetRuntime::Impl* impl, const std::string& call) {
    if (!impl || !impl->ctx) return;
    impl->budget.ticks = 0;
    JSValue r = JS_Eval(impl->ctx, call.c_str(), call.size(), "<dispatch>",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        // Swallow but record; a failing callback must not crash the engine.
        JSValue ex = JS_GetException(impl->ctx);
        JS_FreeValue(impl->ctx, ex);
    }
    JS_FreeValue(impl->ctx, r);
}

void WidgetRuntime::onStart() {
    dispatch(impl_.get(), "__dispatch('onStart');");
}
void WidgetRuntime::onStop() {
    dispatch(impl_.get(), "__dispatch('onStop');");
}
void WidgetRuntime::onTick(double dtMs) {
    dispatch(impl_.get(),
             "__dispatch('onTick'," + std::to_string(dtMs) + ");");
}
void WidgetRuntime::onGift(const std::string& giftJson) {
    dispatch(impl_.get(), "__dispatch('onGift'," +
                              (giftJson.empty() ? "null" : giftJson) + ");");
}
void WidgetRuntime::onIntent(const std::string& intentJson) {
    dispatch(impl_.get(), "__dispatch('onIntent'," +
                              (intentJson.empty() ? "null" : intentJson) +
                              ");");
}
void WidgetRuntime::onShared(const std::string& topic,
                             const std::string& dataJson) {
    // topic as a JSON string literal.
    std::string t = "\"";
    for (char ch : topic) {
        if (ch == '"' || ch == '\\') t += '\\';
        t += ch;
    }
    t += "\"";
    dispatch(impl_.get(), "__dispatch('onShared'," + t + "," +
                              (dataJson.empty() ? "null" : dataJson) + ");");
}

std::string WidgetRuntime::saveState() {
    if (!impl_->ctx) return "{}";
    impl_->budget.ticks = 0;
    JSValue r =
        JS_Eval(impl_->ctx, "JSON.stringify(__dispatch('saveState')||{})",
                std::strlen("JSON.stringify(__dispatch('saveState')||{})"),
                "<saveState>", JS_EVAL_TYPE_GLOBAL);
    std::string out = "{}";
    if (!JS_IsException(r)) {
        out = jsToStr(impl_->ctx, r);
        if (out.empty() || out == "undefined") out = "{}";
    } else {
        JSValue ex = JS_GetException(impl_->ctx);
        JS_FreeValue(impl_->ctx, ex);
    }
    JS_FreeValue(impl_->ctx, r);
    return out;
}

std::string WidgetRuntime::takePublishedState() {
    std::string s = impl_->published;
    impl_->published.clear();
    return s;
}

}  // namespace evo
