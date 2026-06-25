/*
 * test_url_service.cpp  –  Unit tests for UrlService (and indirectly Database)
 *
 * No external test framework is needed.  Each TEST() function is a plain
 * C++ function that uses assert() or throws on failure.  RUN_TEST() calls
 * it, catches exceptions, prints PASS or FAIL, and increments a counter.
 *
 * Tests use an in-memory SQLite database (":memory:") so they:
 *   • Have zero disk I/O
 *   • Are completely isolated from each other
 *   • Run in milliseconds
 *
 * Build & run:
 *   cd build && cmake --build . --target test_url_service --parallel
 *   ./bin/test_url_service
 *   # or via ctest:
 *   ctest --output-on-failure
 */

#include "database.hpp"
#include "url_service.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <chrono>

// ============================================================
// Minimal test harness
// ============================================================

static int g_pass    = 0;
static int g_fail    = 0;

#define TEST(name)  static void name()

#define RUN_TEST(name)                                          \
    do {                                                        \
        std::cout << "  " #name " … ";                         \
        try {                                                   \
            name();                                             \
            std::cout << "\033[32mPASS\033[0m\n";              \
            ++g_pass;                                           \
        } catch (const std::exception& _ex) {                  \
            std::cout << "\033[31mFAIL\033[0m  " << _ex.what() << "\n"; \
            ++g_fail;                                           \
        } catch (...) {                                         \
            std::cout << "\033[31mFAIL\033[0m  (unknown exception)\n"; \
            ++g_fail;                                           \
        }                                                       \
    } while (false)

// Helper: creates an in-memory DB and an initialised UrlService.
// Declared as a lambda-like macro so each test gets its own fresh state.
#define MAKE_SERVICE(db_var, svc_var)                           \
    Database   db_var(":memory:");                              \
    db_var.initialize();                                        \
    UrlService svc_var(db_var, "http://localhost:8081")

// Helper: returns a Unix timestamp N seconds from now.
static int64_t futureTs(int seconds = 3600) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count() + seconds;
}

// ============================================================
// Tests  –  UrlService::isValidUrl
// ============================================================

TEST(test_valid_url_accepts_https) {
    assert(UrlService::isValidUrl("https://example.com")       == true);
    assert(UrlService::isValidUrl("https://sub.domain.org/path") == true);
}

TEST(test_valid_url_accepts_http) {
    assert(UrlService::isValidUrl("http://example.com")  == true);
    assert(UrlService::isValidUrl("http://localhost")     == true);
}

TEST(test_valid_url_rejects_other_schemes) {
    assert(UrlService::isValidUrl("ftp://example.com")   == false);
    assert(UrlService::isValidUrl("mailto:a@b.com")      == false);
    assert(UrlService::isValidUrl("example.com")         == false);
    assert(UrlService::isValidUrl("")                    == false);
    assert(UrlService::isValidUrl("HTTP://example.com")  == false); // case-sensitive
}

// ============================================================
// Tests  –  UrlService::generateShortCode
// ============================================================

TEST(test_generate_short_code_length) {
    std::string code = UrlService::generateShortCode();
    if (code.length() != 6) {
        throw std::runtime_error("Expected length 6, got " +
                                  std::to_string(code.length()));
    }
}

TEST(test_generate_short_code_charset) {
    for (int i = 0; i < 100; ++i) {
        std::string code = UrlService::generateShortCode();
        for (char c : code) {
            if (!std::isalnum(static_cast<unsigned char>(c))) {
                throw std::runtime_error(
                    std::string("Non-alphanumeric char in code: ") + c);
            }
        }
    }
}

TEST(test_generate_short_code_randomness) {
    // Two successive calls should (almost certainly) differ.
    std::string a = UrlService::generateShortCode();
    std::string b = UrlService::generateShortCode();
    // There is a 1-in-62^6 ≈ 1-in-56-billion chance of collision – acceptable.
    if (a == b) {
        throw std::runtime_error("Two generated codes were identical: " + a);
    }
}

// ============================================================
// Tests  –  shortenUrl: happy paths
// ============================================================

TEST(test_shorten_returns_correct_fields) {
    MAKE_SERVICE(db, svc);

    ShortenResult r = svc.shortenUrl("https://example.com",
                                      std::nullopt, std::nullopt);

    if (r.original_url != "https://example.com")
        throw std::runtime_error("original_url mismatch");
    if (r.short_code.length() != 6)
        throw std::runtime_error("short_code should be 6 chars for auto-generated");
    if (r.short_url.find("http://localhost:8081/") == std::string::npos)
        throw std::runtime_error("short_url does not contain base URL");
    if (r.created_at <= 0)
        throw std::runtime_error("created_at should be a positive timestamp");
    if (r.expires_at.has_value())
        throw std::runtime_error("expires_at should be nullopt when not set");
}

TEST(test_shorten_with_custom_alias) {
    MAKE_SERVICE(db, svc);

    ShortenResult r = svc.shortenUrl("https://example.com",
                                      std::string("myalias"), std::nullopt);

    if (r.short_code != "myalias")
        throw std::runtime_error("Expected short_code == 'myalias'");
    if (r.short_url != "http://localhost:8081/myalias")
        throw std::runtime_error("short_url incorrect for custom alias");
}

TEST(test_shorten_with_expiry) {
    MAKE_SERVICE(db, svc);

    int64_t exp = futureTs(7200);
    ShortenResult r = svc.shortenUrl("https://example.com",
                                      std::nullopt, exp);

    if (!r.expires_at.has_value())
        throw std::runtime_error("expires_at should be set");
    if (r.expires_at.value() != exp)
        throw std::runtime_error("expires_at value mismatch");
}

TEST(test_shorten_alias_with_hyphens_and_underscores) {
    MAKE_SERVICE(db, svc);

    // Both hyphens and underscores are valid in custom aliases.
    ShortenResult r = svc.shortenUrl("https://example.com",
                                      std::string("my-cool_link"), std::nullopt);
    if (r.short_code != "my-cool_link")
        throw std::runtime_error("Alias with hyphens/underscores rejected");
}

// ============================================================
// Tests  –  shortenUrl: validation errors
// ============================================================

TEST(test_shorten_invalid_scheme_throws) {
    MAKE_SERVICE(db, svc);

    bool threw = false;
    try {
        svc.shortenUrl("ftp://example.com", std::nullopt, std::nullopt);
    } catch (const ValidationError&) {
        threw = true;
    }
    if (!threw) throw std::runtime_error("Expected ValidationError for ftp://");
}

TEST(test_shorten_duplicate_alias_throws) {
    MAKE_SERVICE(db, svc);

    svc.shortenUrl("https://first.com", std::string("dup"), std::nullopt);

    bool threw = false;
    try {
        svc.shortenUrl("https://second.com", std::string("dup"), std::nullopt);
    } catch (const ValidationError& e) {
        threw = true;
        std::string msg(e.what());
        if (msg.find("already in use") == std::string::npos)
            throw std::runtime_error("Wrong error message: " + msg);
    }
    if (!threw) throw std::runtime_error("Expected ValidationError for duplicate alias");
}

TEST(test_shorten_past_expiry_throws) {
    MAKE_SERVICE(db, svc);

    int64_t past = futureTs(-3600);  // 1 hour in the past

    bool threw = false;
    try {
        svc.shortenUrl("https://example.com", std::nullopt, past);
    } catch (const ValidationError&) {
        threw = true;
    }
    if (!threw) throw std::runtime_error("Expected ValidationError for past expiry");
}

TEST(test_shorten_alias_invalid_chars_throws) {
    MAKE_SERVICE(db, svc);

    bool threw = false;
    try {
        svc.shortenUrl("https://example.com",
                        std::string("bad alias!"), std::nullopt);
    } catch (const ValidationError&) {
        threw = true;
    }
    if (!threw) throw std::runtime_error("Expected ValidationError for alias with spaces/!");
}

TEST(test_shorten_empty_alias_throws) {
    MAKE_SERVICE(db, svc);

    bool threw = false;
    try {
        svc.shortenUrl("https://example.com",
                        std::string(""), std::nullopt);
    } catch (const ValidationError&) {
        threw = true;
    }
    if (!threw) throw std::runtime_error("Expected ValidationError for empty alias");
}

// ============================================================
// Tests  –  getRedirectUrl (redirect + click recording)
// ============================================================

TEST(test_redirect_returns_correct_url) {
    MAKE_SERVICE(db, svc);

    ShortenResult r = svc.shortenUrl("https://target.example.com",
                                      std::nullopt, std::nullopt);
    std::string target = svc.getRedirectUrl(r.short_code);

    if (target != "https://target.example.com")
        throw std::runtime_error("Redirect URL mismatch");
}

TEST(test_redirect_increments_click_count) {
    MAKE_SERVICE(db, svc);

    ShortenResult r = svc.shortenUrl("https://example.com",
                                      std::nullopt, std::nullopt);

    svc.getRedirectUrl(r.short_code);  // click 1
    svc.getRedirectUrl(r.short_code);  // click 2

    UrlRecord stats = svc.getUrlStats(r.short_code);
    if (stats.click_count != 2)
        throw std::runtime_error("Expected click_count == 2, got " +
                                  std::to_string(stats.click_count));
}

TEST(test_redirect_records_last_clicked_at) {
    MAKE_SERVICE(db, svc);

    ShortenResult r = svc.shortenUrl("https://example.com",
                                      std::nullopt, std::nullopt);

    svc.getRedirectUrl(r.short_code);

    UrlRecord stats = svc.getUrlStats(r.short_code);
    if (!stats.last_clicked_at.has_value())
        throw std::runtime_error("last_clicked_at should be set after a click");
    if (stats.last_clicked_at.value() <= 0)
        throw std::runtime_error("last_clicked_at should be a positive timestamp");
}

TEST(test_redirect_nonexistent_throws_not_found) {
    MAKE_SERVICE(db, svc);

    bool threw = false;
    try {
        svc.getRedirectUrl("XXXXXX");
    } catch (const NotFoundError&) {
        threw = true;
    }
    if (!threw) throw std::runtime_error("Expected NotFoundError");
}

TEST(test_redirect_expired_url_throws_expired) {
    MAKE_SERVICE(db, svc);

    // Insert a URL that expires 1 second from now (we test expiry via
    // a past timestamp, but the service rejects past timestamps at insert
    // time).  We therefore insert directly via the database layer and
    // test the service-layer expiry check.
    int64_t past = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count() - 1;

    db.insertUrl("expiredcode", "https://example.com", past);

    bool threw = false;
    try {
        svc.getRedirectUrl("expiredcode");
    } catch (const ExpiredError&) {
        threw = true;
    }
    if (!threw) throw std::runtime_error("Expected ExpiredError for expired URL");
}

// ============================================================
// Tests  –  getAllUrls
// ============================================================

TEST(test_get_all_urls_empty) {
    MAKE_SERVICE(db, svc);

    auto all = svc.getAllUrls();
    if (!all.empty())
        throw std::runtime_error("Expected empty list on fresh DB");
}

TEST(test_get_all_urls_returns_all) {
    MAKE_SERVICE(db, svc);

    svc.shortenUrl("https://first.com",  std::string("aa"), std::nullopt);
    svc.shortenUrl("https://second.com", std::string("bb"), std::nullopt);
    svc.shortenUrl("https://third.com",  std::string("cc"), std::nullopt);

    auto all = svc.getAllUrls();
    if (all.size() != 3)
        throw std::runtime_error("Expected 3 records, got " +
                                  std::to_string(all.size()));
}

// ============================================================
// Tests  –  getUrlStats
// ============================================================

TEST(test_get_stats_correct_fields) {
    MAKE_SERVICE(db, svc);

    svc.shortenUrl("https://example.com", std::string("mycode"), std::nullopt);
    UrlRecord r = svc.getUrlStats("mycode");

    if (r.short_code   != "mycode")           throw std::runtime_error("short_code mismatch");
    if (r.original_url != "https://example.com") throw std::runtime_error("original_url mismatch");
    if (r.click_count  != 0)                  throw std::runtime_error("click_count should start at 0");
    if (r.last_clicked_at.has_value())         throw std::runtime_error("last_clicked_at should be null initially");
}

TEST(test_get_stats_nonexistent_throws) {
    MAKE_SERVICE(db, svc);

    bool threw = false;
    try {
        svc.getUrlStats("doesnotexist");
    } catch (const NotFoundError&) {
        threw = true;
    }
    if (!threw) throw std::runtime_error("Expected NotFoundError");
}

// ============================================================
// Tests  –  deleteUrl
// ============================================================

TEST(test_delete_existing_url) {
    MAKE_SERVICE(db, svc);

    svc.shortenUrl("https://example.com", std::string("todel"), std::nullopt);
    svc.deleteUrl("todel");

    bool threw = false;
    try {
        svc.getUrlStats("todel");
    } catch (const NotFoundError&) {
        threw = true;
    }
    if (!threw) throw std::runtime_error("Record should be gone after delete");
}

TEST(test_delete_nonexistent_throws) {
    MAKE_SERVICE(db, svc);

    bool threw = false;
    try {
        svc.deleteUrl("ghost");
    } catch (const NotFoundError&) {
        threw = true;
    }
    if (!threw) throw std::runtime_error("Expected NotFoundError for missing code");
}

TEST(test_delete_does_not_affect_other_records) {
    MAKE_SERVICE(db, svc);

    svc.shortenUrl("https://keep.com",   std::string("keep"),   std::nullopt);
    svc.shortenUrl("https://remove.com", std::string("remove"), std::nullopt);

    svc.deleteUrl("remove");

    // "keep" must still be retrievable
    UrlRecord r = svc.getUrlStats("keep");
    if (r.original_url != "https://keep.com")
        throw std::runtime_error("Deleted wrong record!");

    auto all = svc.getAllUrls();
    if (all.size() != 1)
        throw std::runtime_error("Expected 1 remaining record after delete");
}

// ============================================================
// main
// ============================================================

int main() {
    std::cout << "\n╔══════════════════════════════════════╗\n"
              <<   "║   C++ URL Shortener  –  Unit Tests  ║\n"
              <<   "╚══════════════════════════════════════╝\n\n";

    // ---- isValidUrl ----
    std::cout << "── isValidUrl ──────────────────────────\n";
    RUN_TEST(test_valid_url_accepts_https);
    RUN_TEST(test_valid_url_accepts_http);
    RUN_TEST(test_valid_url_rejects_other_schemes);

    // ---- generateShortCode ----
    std::cout << "\n── generateShortCode ───────────────────\n";
    RUN_TEST(test_generate_short_code_length);
    RUN_TEST(test_generate_short_code_charset);
    RUN_TEST(test_generate_short_code_randomness);

    // ---- shortenUrl happy paths ----
    std::cout << "\n── shortenUrl (valid input) ────────────\n";
    RUN_TEST(test_shorten_returns_correct_fields);
    RUN_TEST(test_shorten_with_custom_alias);
    RUN_TEST(test_shorten_with_expiry);
    RUN_TEST(test_shorten_alias_with_hyphens_and_underscores);

    // ---- shortenUrl validation errors ----
    std::cout << "\n── shortenUrl (invalid input) ──────────\n";
    RUN_TEST(test_shorten_invalid_scheme_throws);
    RUN_TEST(test_shorten_duplicate_alias_throws);
    RUN_TEST(test_shorten_past_expiry_throws);
    RUN_TEST(test_shorten_alias_invalid_chars_throws);
    RUN_TEST(test_shorten_empty_alias_throws);

    // ---- getRedirectUrl ----
    std::cout << "\n── getRedirectUrl ──────────────────────\n";
    RUN_TEST(test_redirect_returns_correct_url);
    RUN_TEST(test_redirect_increments_click_count);
    RUN_TEST(test_redirect_records_last_clicked_at);
    RUN_TEST(test_redirect_nonexistent_throws_not_found);
    RUN_TEST(test_redirect_expired_url_throws_expired);

    // ---- getAllUrls ----
    std::cout << "\n── getAllUrls ───────────────────────────\n";
    RUN_TEST(test_get_all_urls_empty);
    RUN_TEST(test_get_all_urls_returns_all);

    // ---- getUrlStats ----
    std::cout << "\n── getUrlStats ─────────────────────────\n";
    RUN_TEST(test_get_stats_correct_fields);
    RUN_TEST(test_get_stats_nonexistent_throws);

    // ---- deleteUrl ----
    std::cout << "\n── deleteUrl ───────────────────────────\n";
    RUN_TEST(test_delete_existing_url);
    RUN_TEST(test_delete_nonexistent_throws);
    RUN_TEST(test_delete_does_not_affect_other_records);

    // ---- Summary ----
    std::cout << "\n══════════════════════════════════════════\n";
    int total = g_pass + g_fail;
    if (g_fail == 0) {
        std::cout << "  \033[32mAll " << total << " tests passed.\033[0m\n";
    } else {
        std::cout << "  \033[31m" << g_fail << " / " << total
                  << " tests FAILED.\033[0m\n";
    }
    std::cout << "══════════════════════════════════════════\n\n";

    return (g_fail == 0) ? 0 : 1;
}
