#include "rest_api.hpp"

#include "game_engine.hpp"
#include <nlohmann/json.hpp>
#include "store.hpp"
#include "widget_registry.hpp"



namespace evo {

ApiResponse ApiResponse::err(int status, const std::string& msg) {
    nlohmann::json j = {{"error", msg}};
    return {status, "application/json", j.dump()};
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static nlohmann::json parseBody(const std::string& body) {
    if (body.empty()) return nlohmann::json::object();
    nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    return j.is_discarded() ? nlohmann::json::object() : j;
}

static int64_t toId(const std::string& s) {
    try {
        return std::stoll(s);
    } catch (...) {
        return 0;
    }
}

RestApi::RestApi(Store& store, WidgetRegistry& registry, GameEngine& engine)
    : store_(store), registry_(registry), engine_(engine) {}

ApiResponse RestApi::handle(const ApiRequest& req) {
    auto seg = splitPath(req.path);
    if (seg.empty() || seg[0] != "api")
        return ApiResponse::err(404, "not found");
    // Mutations + control data are password-gated when set (§18). The server
    // sets req.authed; GET on OBS-visible resources is always allowed. We keep
    // it simple: all /api/* require auth when a password is configured, except
    // the avatar proxies which OBS widgets need.
    if (!req.authed) {
        bool avatar = seg.size() >= 4 &&
                      (seg[1] == "users" || seg[1] == "performers") &&
                      seg.back() == "avatar";
        bool galleryOrReg = seg.size() == 2 && (seg[1] == "gift-gallery" ||
                                                seg[1] == "widget-registry");
        if (!(avatar || galleryOrReg))
            return ApiResponse::err(401, "unauthorized");
    }

    const std::string& r = seg[1];
    if (r == "accounts") return routeAccounts(req, seg);
    if (r == "performers" || r == "users") return routePerformers(req, seg);
    if (r == "scenes") return routeScenes(req, seg);
    if (r == "widgets" || r == "widget-registry") return routeWidgets(req, seg);
    if (r == "shifts") return routeShifts(req, seg);
    if (r == "gift-gallery" || r == "faces") return routeServices(req, seg);
    return ApiResponse::err(404, "unknown route");
}

// ---------------------------------------------------------------------------
// Accounts
// ---------------------------------------------------------------------------
static nlohmann::json accountJson(const Account& a) {
    return {{"id", a.id},
            {"name", a.name},
            {"stream", a.stream},
            {"created_at", a.createdAt}};
}

ApiResponse RestApi::routeAccounts(const ApiRequest& req,
                                   const std::vector<std::string>& seg) {
    // /api/accounts
    if (seg.size() == 2) {
        if (req.method == "GET") {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& a : store_.accounts()) arr.push_back(accountJson(a));
            return ApiResponse::json(arr.dump());
        }
        if (req.method == "POST") {
            nlohmann::json b = parseBody(req.body);
            auto a = store_.createAccount(b.value("name", ""),
                                          b.value("stream", ""),
                                          b.value("cookies", ""));
            if (!a) return ApiResponse::err(400, "create failed");
            return ApiResponse::json(accountJson(*a).dump(), 201);
        }
    }
    // /api/accounts/:id/...
    if (seg.size() >= 3) {
        int64_t id = toId(seg[2]);
        // /api/accounts/:id/load
        if (seg.size() == 4 && seg[3] == "load" && req.method == "POST") {
            engine_.postLoadAccount(id);
            if (onAccountLoaded) onAccountLoaded(id);
            return ApiResponse::json("{\"ok\":true}");
        }
        // /api/accounts/:id/performers
        if (seg.size() == 4 && seg[3] == "performers") {
            if (req.method == "GET") {
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& p : store_.performers(id))
                    arr.push_back({{"id", p.id},
                                   {"name", p.name},
                                   {"hasFace", p.hasFace()}});
                return ApiResponse::json(arr.dump());
            }
            if (req.method == "POST") {
                nlohmann::json b = parseBody(req.body);
                auto p = store_.createPerformer(id, b.value("name", ""));
                if (!p) return ApiResponse::err(400, "create failed");
                return ApiResponse::json(nlohmann::json({{"id", p->id},
                                               {"name", p->name}})
                                            .dump(),
                                         201);
            }
        }
        // /api/accounts/:id/scenes
        if (seg.size() == 4 && seg[3] == "scenes") {
            if (req.method == "GET") {
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& s : store_.scenes(id))
                    arr.push_back({{"id", s.id}, {"name", s.name}});
                return ApiResponse::json(arr.dump());
            }
            if (req.method == "POST") {
                nlohmann::json b = parseBody(req.body);
                auto s = store_.createScene(id, b.value("name", ""));
                if (!s) return ApiResponse::err(400, "create failed");
                return ApiResponse::json(
                    nlohmann::json({{"id", s->id}, {"name", s->name}}).dump(), 201);
            }
        }
        if (seg.size() == 3) {
            if (req.method == "GET") {
                auto a = store_.account(id);
                if (!a) return ApiResponse::err(404, "no such account");
                return ApiResponse::json(accountJson(*a).dump());
            }
            if (req.method == "DELETE") {
                store_.deleteAccount(id);
                return ApiResponse::json("{\"ok\":true}");
            }
        }
    }
    return ApiResponse::err(404, "not found");
}

// ---------------------------------------------------------------------------
// Performers / users
// ---------------------------------------------------------------------------
ApiResponse RestApi::routePerformers(const ApiRequest& req,
                                     const std::vector<std::string>& seg) {
    // /api/users/:id/avatar
    if (seg[1] == "users" && seg.size() == 4 && seg[3] == "avatar" &&
        req.method == "GET") {
        int64_t uid = toId(seg[2]);
        std::string path = resolveUserAvatar ? resolveUserAvatar(uid) : "";
        if (path.empty()) return ApiResponse::err(404, "no avatar");
        return {200, "@file", path};  // server streams the file
    }
    // /api/performers/:id[/...]
    if (seg[1] == "performers" && seg.size() >= 3) {
        int64_t id = toId(seg[2]);
        if (seg.size() == 4 && seg[3] == "avatar") {
            if (req.method == "GET") {
                std::string path =
                    resolvePerformerAvatar ? resolvePerformerAvatar(id) : "";
                if (path.empty()) {
                    auto p = store_.performer(id);
                    if (p && !p->avatarPath.empty()) path = p->avatarPath;
                }
                if (path.empty()) return ApiResponse::err(404, "no avatar");
                return {200, "@file", path};
            }
            if (req.method == "POST") {
                nlohmann::json b = parseBody(req.body);
                if (b.contains("url"))
                    store_.setPerformerAvatar(id, b["url"].get<std::string>());
                return ApiResponse::json("{\"ok\":true}");
            }
        }
        if (seg.size() == 5 && seg[3] == "face" && seg[4] == "reset" &&
            req.method == "POST") {
            store_.resetPerformerFace(id);
            return ApiResponse::json("{\"ok\":true}");
        }
        if (seg.size() == 5 && seg[3] == "points" && seg[4] == "adjust" &&
            req.method == "POST") {
            nlohmann::json b = parseBody(req.body);
            engine_.postAdjustPoints(id, b.value("diamonds", (int64_t)0),
                                     b.value("reason", std::string("manual")));
            return ApiResponse::json("{\"ok\":true}");
        }
        if (seg.size() == 3) {
            if (req.method == "PUT") {
                nlohmann::json b = parseBody(req.body);
                if (b.contains("name"))
                    store_.renamePerformer(id, b["name"].get<std::string>());
                return ApiResponse::json("{\"ok\":true}");
            }
            if (req.method == "DELETE") {
                store_.deletePerformer(id);
                return ApiResponse::json("{\"ok\":true}");
            }
        }
    }
    return ApiResponse::err(404, "not found");
}

// ---------------------------------------------------------------------------
// Scenes / composition
// ---------------------------------------------------------------------------
static nlohmann::json widgetJson(const SceneWidget& w) {
    nlohmann::json cfg = nlohmann::json::parse(w.configJson.empty() ? "{}" : w.configJson, nullptr,
                           false);
    if (cfg.is_discarded()) cfg = nlohmann::json::object();
    return {{"id", w.id},      {"type", w.widgetType}, {"x", w.x},
            {"y", w.y},        {"w", w.w},             {"h", w.h},
            {"z", w.z},        {"config", cfg}};
}

ApiResponse RestApi::routeScenes(const ApiRequest& req,
                                 const std::vector<std::string>& seg) {
    // /api/scenes/current  → {sceneId, runId, running}
    if (seg.size() == 3 && seg[2] == "current" && req.method == "GET") {
        int64_t runId = engine_.activeSceneRunId();
        auto run = store_.sceneRun(runId);
        nlohmann::json j;
        j["runId"] = runId;
        j["sceneId"] = run ? run->sceneId : 0;
        j["running"] = engine_.running();
        return ApiResponse::json(j.dump());
    }
    // /api/scenes/current/widgets  (the active scene run's composition)
    if (seg.size() == 4 && seg[2] == "current" && seg[3] == "widgets" &&
        req.method == "GET") {
        int64_t runId = engine_.activeSceneRunId();
        auto run = store_.sceneRun(runId);
        nlohmann::json arr = nlohmann::json::array();
        if (run)
            for (const auto& w : store_.sceneWidgets(run->sceneId))
                arr.push_back(widgetJson(w));
        return ApiResponse::json(arr.dump());
    }
    if (seg.size() >= 3) {
        int64_t id = toId(seg[2]);
        // /api/scenes/:id/widgets
        if (seg.size() == 4 && seg[3] == "widgets") {
            if (req.method == "GET") {
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& w : store_.sceneWidgets(id))
                    arr.push_back(widgetJson(w));
                return ApiResponse::json(arr.dump());
            }
            if (req.method == "PUT") {
                // bulk Save scene
                nlohmann::json b = parseBody(req.body);
                std::vector<SceneWidget> ws;
                const nlohmann::json& list = b.is_array() ? b : b.value("widgets", nlohmann::json::array());
                for (const auto& e : list) {
                    SceneWidget w;
                    w.sceneId = id;
                    w.widgetType = e.value("type", "");
                    w.x = e.value("x", 0.0);
                    w.y = e.value("y", 0.0);
                    w.w = e.value("w", 0.0);
                    w.h = e.value("h", 0.0);
                    w.z = e.value("z", (int64_t)0);
                    w.configJson =
                        e.contains("config") ? e["config"].dump() : "{}";
                    ws.push_back(w);
                }
                store_.saveSceneWidgets(id, ws);
                return ApiResponse::json("{\"ok\":true}");
            }
            if (req.method == "POST") {
                nlohmann::json e = parseBody(req.body);
                SceneWidget w;
                w.sceneId = id;
                w.widgetType = e.value("type", "");
                w.x = e.value("x", 0.0);
                w.y = e.value("y", 0.0);
                w.w = e.value("w", 0.0);
                w.h = e.value("h", 0.0);
                w.z = e.value("z", (int64_t)0);
                w.configJson = e.contains("config") ? e["config"].dump() : "{}";
                auto added = store_.addSceneWidget(w);
                return ApiResponse::json(widgetJson(*added).dump(), 201);
            }
        }
        // /api/scenes/:id/duplicate
        if (seg.size() == 4 && seg[3] == "duplicate" && req.method == "POST") {
            nlohmann::json b = parseBody(req.body);
            auto s = store_.duplicateScene(id, b.value("name", "Copy"));
            if (!s) return ApiResponse::err(400, "duplicate failed");
            return ApiResponse::json(
                nlohmann::json({{"id", s->id}, {"name", s->name}}).dump(), 201);
        }
        // /api/scenes/:id/load | start | stop
        if (seg.size() == 4 && seg[3] == "load" && req.method == "POST") {
            engine_.postLoadScene(id, /*resume=*/false);
            if (onSceneLoad) onSceneLoad(id);
            return ApiResponse::json("{\"ok\":true}");
        }
        if (seg.size() == 4 && seg[3] == "start" && req.method == "POST") {
            engine_.postStart();
            if (onStart) onStart();
            return ApiResponse::json("{\"ok\":true}");
        }
        if (seg.size() == 4 && seg[3] == "stop" && req.method == "POST") {
            engine_.postStop();
            if (onStop) onStop();
            return ApiResponse::json("{\"ok\":true}");
        }
        if (seg.size() == 3) {
            if (req.method == "PUT") {
                nlohmann::json b = parseBody(req.body);
                if (b.contains("name"))
                    store_.renameScene(id, b["name"].get<std::string>());
                return ApiResponse::json("{\"ok\":true}");
            }
            if (req.method == "DELETE") {
                store_.deleteScene(id);
                return ApiResponse::json("{\"ok\":true}");
            }
        }
    }
    return ApiResponse::err(404, "not found");
}

// ---------------------------------------------------------------------------
// Widgets / registry / install
// ---------------------------------------------------------------------------
ApiResponse RestApi::routeWidgets(const ApiRequest& req,
                                  const std::vector<std::string>& seg) {
    if (seg[1] == "widget-registry" && req.method == "GET")
        return ApiResponse::json(registry_.registryJson());
    if (seg[1] == "widgets" && seg.size() == 3 && seg[2] == "install" &&
        req.method == "POST") {
        std::vector<uint8_t> zip(req.body.begin(), req.body.end());
        std::string err;
        auto m = registry_.installFromZip(zip, err);
        if (!m) return ApiResponse::err(400, err);
        return ApiResponse::json(
            nlohmann::json({{"type", m->type}, {"version", m->version}}).dump(), 201);
    }
    if (seg[1] == "widgets" && seg.size() == 3) {
        int64_t id = toId(seg[2]);
        if (req.method == "PUT") {
            nlohmann::json b = parseBody(req.body);
            auto w = store_.sceneWidget(id);
            if (!w) return ApiResponse::err(404, "no such widget");
            w->x = b.value("x", w->x);
            w->y = b.value("y", w->y);
            w->w = b.value("w", w->w);
            w->h = b.value("h", w->h);
            w->z = b.value("z", w->z);
            if (b.contains("config")) w->configJson = b["config"].dump();
            store_.updateSceneWidget(*w);
            return ApiResponse::json("{\"ok\":true}");
        }
        if (req.method == "DELETE") {
            store_.deleteSceneWidget(id);
            return ApiResponse::json("{\"ok\":true}");
        }
    }
    return ApiResponse::err(404, "not found");
}

// ---------------------------------------------------------------------------
// Shifts
// ---------------------------------------------------------------------------
ApiResponse RestApi::routeShifts(const ApiRequest& req,
                                 const std::vector<std::string>& seg) {
    if (seg.size() == 3 && seg[2] == "new" && req.method == "POST") {
        engine_.postNewShift();
        if (onNewShift) onNewShift();
        return ApiResponse::json("{\"ok\":true}");
    }
    if (seg.size() == 4 && seg[3] == "report" && req.method == "GET") {
        int64_t id = toId(seg[2]);
        return ApiResponse::json(store_.shiftReportJson(id));
    }
    return ApiResponse::err(404, "not found");
}

// ---------------------------------------------------------------------------
// Services (gift gallery, faces)
// ---------------------------------------------------------------------------
ApiResponse RestApi::routeServices(const ApiRequest& req,
                                   const std::vector<std::string>& seg) {
    if (seg[1] == "gift-gallery" && req.method == "GET") {
        return ApiResponse::json(engine_.giftGalleryJsonArray());
    }
    if (seg[1] == "faces" && req.method == "GET") {
        std::string j = facesJson ? facesJson() : "[]";
        return ApiResponse::json(j);
    }
    return ApiResponse::err(404, "not found");
}

ApiResponse RestApi::routeRuntime(const ApiRequest&,
                                  const std::vector<std::string>&) {
    return ApiResponse::err(404, "not found");
}

}  // namespace evo
