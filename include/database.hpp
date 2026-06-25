#pragma once
/*
 * database.hpp  –  SQLite persistence layer
 *
 * Design principles:
 *   • RAII: the Database class owns the sqlite3* handle and closes it in its
 *     destructor.  SqliteStatement owns sqlite3_stmt* and finalizes it.
 *   • Thread-safety: a std::mutex guards every public method so the server can
 *     serve concurrent requests without data races.
 *   • Parameterized queries: every user-supplied value is bound with
 *     sqlite3_bind_* to prevent SQL injection.
 *
 * Layer order:
 *   HTTP routes  →  UrlService  →  Database  →  SQLite
 */

#include <sqlite3.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// ============================================================
// UrlRecord – a plain-data snapshot of one row in the urls table
// ============================================================
struct UrlRecord {
    int64_t     id;
    std::string short_code;
    std::string original_url;
    int64_t     created_at;             // Unix seconds
    std::optional<int64_t> expires_at;  // nullopt  →  never expires
    int64_t     click_count;
    std::optional<int64_t> last_clicked_at;
};

// ============================================================
// SqliteStatement  –  RAII wrapper for sqlite3_stmt*
//
// Calling code creates one of these on the stack; when it goes out
// of scope (normally or by exception) sqlite3_finalize is called
// automatically.
// ============================================================
class SqliteStatement {
public:
    // Prepares `sql` against `db`.  Throws std::runtime_error on failure.
    explicit SqliteStatement(sqlite3* db, const std::string& sql);

    // Finalizes the statement (safe to call even if stmt_ is nullptr).
    ~SqliteStatement();

    // Non-copyable – this object exclusively owns its statement handle.
    SqliteStatement(const SqliteStatement&)            = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;

    // Raw handle accessor; needed for sqlite3_bind_* and sqlite3_step calls.
    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

// ============================================================
// Database  –  thread-safe SQLite wrapper
// ============================================================
class Database {
public:
    // Opens (or creates) the database file at `db_path`.
    // Pass ":memory:" for an in-memory database (handy in unit tests).
    // Throws std::runtime_error if the file cannot be opened.
    explicit Database(const std::string& db_path);

    // Closes the SQLite connection.
    ~Database();

    // Non-copyable – owns the sqlite3* handle.
    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    // Creates the urls table and its index if they do not already exist.
    // Must be called once after construction, before any other method.
    void initialize();

    // Inserts a new row and returns it as a UrlRecord.
    // Throws std::runtime_error if the INSERT fails (e.g. UNIQUE violation).
    UrlRecord insertUrl(const std::string&    short_code,
                        const std::string&    original_url,
                        std::optional<int64_t> expires_at);

    // Looks up a row by short_code.
    // Returns nullopt when no matching row exists.
    std::optional<UrlRecord> getUrlByCode(const std::string& short_code);

    // Returns every row, newest first (created_at DESC).
    std::vector<UrlRecord> getAllUrls();

    // Deletes the row for short_code.
    // Returns true if a row was actually removed, false otherwise.
    bool deleteUrl(const std::string& short_code);

    // Atomically increments click_count and sets last_clicked_at.
    void recordClick(const std::string& short_code, int64_t timestamp);

    // Returns true when a row with the given short_code exists.
    // (Used for uniqueness checks before INSERT.)
    bool codeExists(const std::string& short_code);

private:
    sqlite3*           db_    = nullptr;
    mutable std::mutex mutex_; // serialises all sqlite3 calls

    // Executes raw SQL with no parameters; used for PRAGMA and DDL.
    void executeSQL(const std::string& sql);

    // Reads all seven columns from the current row into a UrlRecord.
    static UrlRecord readRecord(sqlite3_stmt* stmt);
};
