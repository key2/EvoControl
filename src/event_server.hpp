#pragma once

// EventServer — EvoControl's embedded HTTP + WebSocket server (civetweb).
//
//   GET /obs                       OBS host page (transparent, read-only, no auth)
//   GET /control                   Control host page (single active session; auth)
//   GET /widgets/<type>/<ver>/…    installed widget bundle files
//   /api/*                         REST surface (§11.3) via RestApi
//   ws://<host>:<port>/ws          multiplexed WS envelope protocol (§11.2)
//   ws text (legacy)               raw TikTok event feed (broadcast())
//
// WS envelope: { v, scope:"obs"|"control", instance:<id|null>, topic, seq, data }
// Single active control session: a new control `hello` supersedes the previous
// control page (black "control lost" screen); "take back control" re-supersedes.
//
// Thread-safety: broadcast()/broadcastEnvelope() may be called from any thread.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

struct mg_context;
struct mg_connection;

namespace evo {
class RestApi;
class GameEngine;
class WidgetRegistry;
}

/// Directory containing the served website (web/): next to the executable, the
/// CWD, or the source tree. Empty if none found.
std::string findWebDir();
/// Resolve a bundled file next to the executable, CWD, or source tree.
std::string findResource(const std::string& relPath);
/// Writable root for runtime resources (DB, widgets, downloaded models).
std::string resourceRootDir();

class EventServer {
public:
    EventServer() = default;
    ~EventServer() { stop(); }

    EventServer(const EventServer&) = delete;
    EventServer& operator=(const EventServer&) = delete;

    // Wire the EvoControl subsystems (optional: without them the server still
    // serves static files + the raw feed). Set before start().
    void setApi(evo::RestApi* api) { api_ = api; }
    void setEngine(evo::GameEngine* engine) { engine_ = engine; }
    void setRegistry(evo::WidgetRegistry* reg) { registry_ = reg; }
    /// Optional control-view password (bcrypt-free simple gate, §18). Empty =
    /// no auth. Checked for /control, /api mutations, and control WS hello.
    void setControlPassword(const std::string& pw) { controlPassword_ = pw; }

    bool start(int port, const std::string& webRoot);
    void stop();

    /// Send a raw text (JSON) message to every legacy /ws client (event feed).
    void broadcast(const std::string& text);

    /// Broadcast an envelope to pages matching `scope` ("all"|"obs"|"control")
    /// and `instance` (0 = shared/all instances). Called by the engine.
    void broadcastEnvelope(const std::string& scope, int64_t instance,
                           const std::string& topic, const std::string& dataJson);

    bool running() const { return ctx_ != nullptr; }
    int port() const { return port_; }
    int clientCount() const;
    std::string error() const;

private:
    // civetweb websocket callbacks.
    static int wsConnect(const mg_connection* conn, void* user);
    static void wsReady(mg_connection* conn, void* user);
    static int wsData(mg_connection* conn, int flags, char* data, size_t len,
                      void* user);
    static void wsClose(const mg_connection* conn, void* user);

    // civetweb request handlers.
    static int handleApi(mg_connection* conn, void* user);
    static int handleWidgets(mg_connection* conn, void* user);
    static int handlePage(mg_connection* conn, void* user);

    void handleWsFrame(mg_connection* conn, const std::string& msg);
    void sendTo(mg_connection* conn, const std::string& text);

    struct ClientInfo {
        std::string scope;   // "obs" | "control" | "" (legacy)
        int64_t sessionId = 0;
        bool superseded = false;
        bool authed = false;
    };

    mg_context* ctx_ = nullptr;
    int port_ = 0;

    evo::RestApi* api_ = nullptr;
    evo::GameEngine* engine_ = nullptr;
    evo::WidgetRegistry* registry_ = nullptr;
    std::string controlPassword_;
    std::atomic<int64_t> nextSession_{1};
    int64_t activeControl_ = 0;  // session id of the live control page

    mutable std::mutex mutex_;
    std::set<mg_connection*> clients_;                 // legacy raw feed
    std::map<mg_connection*, ClientInfo> wsClients_;   // envelope protocol
    std::string error_;
};
