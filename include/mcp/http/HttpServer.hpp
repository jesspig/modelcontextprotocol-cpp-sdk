#pragma once

#include <mcp/JsonRpc.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp {

struct HttpRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status_code{200};
    std::string status_text{"OK"};
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool is_sse{false};
    bool sse_close_after_write{false};
};

using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

using HttpRequestCallback = std::function<void(const HttpRequest&)>;
using HttpConnectCallback = std::function<void()>;
using HttpDisconnectCallback = std::function<void()>;

struct HttpServerOptions {
    HttpRequestCallback on_request;
    HttpConnectCallback on_connect;
    HttpDisconnectCallback on_disconnect;

    std::vector<std::string> allowed_hosts;
    std::vector<std::string> allowed_origins;

    int sse_keep_alive_ms{0};

    std::string bind_host;
};

struct HttpServerImpl;
class HttpServer {
public:
    HttpServer(uint16_t port,
               const HttpServerOptions& options = {});
    ~HttpServer();

    void Start();

    void Stop();

    void SetHandler(std::string_view method, std::string_view path,
                    HttpHandler handler);

    using SseClientId = uint64_t;
    SseClientId AddSseClient(std::function<void(std::string_view)> send_fn);
    void RemoveSseClient(SseClientId id);
    void BroadcastSse(std::string_view event);
    std::size_t SseClientCount() const;

private:
    uint16_t port_;
    std::atomic<bool> running_{false};
    HttpServerOptions options_;

    std::map<std::pair<std::string, std::string>, HttpHandler> handlers_;

    std::shared_ptr<HttpServerImpl> impl_;
};

} // namespace mcp
