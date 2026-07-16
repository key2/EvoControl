#pragma once

// Db — a thin RAII wrapper over the vendored SQLite3 amalgamation, opened in
// WAL mode (crash-safe: committed ledger rows survive any crash/restart, which
// is the salary-safety requirement of EvoControl §10).
//
// The wrapper is deliberately minimal: an open()/exec()/prepare() surface plus
// a Stmt helper that binds by index and reads typed columns. All EvoControl
// persistence (accounts, performers, shifts, scenes, the gift ledger, widget
// state, settings) is expressed on top of it.
//
// Threading: a single Db (and its sqlite3* handle) is used from the engine /
// server threads under the caller's own locking. SQLite is compiled
// THREADSAFE=1 (serialized) so concurrent use is safe but callers should still
// serialize logically-related statements (transactions).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace evo {

class Db;

/// A prepared statement. Bind 1-indexed parameters, step(), read columns.
class Stmt {
public:
    Stmt() = default;
    Stmt(sqlite3* db, const std::string& sql);
    ~Stmt();

    Stmt(Stmt&&) noexcept;
    Stmt& operator=(Stmt&&) noexcept;
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool valid() const { return stmt_ != nullptr; }

    // Binding (1-indexed). Chainable.
    Stmt& bind(int idx, int64_t v);
    Stmt& bind(int idx, int v) { return bind(idx, (int64_t)v); }
    Stmt& bind(int idx, double v);
    Stmt& bind(int idx, const std::string& v);
    Stmt& bindNull(int idx);
    /// Bind an optional int64: null when nullopt.
    Stmt& bind(int idx, const std::optional<int64_t>& v);
    /// Bind a blob.
    Stmt& bindBlob(int idx, const void* data, size_t len);

    /// Advance one row. Returns true if a row is available, false at the end.
    bool step();

    /// Reset for re-execution (keeps bindings unless clearBindings).
    void reset(bool clearBindings = true);

    // Column readers (0-indexed).
    int64_t columnInt(int col) const;
    double columnDouble(int col) const;
    std::string columnText(int col) const;
    bool columnIsNull(int col) const;
    std::vector<uint8_t> columnBlob(int col) const;
    std::optional<int64_t> columnOptInt(int col) const;

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Db {
public:
    Db() = default;
    ~Db();

    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    /// Open (creating if needed) the database at `path` in WAL mode and run the
    /// schema migrations. Returns false on failure (error() has the message).
    bool open(const std::string& path);
    void close();

    bool isOpen() const { return handle_ != nullptr; }
    sqlite3* handle() const { return handle_; }
    std::string error() const { return error_; }

    /// Execute one or more statements (no results). Returns false on error.
    bool exec(const std::string& sql);

    /// Prepare a statement.
    Stmt prepare(const std::string& sql);

    /// rowid of the last INSERT.
    int64_t lastInsertRowId() const;

    // Convenience transaction helpers (BEGIN IMMEDIATE / COMMIT / ROLLBACK).
    bool begin();
    bool commit();
    bool rollback();

private:
    bool migrate();

    sqlite3* handle_ = nullptr;
    std::string error_;
};

}  // namespace evo
