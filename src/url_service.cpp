/*
 * url_service.cpp  –  Business logic layer implementation
 *
 * Validation rules enforced here (not in the HTTP or DB layers):
 *   • URL must begin with "http://" or "https://"
 *   • expires_at (when present) must be a future timestamp
 *   • Custom alias: 1-50 chars, [A-Za-z0-9_-] only, must not already exist
 *   • Auto-generated codes: 6 random base62 chars, unique (retried up to 10x)
 */

#include "url_service.hpp"

#include <cctype>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

// ============================================================
// Constants
// ============================================================

// 62-character alphabet used for random short-code generation.
static constexpr char BASE62[] =
    "0123456789"
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static constexpr int SHORT_CODE_LENGTH  = 6;
static constexpr int MAX_ALIAS_LENGTH   = 50;
static constexpr int MAX_CODEGEN_TRIES  = 10;

// ============================================================
// UrlService  –  construction
// ============================================================

UrlService::UrlService(Database& db, const std::string& base_url)
    : db_(db), base_url_(base_url)
{}

// ============================================================
// Static helpers
// ============================================================

int64_t UrlService::nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count();
}

bool UrlService::isValidUrl(const std::string& url) {
    // Accept only "http://" or "https://" schemes.
    // rfind at position 0 efficiently checks for a prefix.
    return (url.rfind("http://",  0) == 0 ||
            url.rfind("https://", 0) == 0);
}

std::string UrlService::generateShortCode() {
    // thread_local RNG: each thread gets its own seeded engine, avoiding
    // both lock contention and the cost of seeding on every call.
    thread_local std::mt19937 rng{ std::random_device{}() };
    thread_local std::uniform_int_distribution<int> dist(0, 61); // 0..61 for BASE62

    std::string code(SHORT_CODE_LENGTH, '\0');
    for (char& c : code) {
        c = BASE62[dist(rng)];
    }
    return code;
}

// ============================================================
// shortenUrl
// ============================================================

ShortenResult UrlService::shortenUrl(const std::string&            original_url,
                                     const std::optional<std::string>& custom_alias,
                                     const std::optional<int64_t>&     expires_at) {
    // --- Validate URL scheme ---
    if (!isValidUrl(original_url)) {
        throw ValidationError(
            "Invalid URL: must begin with http:// or https://");
    }

    // --- Validate expiry (if provided, must be in the future) ---
    if (expires_at.has_value()) {
        if (expires_at.value() <= nowSeconds()) {
            throw ValidationError(
                "expires_at must be a Unix timestamp in the future");
        }
    }

    // --- Determine the short code ---
    std::string short_code;

    if (custom_alias.has_value()) {
        const std::string& alias = custom_alias.value();

        // Length check
        if (alias.empty() || alias.size() > static_cast<size_t>(MAX_ALIAS_LENGTH)) {
            throw ValidationError(
                "Custom alias must be between 1 and " +
                std::to_string(MAX_ALIAS_LENGTH) + " characters");
        }

        // Character set: letters, digits, hyphens, underscores only
        for (char c : alias) {
            if (!std::isalnum(static_cast<unsigned char>(c)) &&
                c != '-' && c != '_') {
                throw ValidationError(
                    "Custom alias may only contain letters, digits, "
                    "hyphens (-), and underscores (_)");
            }
        }

        // Uniqueness check
        if (db_.codeExists(alias)) {
            throw ValidationError(
                "Custom alias '" + alias + "' is already in use");
        }

        short_code = alias;

    } else {
        // Auto-generate a unique 6-character base62 code.
        // Collision probability is extremely low (~1 in 56 billion per attempt)
        // but we still retry a bounded number of times to be safe.
        int attempts = 0;
        do {
            short_code = generateShortCode();
            ++attempts;
        } while (db_.codeExists(short_code) && attempts < MAX_CODEGEN_TRIES);

        if (attempts >= MAX_CODEGEN_TRIES) {
            throw std::runtime_error(
                "Could not generate a unique short code after " +
                std::to_string(MAX_CODEGEN_TRIES) + " attempts");
        }
    }

    // --- Persist the record ---
    UrlRecord record = db_.insertUrl(short_code, original_url, expires_at);

    return ShortenResult{
        record.short_code,
        base_url_ + "/" + record.short_code,
        record.original_url,
        record.created_at,
        record.expires_at
    };
}

// ============================================================
// getRedirectUrl  –  used by the redirect endpoint
// ============================================================

std::string UrlService::getRedirectUrl(const std::string& short_code) {
    auto opt = db_.getUrlByCode(short_code);

    if (!opt.has_value()) {
        throw NotFoundError("Short code '" + short_code + "' not found");
    }

    const UrlRecord& r = opt.value();

    // Check whether the link has passed its expiry timestamp.
    if (r.expires_at.has_value() && r.expires_at.value() < nowSeconds()) {
        throw ExpiredError(
            "The link '" + short_code + "' has expired");
    }

    // Record the click (best-effort: don't let a click-recording failure
    // prevent the redirect from succeeding).
    try {
        db_.recordClick(short_code, nowSeconds());
    } catch (const std::exception& e) {
        // Log but do not propagate – the redirect still works.
        // In production you might push this to an async queue instead.
        std::cerr << "[Warning] recordClick failed: " << e.what() << "\n";
    }

    return r.original_url;
}

// ============================================================
// getAllUrls
// ============================================================

std::vector<UrlRecord> UrlService::getAllUrls() {
    return db_.getAllUrls();
}

// ============================================================
// getUrlStats
// ============================================================

UrlRecord UrlService::getUrlStats(const std::string& short_code) {
    auto opt = db_.getUrlByCode(short_code);
    if (!opt.has_value()) {
        throw NotFoundError("Short code '" + short_code + "' not found");
    }
    return opt.value();
}

// ============================================================
// deleteUrl
// ============================================================

bool UrlService::deleteUrl(const std::string& short_code) {
    // Check existence first to give a clear 404 instead of a silent no-op.
    if (!db_.codeExists(short_code)) {
        throw NotFoundError("Short code '" + short_code + "' not found");
    }
    return db_.deleteUrl(short_code);
}
