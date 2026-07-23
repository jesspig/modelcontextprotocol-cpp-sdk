#pragma once

#include <mcp/JsonRpc.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp {

// ── HTTP request / response ──
struct HttpRequest {
    std::string method;      // GET, POST
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status_code{200};
    std::string status_text{"OK"};
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool is_sse{false};  // if true, body is ignored and SSE stream is used
};

// ── HTTP handler callback ──
using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

// ── HttpServer options — event callbacks ──
using HttpRequestCallback = std::function<void(const HttpRequest&)>;
using HttpConnectCallback = std::function<void()>;
using HttpDisconnectCallback = std::function<void()>;

struct HttpServerOptions {
    HttpRequestCallback on_request;
    HttpConnectCallback on_connect;
    HttpDisconnectCallback on_disconnect;
};

// ── HttpServer — minimal HTTP server ──
// Handles GET and POST. Supports SSE streaming via callback.
struct HttpServerImpl;
class HttpServer {
public:
    HttpServer(uint16_t port,
               const HttpServerOptions& options = {});
    ~HttpServer();

    // Start accepting connections
    void Start();

    // Stop the server
    void Stop();

    // Set handler for a specific path + method
    void SetHandler(std::string_view method, std::string_view path,
                    HttpHandler handler);

    // Send SSE event to a connected SSE client
    // (for server-initiated notifications)
    using SseClientId = uint64_t;
    SseClientId AddSseClient(std::function<void(std::string_view)> send_fn);
    void RemoveSseClient(SseClientId id);
    void BroadcastSse(std::string_view event);

private:
    uint16_t port_;
    bool running_{false};
    HttpServerOptions options_;

    // Handlers: (method, path) → handler
    std::map<std::pair<std::string, std::string>, HttpHandler> handlers_;

    // SSE clients
    std::unordered_map<SseClientId, std::function<void(std::string_view)>> sse_clients_;
    SseClientId next_sse_id_{1};
    std::mutex sse_mutex_;

    std::unique_ptr<HttpServerImpl> impl_;
};

} // namespace mcp
