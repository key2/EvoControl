#include "event_server.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "civetweb.h"
#include "game_engine.hpp"
#include "rest_api.hpp"
#include "widget_registry.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

using nlohmann::json;

namespace {

std::filesystem::path exeDir() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        return std::filesystem::path(buf).parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0)
        return std::filesystem::path(buf).parent_path();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return {};
}

std::string mimeFor(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    if (ext == ".js" || ext == ".mjs") return "text/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".html") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

std::string findWebDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::path base : {exeDir(), fs::current_path(ec)}) {
        if (base.empty()) continue;
        fs::path p = base / "web";
        if (fs::exists(p / "obs" / "index.html", ec) ||
            fs::exists(p / "index.html", ec))
            return p.string();
    }
#ifdef EVOCONTROL_WEB_DIR
    if (fs::exists(fs::path(EVOCONTROL_WEB_DIR) / "obs" / "index.html", ec) ||
        fs::exists(fs::path(EVOCONTROL_WEB_DIR) / "index.html", ec))
        return EVOCONTROL_WEB_DIR;
#endif
#ifdef DEARTT_WEB_DIR
    if (fs::exists(fs::path(DEARTT_WEB_DIR) / "index.html", ec))
        return DEARTT_WEB_DIR;
#endif
    return {};
}

std::string resourceRootDir() {
    namespace fs = std::filesystem;
    fs::path p = exeDir();
    if (!p.empty()) return p.string();
    std::error_code ec;
    return fs::current_path(ec).string();
}

std::string findResource(const std::string& relPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::path base : {exeDir(), fs::current_path(ec)}) {
        if (base.empty()) continue;
        fs::path p = base / relPath;
        if (fs::exists(p, ec)) return p.string();
    }
#ifdef EVOCONTROL_SOURCE_DIR
    fs::path p = fs::path(EVOCONTROL_SOURCE_DIR) / relPath;
    if (fs::exists(p, ec)) return p.string();
#endif
#ifdef DEARTT_SOURCE_DIR
    fs::path q = fs::path(DEARTT_SOURCE_DIR) / relPath;
    if (fs::exists(q, ec)) return q.string();
#endif
    return {};
}

// ---------------------------------------------------------------------------
// Server lifecycle
// ---------------------------------------------------------------------------
bool EventServer::start(int port, const std::string& webRoot) {
    stop();

    static bool initialized = false;
    if (!initialized) {
        mg_init_library(0);
        initialized = true;
    }

    mg_callbacks callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));

    for (int p = port; p < port + 10 && p < 65536; p++) {
        std::string ports = std::to_string(p);
        const char* options[] = {
            "listening_ports",          ports.c_str(),
            "document_root",            webRoot.empty() ? "." : webRoot.c_str(),
            "enable_directory_listing", "no",
            "num_threads",              "12",
            nullptr,
        };
        ctx_ = mg_start(&callbacks, this, options);
        if (ctx_) {
            mg_set_websocket_handler(ctx_, "/ws", &EventServer::wsConnect,
                                     &EventServer::wsReady,
                                     &EventServer::wsData,
                                     &EventServer::wsClose, this);
            mg_set_request_handler(ctx_, "/api/", &EventServer::handleApi, this);
            mg_set_request_handler(ctx_, "/widgets/",
                                   &EventServer::handleWidgets, this);
            mg_set_request_handler(ctx_, "/obs", &EventServer::handlePage, this);
            mg_set_request_handler(ctx_, "/control", &EventServer::handlePage,
                                   this);
            port_ = p;
            return true;
        }
    }

    std::lock_guard<std::mutex> lk(mutex_);
    error_ = "failed to start web server on ports " + std::to_string(port) +
             "-" + std::to_string(port + 9);
    return false;
}

void EventServer::stop() {
    if (!ctx_) return;
    mg_stop(ctx_);
    ctx_ = nullptr;
    port_ = 0;
    std::lock_guard<std::mutex> lk(mutex_);
    clients_.clear();
    wsClients_.clear();
}

// ---------------------------------------------------------------------------
// Raw event feed (legacy) + envelope broadcast
// ---------------------------------------------------------------------------
void EventServer::broadcast(const std::string& text) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (mg_connection* conn : clients_)
        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, text.c_str(),
                           text.size());
}

void EventServer::broadcastEnvelope(const std::string& scope, int64_t instance,
                                    const std::string& topic,
                                    const std::string& dataJson) {
    json data = json::parse(dataJson, nullptr, false);
    json env;
    env["v"] = 1;
    env["instance"] = instance ? json(instance) : json(nullptr);
    env["topic"] = topic;
    env["data"] = data.is_discarded() ? json::object() : data;
    std::string text = env.dump();

    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& [conn, info] : wsClients_) {
        if (info.superseded) continue;
        if (scope != "all" && info.scope != scope) continue;
        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, text.c_str(),
                           text.size());
    }
}

void EventServer::sendTo(mg_connection* conn, const std::string& text) {
    mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, text.c_str(),
                       text.size());
}

int EventServer::clientCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return (int)clients_.size() + (int)wsClients_.size();
}

std::string EventServer::error() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return error_;
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------
int EventServer::wsConnect(const mg_connection*, void*) { return 0; }

void EventServer::wsReady(mg_connection* conn, void* user) {
    auto* self = (EventServer*)user;
    std::lock_guard<std::mutex> lk(self->mutex_);
    // Registered as a legacy feed client by default; the first `hello` frame
    // upgrades it to an envelope client. We keep it in clients_ so raw events
    // still flow to plain listeners; envelope clients are tracked separately
    // once they say hello.
    self->clients_.insert(conn);
    static const char hello[] =
        "{\"type\":\"Hello\",\"data\":{\"app\":\"EvoControl\","
        "\"endpoint\":\"/ws\"}}";
    mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, hello,
                       sizeof(hello) - 1);
}

int EventServer::wsData(mg_connection* conn, int flags, char* data, size_t len,
                        void* user) {
    auto* self = (EventServer*)user;
    int op = flags & 0x0F;
    if (op == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) return 0;
    if (op == MG_WEBSOCKET_OPCODE_TEXT && data && len > 0)
        self->handleWsFrame(conn, std::string(data, len));
    return 1;
}

void EventServer::wsClose(const mg_connection* conn, void* user) {
    auto* self = (EventServer*)user;
    std::lock_guard<std::mutex> lk(self->mutex_);
    self->clients_.erase(const_cast<mg_connection*>(conn));
    self->wsClients_.erase(const_cast<mg_connection*>(conn));
}

void EventServer::handleWsFrame(mg_connection* conn, const std::string& msg) {
    json j = json::parse(msg, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    std::string topic = j.value("topic", "");

    if (topic == "hello") {
        std::string scope = "obs";
        if (j.contains("data") && j["data"].is_object())
            scope = j["data"].value("scope", "obs");
        std::string auth;
        if (j.contains("data") && j["data"].is_object())
            auth = j["data"].value("auth", "");

        ClientInfo info;
        info.scope = scope;
        info.sessionId = nextSession_++;
        info.authed = controlPassword_.empty() || auth == controlPassword_;

        std::string snapshot;
        int64_t supersededSession = 0;
        mg_connection* supersededConn = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            // Envelope clients are no longer part of the raw feed set.
            clients_.erase(conn);
            if (scope == "control") {
                if (!info.authed) {
                    // Reject: tell the page it is unauthorized.
                    json e = {{"v", 1},
                              {"topic", "session"},
                              {"data", {{"status", "unauthorized"}}}};
                    sendTo(conn, e.dump());
                    return;
                }
                // Supersede the previous control session.
                for (auto& [c, ci] : wsClients_) {
                    if (ci.scope == "control" && !ci.superseded) {
                        ci.superseded = true;
                        supersededConn = c;
                        supersededSession = ci.sessionId;
                    }
                }
                activeControl_ = info.sessionId;
            }
            wsClients_[conn] = info;
        }

        // Grant this page its session.
        json grant = {{"v", 1},
                      {"topic", "session"},
                      {"data", {{"id", info.sessionId}, {"scope", scope}}}};
        sendTo(conn, grant.dump());

        // Notify the superseded control page.
        if (supersededConn) {
            json sup = {{"v", 1},
                        {"topic", "session"},
                        {"data", {{"status", "superseded"}}}};
            sendTo(supersededConn, sup.dump());
        }

        // Late joiner snapshot (§6.2).
        if (engine_) {
            snapshot = engine_->snapshotJson();
            json snap = {{"v", 1},
                         {"topic", "snapshot"},
                         {"data", json::parse(snapshot, nullptr, false)}};
            sendTo(conn, snap.dump());
        }
        return;
    }

    if (topic == "takeback") {
        // "Take back control" — make this the active session, supersede others.
        mg_connection* other = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = wsClients_.find(conn);
            if (it == wsClients_.end() || it->second.scope != "control") return;
            it->second.superseded = false;
            for (auto& [c, ci] : wsClients_) {
                if (c != conn && ci.scope == "control" && !ci.superseded) {
                    ci.superseded = true;
                    other = c;
                }
            }
            activeControl_ = it->second.sessionId;
        }
        json grant = {{"v", 1},
                      {"topic", "session"},
                      {"data", {{"status", "active"}}}};
        sendTo(conn, grant.dump());
        if (other) {
            json sup = {{"v", 1},
                        {"topic", "session"},
                        {"data", {{"status", "superseded"}}}};
            sendTo(other, sup.dump());
        }
        if (engine_) {
            json snap = {{"v", 1},
                         {"topic", "snapshot"},
                         {"data", json::parse(engine_->snapshotJson(), nullptr,
                                              false)}};
            sendTo(conn, snap.dump());
        }
        return;
    }

    if (topic == "intent") {
        // Only the active control session may send intents (stale rejected).
        bool ok = false;
        int64_t instance = 0;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = wsClients_.find(conn);
            if (it != wsClients_.end() && it->second.scope == "control" &&
                !it->second.superseded)
                ok = true;
        }
        if (!ok) return;
        if (j.contains("instance") && j["instance"].is_number())
            instance = j["instance"].get<int64_t>();
        std::string data = j.contains("data") ? j["data"].dump() : "{}";
        if (engine_) engine_->postIntent(instance, data);
        return;
    }
}

// ---------------------------------------------------------------------------
// REST bridge
// ---------------------------------------------------------------------------
int EventServer::handleApi(mg_connection* conn, void* user) {
    auto* self = (EventServer*)user;
    const mg_request_info* ri = mg_get_request_info(conn);
    if (!self->api_) {
        mg_printf(conn,
                  "HTTP/1.1 503 Service Unavailable\r\nContent-Length: "
                  "0\r\n\r\n");
        return 503;
    }
    evo::ApiRequest req;
    req.method = ri->request_method ? ri->request_method : "GET";
    req.path = ri->local_uri ? ri->local_uri : "";
    req.query = ri->query_string ? ri->query_string : "";
    // Read body.
    char buf[8192];
    int r;
    while ((r = mg_read(conn, buf, sizeof(buf))) > 0)
        req.body.append(buf, (size_t)r);
    // Auth: control password gate. Localhost is trusted; if a password is set,
    // require it via the X-Evo-Auth header (defense-in-depth, §18).
    if (self->controlPassword_.empty()) {
        req.authed = true;
    } else {
        const char* h = mg_get_header(conn, "X-Evo-Auth");
        req.authed = h && self->controlPassword_ == h;
    }

    evo::ApiResponse resp = self->api_->handle(req);

    // "@file" content type is a sentinel: stream the file at resp.body.
    if (resp.contentType == "@file") {
        std::string data = readFile(resp.body);
        if (data.empty()) {
            mg_printf(conn,
                      "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            return 404;
        }
        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nCache-Control: "
                  "max-age=3600\r\nContent-Length: %zu\r\n\r\n",
                  mimeFor(resp.body).c_str(), data.size());
        mg_write(conn, data.data(), data.size());
        return 200;
    }

    mg_printf(conn,
              "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nAccess-Control-Allow-"
              "Origin: *\r\nContent-Length: %zu\r\n\r\n",
              resp.status, resp.status < 300 ? "OK" : "Error",
              resp.contentType.c_str(), resp.body.size());
    mg_write(conn, resp.body.data(), resp.body.size());
    return resp.status;
}

int EventServer::handleWidgets(mg_connection* conn, void* user) {
    auto* self = (EventServer*)user;
    const mg_request_info* ri = mg_get_request_info(conn);
    if (!self->registry_) {
        mg_printf(conn, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: "
                        "0\r\n\r\n");
        return 503;
    }
    // /widgets/<type>/<version>/<rel...>
    std::string uri = ri->local_uri ? ri->local_uri : "";
    auto seg = evo::splitPath(uri);  // ["widgets", type, version, rel...]
    if (seg.size() < 4) {
        mg_printf(conn, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        return 404;
    }
    std::string type = seg[1], version = seg[2];
    std::string rel;
    for (size_t i = 3; i < seg.size(); i++) {
        if (i > 3) rel += "/";
        rel += seg[i];
    }
    std::string path = self->registry_->resolveFile(type, version, rel);
    if (path.empty()) {
        mg_printf(conn, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        return 404;
    }
    std::string data = readFile(path);
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nAccess-Control-Allow-"
              "Origin: *\r\nContent-Length: %zu\r\n\r\n",
              mimeFor(path).c_str(), data.size());
    mg_write(conn, data.data(), data.size());
    return 200;
}

int EventServer::handlePage(mg_connection* conn, void* user) {
    auto* self = (EventServer*)user;
    const mg_request_info* ri = mg_get_request_info(conn);
    std::string uri = ri->local_uri ? ri->local_uri : "";
    bool obs = uri.rfind("/obs", 0) == 0;
    // The Control page is password-gated when a password is set (§18). We serve
    // the page regardless (the page shows a login form and authenticates over
    // the WS hello), keeping the gate at the WS/REST layer.
    std::string web = findWebDir();
    std::string file =
        obs ? (web + "/obs/index.html") : (web + "/control/index.html");
    std::string data = readFile(file);
    if (data.empty()) {
        // Fallback minimal page if the host bundle isn't deployed yet.
        data = obs ? "<!doctype html><meta charset=utf-8><title>OBS</title>"
                     "<body style='margin:0;background:transparent'>"
                     "<script type=module src='/obs/host.js'></script>"
                   : "<!doctype html><meta charset=utf-8><title>Control</title>"
                     "<body><script type=module src='/control/host.js'>"
                     "</script>";
    }
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: "
              "%zu\r\n\r\n",
              data.size());
    mg_write(conn, data.data(), data.size());
    return 200;
}
