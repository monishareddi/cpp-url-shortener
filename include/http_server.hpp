#pragma once
/*
 * http_server.hpp  –  HTTP routing and response layer
 *
 * HttpServer wraps cpp-httplib and wires each route to a handler method.
 * All business logic is delegated to UrlService; this class only handles:
 *   • Parsing JSON request bodies
 *   • Calling the appropriate UrlService method
 *   • Translating exceptions to HTTP status codes + JSON error bodies
 *   • Serialising UrlRecord / ShortenResult structs to JSON
 *
 * Routes registered:
 *   POST   /api/shorten                  → handleShorten      (201 / 400)
 *   GET    /api/urls                     → handleGetAllUrls   (200)
 *   GET    /api/urls/:code               → handleGetUrlStats  (200 / 404)
 *   DELETE /api/urls/:code               → handleDeleteUrl    (200 / 404)
 *   GET    /:code                        → handleRedirect     (302 / 404 / 410)
 *
 * httplib.h is included here so that httplib::Request / httplib::Response
 * are visible in the private method signatures.
 */

// httplib is a single-header library located in third_party/ (added to
// the compiler's include search path by CMake).
#include "httplib.h"
#include "url_service.hpp"

#include <memory>
#include <string>

class HttpServer {
public:
    // url_service : the business logic layer (must outlive HttpServer)
    // port        : TCP port to bind (8081 by default)
    HttpServer(UrlService& url_service, int port);

    // Stops the server and releases the httplib::Server object.
    ~HttpServer();

    // Starts accepting connections.  Blocks until stop() is called.
    void start();

    // Signals the internal server loop to stop (safe to call from a
    // signal handler via a global pointer set in main()).
    void stop();

private:
    UrlService&                      url_service_;
    int                              port_;
    std::unique_ptr<httplib::Server> server_;

    // Registers all route callbacks on server_.
    void setupRoutes();

    // ---- Route handlers -------------------------------------------------
    // Each handler corresponds to one API endpoint.

    // POST /api/shorten
    void handleShorten    (const httplib::Request& req, httplib::Response& res);

    // GET  /:short_code   →  302 redirect
    void handleRedirect   (const httplib::Request& req, httplib::Response& res);

    // GET  /api/urls
    void handleGetAllUrls (const httplib::Request& req, httplib::Response& res);

    // GET  /api/urls/:short_code
    void handleGetUrlStats(const httplib::Request& req, httplib::Response& res);

    // DELETE /api/urls/:short_code
    void handleDeleteUrl  (const httplib::Request& req, httplib::Response& res);

    // ---- Helpers --------------------------------------------------------

    // Sets res.status and writes a {"error": "…", "status": N} JSON body.
    void sendError(httplib::Response& res, int status, const std::string& message);

    // Sets res.status and writes a pre-built JSON string as the response body.
    void sendJson (httplib::Response& res, int status, const std::string& json_body);
};
