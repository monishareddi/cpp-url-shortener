/*
 * http_server.cpp  –  HTTP routing and response serialisation
 *
 * Responsibility boundary:
 *   • Parse the incoming JSON request body.
 *   • Call the appropriate UrlService method.
 *   • Catch domain exceptions and map them to HTTP status codes.
 *   • Serialise UrlRecord / ShortenResult structs to JSON responses.
 *
 * JSON is handled with nlohmann/json (single-header in third_party/).
 * HTTP is handled with cpp-httplib (single-header in third_party/).
 *
 * This file is the only translation unit that includes both heavy headers,
 * keeping compile times fast for all other source files.
 */

#include "http_server.hpp"

// nlohmann/json – included only here so it doesn't slow down other TUs.
#include "nlohmann/json.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <string>

using json = nlohmann::json;

// ============================================================
// File-local helpers
// ============================================================

// Converts a Unix timestamp (seconds since epoch) to an ISO-8601 string
// in UTC, e.g. "2024-06-15T09:30:00Z".
// Uses gmtime_r (POSIX / Linux / WSL) for thread-safety.
static std::string timestampToIso8601(int64_t ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    struct tm   tm_buf{};
    gmtime_r(&t, &tm_buf);           // thread-safe; Linux/WSL guaranteed

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

// Returns true if ts is in the past relative to the current wall clock.
static bool isExpired(int64_t ts) {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    return ts < now;
}

// Serialises a UrlRecord into a JSON object suitable for API responses.
// base_url is used to reconstruct the short_url field.
static json recordToJson(const UrlRecord& r, const std::string& base_url) {
    json j;
    j["short_code"]   = r.short_code;
    j["short_url"]    = base_url + "/" + r.short_code;
    j["original_url"] = r.original_url;
    j["created_at"]   = timestampToIso8601(r.created_at);
    j["click_count"]  = r.click_count;

    if (r.expires_at.has_value()) {
        j["expires_at"] = timestampToIso8601(r.expires_at.value());
        j["is_expired"] = isExpired(r.expires_at.value());
    } else {
        j["expires_at"] = nullptr;
        j["is_expired"] = false;
    }

    if (r.last_clicked_at.has_value()) {
        j["last_clicked_at"] = timestampToIso8601(r.last_clicked_at.value());
    } else {
        j["last_clicked_at"] = nullptr;
    }

    return j;
}

// ============================================================
// HttpServer  –  construction / destruction
// ============================================================

HttpServer::HttpServer(UrlService& url_service, int port)
    : url_service_(url_service),
      port_(port),
      server_(std::make_unique<httplib::Server>())
{
    setupRoutes();
}

// Destructor is defined in the .cpp so the compiler sees the full
// httplib::Server definition when the unique_ptr deleter runs.
HttpServer::~HttpServer() = default;

// ============================================================
// start / stop
// ============================================================

void HttpServer::start() {
    std::cout << "[Server] Listening on http://0.0.0.0:" << port_ << "\n";
    std::cout << "[Server] Press Ctrl+C to stop.\n";

    // listen() blocks until stop() is called.
    if (!server_->listen("0.0.0.0", port_)) {
        throw std::runtime_error(
            "Failed to bind to port " + std::to_string(port_) +
            ". Is another process using it?");
    }
}

void HttpServer::stop() {
    server_->stop();
}

// ============================================================
// Response helpers
// ============================================================

void HttpServer::sendError(httplib::Response& res,
                           int status,
                           const std::string& message) {
    json body = {{"error", message}, {"status", status}};
    res.status = status;
    res.set_content(body.dump(2), "application/json");
}

void HttpServer::sendJson(httplib::Response& res,
                          int status,
                          const std::string& json_body) {
    res.status = status;
    res.set_content(json_body, "application/json");
}

// ============================================================
// setupRoutes
// ============================================================

void HttpServer::setupRoutes() {
    // Add CORS / content-type headers to every response for convenience
    // when testing with a browser-based tool during development.
    server_->set_default_headers({
        {"Access-Control-Allow-Origin",  "*"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"}
    });

    // Pre-flight CORS for browsers
    server_->Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // ----------------------------------------------------------
    // POST /api/shorten   – create a shortened URL
    // ----------------------------------------------------------
    server_->Post("/api/shorten", [this](const httplib::Request& req,
                                         httplib::Response& res) {
        handleShorten(req, res);
    });

    // ----------------------------------------------------------
    // GET /api/urls   – list all URLs (exact path, registered first)
    // ----------------------------------------------------------
    server_->Get("/api/urls", [this](const httplib::Request& req,
                                      httplib::Response& res) {
        handleGetAllUrls(req, res);
    });

    // ----------------------------------------------------------
    // GET /api/urls/:short_code   – analytics for one URL
    // Regex allows letters, digits, hyphens, underscores.
    // ----------------------------------------------------------
    server_->Get(R"(/api/urls/([A-Za-z0-9_-]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            handleGetUrlStats(req, res);
        });

    // ----------------------------------------------------------
    // DELETE /api/urls/:short_code   – remove a URL
    // ----------------------------------------------------------
    server_->Delete(R"(/api/urls/([A-Za-z0-9_-]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            handleDeleteUrl(req, res);
        });

    // ----------------------------------------------------------
    // GET /:short_code   – redirect endpoint (registered LAST so that
    // the more-specific /api/… routes above take priority).
    // The regex does NOT allow '/', so it only matches a single segment
    // and will never accidentally match /api/urls/something.
    // ----------------------------------------------------------
    server_->Get(R"(/([A-Za-z0-9_-]+))",
        [this](const httplib::Request& req, httplib::Response& res) {
            handleRedirect(req, res);
        });
}

// ============================================================
// POST /api/shorten
// ============================================================

void HttpServer::handleShorten(const httplib::Request& req,
                                httplib::Response& res) {
    // --- Parse JSON body ---
    json body;
    try {
        body = json::parse(req.body);
    } catch (const json::exception&) {
        sendError(res, 400, "Request body is not valid JSON");
        return;
    }

    // --- Required field: original_url ---
    if (!body.contains("original_url") || !body["original_url"].is_string()) {
        sendError(res, 400, "Missing or invalid field: 'original_url' (string required)");
        return;
    }
    std::string original_url = body["original_url"].get<std::string>();

    // --- Optional field: custom_alias ---
    std::optional<std::string> custom_alias;
    if (body.contains("custom_alias") && body["custom_alias"].is_string()) {
        custom_alias = body["custom_alias"].get<std::string>();
    }

    // --- Optional field: expires_at (Unix timestamp integer) ---
    std::optional<int64_t> expires_at;
    if (body.contains("expires_at") && !body["expires_at"].is_null()) {
        if (!body["expires_at"].is_number_integer()) {
            sendError(res, 400,
                "Invalid field: 'expires_at' must be an integer Unix timestamp");
            return;
        }
        expires_at = body["expires_at"].get<int64_t>();
    }

    // --- Delegate to service ---
    try {
        ShortenResult result =
            url_service_.shortenUrl(original_url, custom_alias, expires_at);

        json response;
        response["short_code"]   = result.short_code;
        response["short_url"]    = result.short_url;
        response["original_url"] = result.original_url;
        response["created_at"]   = timestampToIso8601(result.created_at);

        if (result.expires_at.has_value()) {
            response["expires_at"] = timestampToIso8601(result.expires_at.value());
        } else {
            response["expires_at"] = nullptr;
        }

        sendJson(res, 201, response.dump(2));

    } catch (const ValidationError& e) {
        sendError(res, 400, e.what());
    } catch (const std::exception& e) {
        std::cerr << "[Error] handleShorten: " << e.what() << "\n";
        sendError(res, 500, "Internal server error");
    }
}

// ============================================================
// GET /:short_code   – 302 redirect
// ============================================================

void HttpServer::handleRedirect(const httplib::Request& req,
                                 httplib::Response& res) {
    // req.matches[1] is the first capture group from the registered regex.
    std::string short_code = req.matches[1];

    try {
        std::string target = url_service_.getRedirectUrl(short_code);

        // HTTP 302 Found – temporary redirect.
        res.status = 302;
        res.set_header("Location", target);
        // Body is optional for 302 but helpful for clients that follow redirects
        // in plain text or do not honour Location automatically.
        res.set_content("Redirecting to: " + target, "text/plain");

    } catch (const NotFoundError& e) {
        sendError(res, 404, e.what());
    } catch (const ExpiredError& e) {
        // 410 Gone is more semantically accurate than 404 for expired links.
        sendError(res, 410, e.what());
    } catch (const std::exception& e) {
        std::cerr << "[Error] handleRedirect: " << e.what() << "\n";
        sendError(res, 500, "Internal server error");
    }
}

// ============================================================
// GET /api/urls   – list all URLs
// ============================================================

void HttpServer::handleGetAllUrls(const httplib::Request& /*req*/,
                                   httplib::Response& res) {
    try {
        std::vector<UrlRecord> records = url_service_.getAllUrls();

        json arr = json::array();
        for (const auto& r : records) {
            arr.push_back(recordToJson(r, url_service_.getBaseUrl()));
        }

        sendJson(res, 200, arr.dump(2));

    } catch (const std::exception& e) {
        std::cerr << "[Error] handleGetAllUrls: " << e.what() << "\n";
        sendError(res, 500, "Internal server error");
    }
}

// ============================================================
// GET /api/urls/:short_code   – analytics for one URL
// ============================================================

void HttpServer::handleGetUrlStats(const httplib::Request& req,
                                    httplib::Response& res) {
    std::string short_code = req.matches[1];

    try {
        UrlRecord record = url_service_.getUrlStats(short_code);
        sendJson(res, 200,
                 recordToJson(record, url_service_.getBaseUrl()).dump(2));

    } catch (const NotFoundError& e) {
        sendError(res, 404, e.what());
    } catch (const std::exception& e) {
        std::cerr << "[Error] handleGetUrlStats: " << e.what() << "\n";
        sendError(res, 500, "Internal server error");
    }
}

// ============================================================
// DELETE /api/urls/:short_code
// ============================================================

void HttpServer::handleDeleteUrl(const httplib::Request& req,
                                  httplib::Response& res) {
    std::string short_code = req.matches[1];

    try {
        url_service_.deleteUrl(short_code);

        json response = {
            {"message",    "URL deleted successfully"},
            {"short_code", short_code}
        };
        sendJson(res, 200, response.dump(2));

    } catch (const NotFoundError& e) {
        sendError(res, 404, e.what());
    } catch (const std::exception& e) {
        std::cerr << "[Error] handleDeleteUrl: " << e.what() << "\n";
        sendError(res, 500, "Internal server error");
    }
}
