#include "db.hpp"

#include <cstring>

#include "sqlite3.h"

namespace evo {

// ---------------------------------------------------------------------------
// Stmt
// ---------------------------------------------------------------------------

Stmt::Stmt(sqlite3* db, const std::string& sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql.c_str(), (int)sql.size(), &stmt_, nullptr) !=
        SQLITE_OK) {
        stmt_ = nullptr;
    }
}

Stmt::~Stmt() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Stmt::Stmt(Stmt&& o) noexcept : db_(o.db_), stmt_(o.stmt_) {
    o.stmt_ = nullptr;
    o.db_ = nullptr;
}

Stmt& Stmt::operator=(Stmt&& o) noexcept {
    if (this != &o) {
        if (stmt_) sqlite3_finalize(stmt_);
        db_ = o.db_;
        stmt_ = o.stmt_;
        o.stmt_ = nullptr;
        o.db_ = nullptr;
    }
    return *this;
}

Stmt& Stmt::bind(int idx, int64_t v) {
    if (stmt_) sqlite3_bind_int64(stmt_, idx, v);
    return *this;
}

Stmt& Stmt::bind(int idx, double v) {
    if (stmt_) sqlite3_bind_double(stmt_, idx, v);
    return *this;
}

Stmt& Stmt::bind(int idx, const std::string& v) {
    if (stmt_)
        sqlite3_bind_text(stmt_, idx, v.c_str(), (int)v.size(),
                          SQLITE_TRANSIENT);
    return *this;
}

Stmt& Stmt::bindNull(int idx) {
    if (stmt_) sqlite3_bind_null(stmt_, idx);
    return *this;
}

Stmt& Stmt::bind(int idx, const std::optional<int64_t>& v) {
    if (v)
        return bind(idx, *v);
    return bindNull(idx);
}

Stmt& Stmt::bindBlob(int idx, const void* data, size_t len) {
    if (stmt_)
        sqlite3_bind_blob(stmt_, idx, data, (int)len, SQLITE_TRANSIENT);
    return *this;
}

bool Stmt::step() {
    if (!stmt_) return false;
    return sqlite3_step(stmt_) == SQLITE_ROW;
}

void Stmt::reset(bool clearBindings) {
    if (!stmt_) return;
    sqlite3_reset(stmt_);
    if (clearBindings) sqlite3_clear_bindings(stmt_);
}

int64_t Stmt::columnInt(int col) const {
    return stmt_ ? sqlite3_column_int64(stmt_, col) : 0;
}

double Stmt::columnDouble(int col) const {
    return stmt_ ? sqlite3_column_double(stmt_, col) : 0.0;
}

std::string Stmt::columnText(int col) const {
    if (!stmt_) return {};
    const unsigned char* s = sqlite3_column_text(stmt_, col);
    int n = sqlite3_column_bytes(stmt_, col);
    if (!s || n <= 0) return {};
    return std::string(reinterpret_cast<const char*>(s), (size_t)n);
}

bool Stmt::columnIsNull(int col) const {
    return !stmt_ || sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

std::vector<uint8_t> Stmt::columnBlob(int col) const {
    std::vector<uint8_t> out;
    if (!stmt_) return out;
    const void* p = sqlite3_column_blob(stmt_, col);
    int n = sqlite3_column_bytes(stmt_, col);
    if (p && n > 0) {
        out.resize((size_t)n);
        std::memcpy(out.data(), p, (size_t)n);
    }
    return out;
}

std::optional<int64_t> Stmt::columnOptInt(int col) const {
    if (columnIsNull(col)) return std::nullopt;
    return columnInt(col);
}

// ---------------------------------------------------------------------------
// Db
// ---------------------------------------------------------------------------

Db::~Db() { close(); }

bool Db::open(const std::string& path) {
    close();
    if (sqlite3_open(path.c_str(), &handle_) != SQLITE_OK) {
        error_ = handle_ ? sqlite3_errmsg(handle_) : "sqlite3_open failed";
        if (handle_) {
            sqlite3_close(handle_);
            handle_ = nullptr;
        }
        return false;
    }
    // WAL: crash-safe durability with good concurrency. NORMAL sync is safe
    // under WAL (a crash can only lose the last uncommitted transaction).
    sqlite3_busy_timeout(handle_, 5000);
    if (!exec("PRAGMA journal_mode=WAL;") ||
        !exec("PRAGMA synchronous=NORMAL;") ||
        !exec("PRAGMA foreign_keys=ON;")) {
        return false;
    }
    return migrate();
}

void Db::close() {
    if (handle_) {
        sqlite3_close(handle_);
        handle_ = nullptr;
    }
}

bool Db::exec(const std::string& sql) {
    if (!handle_) {
        error_ = "database not open";
        return false;
    }
    char* err = nullptr;
    if (sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &err) !=
        SQLITE_OK) {
        error_ = err ? err : "exec failed";
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

Stmt Db::prepare(const std::string& sql) {
    Stmt s(handle_, sql);
    if (!s.valid() && handle_) error_ = sqlite3_errmsg(handle_);
    return s;
}

int64_t Db::lastInsertRowId() const {
    return handle_ ? sqlite3_last_insert_rowid(handle_) : 0;
}

bool Db::begin() { return exec("BEGIN IMMEDIATE;"); }
bool Db::commit() { return exec("COMMIT;"); }
bool Db::rollback() { return exec("ROLLBACK;"); }

// ---------------------------------------------------------------------------
// Schema (§12). Idempotent — safe to run on every open.
// ---------------------------------------------------------------------------
bool Db::migrate() {
    static const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS account (
  id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL,
  stream TEXT NOT NULL, cookies TEXT, created_at INTEGER);

CREATE TABLE IF NOT EXISTS performer (
  id INTEGER PRIMARY KEY,
  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
  name TEXT NOT NULL, avatar_path TEXT,
  face_embedding BLOB, face_quality REAL,
  UNIQUE(account_id, name));

CREATE TABLE IF NOT EXISTS shift (
  id INTEGER PRIMARY KEY,
  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
  started_at INTEGER NOT NULL, ended_at INTEGER);

CREATE TABLE IF NOT EXISTS scene (
  id INTEGER PRIMARY KEY,
  account_id INTEGER NOT NULL REFERENCES account(id) ON DELETE CASCADE,
  name TEXT NOT NULL, ordering INTEGER, UNIQUE(account_id, name));

CREATE TABLE IF NOT EXISTS scene_widget (
  id INTEGER PRIMARY KEY,
  scene_id INTEGER NOT NULL REFERENCES scene(id) ON DELETE CASCADE,
  widget_type TEXT NOT NULL,
  x REAL, y REAL, w REAL, h REAL, z INTEGER,
  config_json TEXT);

CREATE TABLE IF NOT EXISTS scene_run (
  id INTEGER PRIMARY KEY,
  scene_id INTEGER NOT NULL REFERENCES scene(id),
  shift_id INTEGER NOT NULL REFERENCES shift(id),
  loaded_at INTEGER, started_at INTEGER, ended_at INTEGER,
  widget_versions_json TEXT);

CREATE TABLE IF NOT EXISTS gift_ledger (
  id INTEGER PRIMARY KEY,
  ts INTEGER NOT NULL,
  account_id INTEGER NOT NULL, shift_id INTEGER NOT NULL,
  scene_run_id INTEGER,
  msg_id INTEGER,
  gifter_id INTEGER, gifter_name TEXT,
  gift_id INTEGER, gift_name TEXT, repeat_count INTEGER,
  diamonds INTEGER NOT NULL,
  performer_id INTEGER,
  source TEXT,
  UNIQUE(msg_id, performer_id));

CREATE INDEX IF NOT EXISTS ledger_shift ON gift_ledger(shift_id, performer_id);
CREATE INDEX IF NOT EXISTS ledger_run   ON gift_ledger(scene_run_id, performer_id);

CREATE TABLE IF NOT EXISTS widget_state (
  scene_run_id INTEGER NOT NULL,
  scene_widget_id INTEGER NOT NULL,
  state_json TEXT, seq INTEGER, updated_at INTEGER,
  PRIMARY KEY (scene_run_id, scene_widget_id));

CREATE TABLE IF NOT EXISTS runtime (
  id INTEGER PRIMARY KEY CHECK (id=1),
  active_account_id INTEGER, active_shift_id INTEGER,
  active_scene_run_id INTEGER, running INTEGER DEFAULT 0);

CREATE TABLE IF NOT EXISTS widget_bundle (
  type TEXT, version TEXT, path TEXT, manifest_json TEXT, installed_at INTEGER,
  PRIMARY KEY (type, version));

CREATE TABLE IF NOT EXISTS setting (key TEXT PRIMARY KEY, value TEXT);

INSERT OR IGNORE INTO runtime(id, running) VALUES (1, 0);
)SQL";
    if (!exec(kSchema)) return false;
    // Additive migrations (ignore "duplicate column" errors on re-run).
    exec("ALTER TABLE performer ADD COLUMN tiktok_user TEXT;");
    return true;
}

}  // namespace evo
