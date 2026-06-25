/*
 * database.cpp  –  SQLite persistence layer implementation
 *
 * Every public method acquires mutex_ before touching the sqlite3* handle.
 * This makes the class safe to call from multiple threads simultaneously
 * (which cpp-httplib does for concurrent requests).
 *
 * All SQL parameters are bound with sqlite3_bind_* – never via string
 * concatenation – so user-supplied data cannot alter query structure.
 */

#include "database.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

// ============================================================
// SqliteStatement  –  RAII prepared statement
// ============================================================

SqliteStatement::SqliteStatement(sqlite3* db, const std::string& sql) {
    // sqlite3_prepare_v2 compiles the SQL into bytecode stored in stmt_.
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(
            std::string("SQL prepare failed: ") + sqlite3_errmsg(db));
    }
}

SqliteStatement::~SqliteStatement() {
    // sqlite3_finalize is a no-op when stmt_ is nullptr.
    if (stmt_) {
        sqlite3_finalize(stmt_);
    }
}

// ============================================================
// Database  –  connection lifecycle
// ============================================================

Database::Database(const std::string& db_path) {
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);   // must close even on error
        db_ = nullptr;
        throw std::runtime_error("Cannot open database '" + db_path + "': " + err);
    }

    // WAL journal mode allows concurrent reads while a write is in progress.
    executeSQL("PRAGMA journal_mode=WAL;");

    // Enforce referential integrity (good practice even without FK columns).
    executeSQL("PRAGMA foreign_keys=ON;");
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

// ============================================================
// initialize  –  DDL
// ============================================================

void Database::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    // CREATE TABLE IF NOT EXISTS – idempotent; safe to call on every startup.
    const char* ddl = R"(
        CREATE TABLE IF NOT EXISTS urls (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            short_code      TEXT    UNIQUE NOT NULL,
            original_url    TEXT    NOT NULL,
            created_at      INTEGER NOT NULL,
            expires_at      INTEGER,
            click_count     INTEGER DEFAULT 0,
            last_clicked_at INTEGER
        );

        CREATE INDEX IF NOT EXISTS idx_short_code ON urls(short_code);
    )";

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, ddl, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string err(err_msg);
        sqlite3_free(err_msg);
        throw std::runtime_error("Database initialization failed: " + err);
    }

    std::cout << "[Database] Schema ready.\n";
}

// ============================================================
// Private helpers
// ============================================================

void Database::executeSQL(const std::string& sql) {
    // NOTE: called from the constructor (before mutex is needed) and from
    // initialize() (where the mutex is already held by the caller).
    // We do NOT lock here to avoid deadlocking with initialize().
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string err(err_msg);
        sqlite3_free(err_msg);
        throw std::runtime_error("SQL execution failed: " + err);
    }
}

// Reads columns 0-6 from the current row of a stepped statement.
// Column order must match every SELECT that uses this helper:
//   0 id, 1 short_code, 2 original_url, 3 created_at,
//   4 expires_at, 5 click_count, 6 last_clicked_at
UrlRecord Database::readRecord(sqlite3_stmt* stmt) {
    UrlRecord r;

    r.id          = sqlite3_column_int64(stmt, 0);
    r.short_code  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    r.original_url = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    r.created_at  = sqlite3_column_int64(stmt, 3);

    if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
        r.expires_at = sqlite3_column_int64(stmt, 4);
    }

    r.click_count = sqlite3_column_int64(stmt, 5);

    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
        r.last_clicked_at = sqlite3_column_int64(stmt, 6);
    }

    return r;
}

// ============================================================
// insertUrl
// ============================================================

UrlRecord Database::insertUrl(const std::string&    short_code,
                              const std::string&    original_url,
                              std::optional<int64_t> expires_at) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Use the system clock for created_at so it aligns with real time.
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

    SqliteStatement stmt(db_,
        "INSERT INTO urls (short_code, original_url, created_at, expires_at) "
        "VALUES (?, ?, ?, ?)");

    // Bind parameters – SQLITE_TRANSIENT copies the strings so they stay valid
    // after the bind call even if the std::string is later modified.
    sqlite3_bind_text(stmt.get(), 1, short_code.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, original_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 3, now);

    if (expires_at.has_value()) {
        sqlite3_bind_int64(stmt.get(), 4, expires_at.value());
    } else {
        sqlite3_bind_null(stmt.get(), 4);
    }

    int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(
            std::string("INSERT failed: ") + sqlite3_errmsg(db_));
    }

    // Build and return the newly-inserted record without a second round-trip.
    UrlRecord r;
    r.id           = sqlite3_last_insert_rowid(db_);
    r.short_code   = short_code;
    r.original_url = original_url;
    r.created_at   = now;
    r.expires_at   = expires_at;
    r.click_count  = 0;
    // last_clicked_at stays nullopt (was never clicked yet)
    return r;
}

// ============================================================
// getUrlByCode
// ============================================================

std::optional<UrlRecord> Database::getUrlByCode(const std::string& short_code) {
    std::lock_guard<std::mutex> lock(mutex_);

    SqliteStatement stmt(db_,
        "SELECT id, short_code, original_url, created_at, expires_at, "
        "       click_count, last_clicked_at "
        "FROM   urls "
        "WHERE  short_code = ?");

    sqlite3_bind_text(stmt.get(), 1, short_code.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        return readRecord(stmt.get());
    }
    if (rc == SQLITE_DONE) {
        return std::nullopt;   // no such row
    }
    throw std::runtime_error(
        std::string("SELECT failed: ") + sqlite3_errmsg(db_));
}

// ============================================================
// getAllUrls
// ============================================================

std::vector<UrlRecord> Database::getAllUrls() {
    std::lock_guard<std::mutex> lock(mutex_);

    SqliteStatement stmt(db_,
        "SELECT id, short_code, original_url, created_at, expires_at, "
        "       click_count, last_clicked_at "
        "FROM   urls "
        "ORDER BY created_at DESC");

    std::vector<UrlRecord> rows;
    int rc;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        rows.push_back(readRecord(stmt.get()));
    }
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(
            std::string("SELECT ALL failed: ") + sqlite3_errmsg(db_));
    }
    return rows;
}

// ============================================================
// deleteUrl
// ============================================================

bool Database::deleteUrl(const std::string& short_code) {
    std::lock_guard<std::mutex> lock(mutex_);

    SqliteStatement stmt(db_, "DELETE FROM urls WHERE short_code = ?");
    sqlite3_bind_text(stmt.get(), 1, short_code.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(
            std::string("DELETE failed: ") + sqlite3_errmsg(db_));
    }

    // sqlite3_changes returns the number of rows affected by the last statement.
    return sqlite3_changes(db_) > 0;
}

// ============================================================
// recordClick
// ============================================================

void Database::recordClick(const std::string& short_code, int64_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    SqliteStatement stmt(db_,
        "UPDATE urls "
        "SET    click_count     = click_count + 1, "
        "       last_clicked_at = ? "
        "WHERE  short_code = ?");

    sqlite3_bind_int64(stmt.get(), 1, timestamp);
    sqlite3_bind_text(stmt.get(), 2, short_code.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(
            std::string("UPDATE click failed: ") + sqlite3_errmsg(db_));
    }
}

// ============================================================
// codeExists
// ============================================================

bool Database::codeExists(const std::string& short_code) {
    std::lock_guard<std::mutex> lock(mutex_);

    SqliteStatement stmt(db_,
        "SELECT COUNT(*) FROM urls WHERE short_code = ?");
    sqlite3_bind_text(stmt.get(), 1, short_code.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        return sqlite3_column_int(stmt.get(), 0) > 0;
    }
    // On unexpected error, assume not found (insert will fail with UNIQUE anyway).
    return false;
}
