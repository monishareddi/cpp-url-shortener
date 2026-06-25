#pragma once
/*
 * url_service.hpp  –  Business logic layer
 *
 * UrlService is the only component that knows about:
 *   • URL validation rules
 *   • Short-code generation / collision avoidance
 *   • Expiry checking
 *   • Click recording on redirect
 *
 * It is deliberately ignorant of HTTP; the HTTP layer calls it and
 * translates its exceptions into appropriate HTTP status codes.
 *
 * Layer order:
 *   HTTP routes  →  UrlService  →  Database  →  SQLite
 */

#include "database.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// ============================================================
// Domain-specific exceptions
// HTTP layer maps these to status codes:
//   ValidationError → 400 Bad Request
//   NotFoundError   → 404 Not Found
//   ExpiredError    → 410 Gone
// ============================================================

class ValidationError : public std::runtime_error {
public:
    explicit ValidationError(const std::string& msg) : std::runtime_error(msg) {}
};

class NotFoundError : public std::runtime_error {
public:
    explicit NotFoundError(const std::string& msg) : std::runtime_error(msg) {}
};

class ExpiredError : public std::runtime_error {
public:
    explicit ExpiredError(const std::string& msg) : std::runtime_error(msg) {}
};

// ============================================================
// ShortenResult  –  value returned by shortenUrl()
// ============================================================
struct ShortenResult {
    std::string            short_code;
    std::string            short_url;    // full clickable URL, e.g. http://localhost:8081/aB3x7q
    std::string            original_url;
    int64_t                created_at;   // Unix seconds
    std::optional<int64_t> expires_at;
};

// ============================================================
// UrlService
// ============================================================
class UrlService {
public:
    // db       : an already-initialised Database (must outlive this object)
    // base_url : the server's public root, e.g. "http://localhost:8081"
    UrlService(Database& db, const std::string& base_url);

    // ---- Core operations ------------------------------------------------

    // Validates and persists a new shortened URL.
    // Throws ValidationError on bad input (invalid URL, expired time, bad alias,
    // duplicate alias).
    ShortenResult shortenUrl(const std::string&            original_url,
                             const std::optional<std::string>& custom_alias,
                             const std::optional<int64_t>&     expires_at);

    // Looks up the destination URL, records a click, and returns the URL.
    // Throws NotFoundError if short_code is unknown.
    // Throws ExpiredError  if the URL's expiry has passed.
    std::string getRedirectUrl(const std::string& short_code);

    // Returns all persisted URL records (newest first).
    std::vector<UrlRecord> getAllUrls();

    // Returns full analytics for a single URL.
    // Throws NotFoundError if short_code is unknown.
    UrlRecord getUrlStats(const std::string& short_code);

    // Removes a URL record permanently.
    // Throws NotFoundError if short_code is unknown.
    // Returns true when a row was deleted.
    bool deleteUrl(const std::string& short_code);

    // Accessor used by the HTTP layer when building short_url strings.
    const std::string& getBaseUrl() const { return base_url_; }

    // ---- Static helpers (also used in unit tests) ------------------------

    // Returns true when url starts with "http://" or "https://".
    static bool isValidUrl(const std::string& url);

    // Returns a random 6-character base62 string (digits + lower + upper).
    static std::string generateShortCode();

private:
    Database&   db_;
    std::string base_url_;

    // Returns the current time as a Unix timestamp (seconds since epoch).
    static int64_t nowSeconds();
};
