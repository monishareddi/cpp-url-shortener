/*
 * main.cpp  –  Entry point for the C++ URL Shortener server
 *
 * Startup sequence:
 *   1. Open (or create) the SQLite database.
 *   2. Run schema initialisation (idempotent).
 *   3. Construct UrlService with the database + base URL.
 *   4. Construct HttpServer with the service + port.
 *   5. Install SIGINT / SIGTERM handlers for graceful shutdown.
 *   6. Call server.start() – blocks until Ctrl+C.
 *
 * Configuration is hard-coded as constants below for simplicity.
 * In production you would load these from environment variables
 * or a config file.
 */

#include "database.hpp"
#include "http_server.hpp"
#include "url_service.hpp"

#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>

// ============================================================
// Configuration constants
// ============================================================

static constexpr int         SERVER_PORT = 8081;
static const     std::string DB_PATH     = "urls.db";
static const     std::string BASE_URL    = "http://localhost:8081";

// ============================================================
// Global server pointer – used only in the signal handler.
// A raw pointer is intentional here: the signal handler must not
// call delete; it just needs to invoke stop().
// ============================================================
static HttpServer* g_server = nullptr;

// ============================================================
// Signal handler  –  graceful shutdown on Ctrl+C / kill
// ============================================================
static void onSignal(int signum) {
    std::cout << "\n[Server] Caught signal " << signum
              << " – shutting down gracefully …\n";
    if (g_server) {
        g_server->stop();
    }
}

// ============================================================
// main
// ============================================================
int main() {
    std::cout << "╔══════════════════════════════════════╗\n"
              << "║      C++ URL Shortener  v1.0         ║\n"
              << "╚══════════════════════════════════════╝\n\n";

    try {
        // ---- Layer 1: Database ----
        std::cout << "[Init] Opening database: " << DB_PATH << "\n";
        Database db(DB_PATH);
        db.initialize();   // creates table + index if they don't exist

        // ---- Layer 2: Business logic ----
        std::cout << "[Init] Base URL: " << BASE_URL << "\n";
        UrlService url_service(db, BASE_URL);

        // ---- Layer 3: HTTP server ----
        HttpServer server(url_service, SERVER_PORT);
        g_server = &server;   // expose to signal handler

        // Install signal handlers AFTER the server object exists.
        std::signal(SIGINT,  onSignal);   // Ctrl+C
        std::signal(SIGTERM, onSignal);   // kill / systemd stop

        std::cout << "[Init] All layers ready.\n\n";

        // Blocks here until stop() is called from the signal handler.
        server.start();

    } catch (const std::exception& e) {
        std::cerr << "\n[FATAL] " << e.what() << "\n";
        return 1;
    }

    std::cout << "[Server] Stopped. Goodbye.\n";
    return 0;
}
