#pragma once

// RestApi — the EvoControl REST surface (§11.3), decoupled from the HTTP
// transport. The server parses a request into an ApiRequest and calls
// handle(); the returned ApiResponse carries the status, content type, and
// body. This keeps all route logic testable and civetweb-agnostic.
//
// Native-app actions (load account/scene, Start/Stop, New Shift) are also
// reachable here so the native console and any localhost tooling share one
// implementation; game intents go over the WS only (§11).

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace evo {

class Store;
class WidgetRegistry;
class GameEngine;

struct ApiRequest {
    std::string method;   // GET / POST / PUT / DELETE
    std::string path;     // /api/...
    std::string query;    // raw query string
    std::string body;     // request body (JSON or raw upload)
    bool authed = false;  // control-scope authentication satisfied
};

struct ApiResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    static ApiResponse json(const std::string& j, int status = 200) {
        return {status, "application/json", j};
    }
    static ApiResponse err(int status, const std::string& msg);
};

class RestApi {
public:
    RestApi(Store& store, WidgetRegistry& registry, GameEngine& engine);

    // Called by the native/native-app actions that require host-side effects
    // beyond the store (loading account/scene into the engine). Set by main.
    // For account/scene load the engine posts are performed here.
    ApiResponse handle(const ApiRequest& req);

    // Hooks the native app can set to react to REST-driven runtime changes
    // (e.g. connect the ttlive client when an account is loaded).
    std::function<void(int64_t accountId)> onAccountLoaded;
    std::function<void(int64_t sceneId)> onSceneLoad;
    std::function<void()> onStart;
    std::function<void()> onStop;
    std::function<void()> onNewShift;
    // Resolve a cached user/performer avatar file path for serving.
    std::function<std::string(int64_t userId)> resolveUserAvatar;
    std::function<std::string(int64_t performerId)> resolvePerformerAvatar;
    // Latest face-detector output (JSON array) for GET /api/faces.
    std::function<std::string()> facesJson;

private:
    // Route helpers.
    ApiResponse routeAccounts(const ApiRequest&, const std::vector<std::string>&);
    ApiResponse routePerformers(const ApiRequest&, const std::vector<std::string>&);
    ApiResponse routeScenes(const ApiRequest&, const std::vector<std::string>&);
    ApiResponse routeWidgets(const ApiRequest&, const std::vector<std::string>&);
    ApiResponse routeShifts(const ApiRequest&, const std::vector<std::string>&);
    ApiResponse routeRuntime(const ApiRequest&, const std::vector<std::string>&);
    ApiResponse routeServices(const ApiRequest&, const std::vector<std::string>&);

    Store& store_;
    WidgetRegistry& registry_;
    GameEngine& engine_;
};

// Split a path into non-empty segments (URL-decoded already by the server).
std::vector<std::string> splitPath(const std::string& path);

}  // namespace evo
