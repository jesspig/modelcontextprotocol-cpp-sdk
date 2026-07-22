#include <mcp/http/HttpServer.hpp>

// TODO(libhv): rewrite HttpServer to use libhv instead of asio
// #include <asio/read.hpp>
// #include <asio/write.hpp>
// #include <asio/read_until.hpp>

#include <sstream>
#include <thread>

namespace mcp {

HttpServer::HttpServer(uint16_t port,
                       const HttpServerOptions& options)
    : port_(port)
    , options_(options)
{
    (void)port_;
    // TODO(libhv): initialize listener
}

HttpServer::~HttpServer() {
    Stop();
}

void HttpServer::Start() {
    running_ = true;
    // TODO(libhv): start accepting connections
}

void HttpServer::Stop() {
    running_ = false;
    // TODO(libhv): close acceptor
}

void HttpServer::SetHandler(std::string_view method, std::string_view path,
                            HttpHandler handler)
{
    handlers_[{std::string(method), std::string(path)}] = std::move(handler);
}

// ── SSE client management ──
HttpServer::SseClientId HttpServer::AddSseClient(
    std::function<void(std::string_view)> send_fn)
{
    std::lock_guard<std::mutex> lock(sse_mutex_);
    auto id = next_sse_id_++;
    sse_clients_[id] = std::move(send_fn);
    return id;
}

void HttpServer::RemoveSseClient(SseClientId id) {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    sse_clients_.erase(id);
}

void HttpServer::BroadcastSse(std::string_view event) {
    std::vector<SseClientId> ids;
    std::vector<std::function<void(std::string_view)>> fns;
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        ids.reserve(sse_clients_.size());
        fns.reserve(sse_clients_.size());
        for (auto& [id, fn] : sse_clients_) {
            ids.push_back(id);
            fns.push_back(fn);
        }
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        try {
            fns[i](event);
        } catch (...) {}
    }
}

} // namespace mcp
