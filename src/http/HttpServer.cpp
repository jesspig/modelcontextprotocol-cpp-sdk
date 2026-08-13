// HttpServer.cpp — HTTP server implementation (self-hosted)

#include <mcp/http/HttpServer.hpp>
#include <mcp/detail/ThreadUtils.hpp>

#include <http/HttpServerImpl.hpp>

#include <stdexcept>
#include <thread>

namespace {

// Strip an optional ":port" suffix and validate a localhost host literal.
bool IsLocalhostHost(const std::string& host_with_port) {
    std::string h = host_with_port;
    auto colon = h.rfind(':');
    if (colon != std::string::npos) {
        if (h.front() == '[' && h.back() == ']') {
            // bare IPv6 literal
            h = h.substr(1, h.size() - 2);
        } else if (h.front() == '[') {
            // [::1]:port
            auto close = h.find(']');
            if (close != std::string::npos) h = h.substr(1, close - 1);
        } else {
            // host:port
            h = h.substr(0, colon);
        }
    }
    return h == "localhost" || h == "127.0.0.1" || h == "::1";
}

} // namespace

namespace mcp {

// HttpServer.hpp 前向声明的 PIMPL：即自研实现（公共 API 零改动）
struct HttpServerImpl : detail::http_server_impl::Impl {
    using detail::http_server_impl::Impl::Impl;
};

HttpServer::HttpServer(uint16_t port,
                       const HttpServerOptions& options)
    : port_(port)
    , options_(options)
{
}

HttpServer::~HttpServer() {
    Stop();
}

void HttpServer::Start() {
    if (running_.exchange(true)) return;

    auto impl = std::make_shared<HttpServerImpl>();
    try {
        impl->Start(port_, handlers_, options_);
    } catch (...) {
        running_.store(false);
        throw;
    }
    std::atomic_store(&impl_, impl);
}

void HttpServer::Stop() {
    if (!running_.exchange(false)) return;
    auto impl = std::atomic_load(&impl_);
    if (impl) {
        // Stopper thread so Stop() may be called from a connection/callback
        // thread without self-joining.
        std::thread stopper([impl]() {
            impl->Stop();
        });
        detail::JoinThreadSafely(stopper);
    }
    std::atomic_store(&impl_, std::shared_ptr<HttpServerImpl>());
}

bool HttpServer::IsRequestAllowed(const HttpRequest& req) const {
    auto host_it = req.headers.find("host");
    if (host_it == req.headers.end()) return false;
    bool host_ok = false;
    if (!options_.allowed_hosts.empty()) {
        for (const auto& allowed : options_.allowed_hosts) {
            if (host_it->second == allowed) { host_ok = true; break; }
        }
    } else {
        host_ok = IsLocalhostHost(host_it->second);
    }
    if (!host_ok) return false;

    auto origin_it = req.headers.find("origin");
    if (origin_it == req.headers.end() || options_.allowed_origins.empty())
        return true;
    for (const auto& allowed : options_.allowed_origins) {
        if (origin_it->second == allowed) return true;
    }
    return false;
}

void HttpServer::SetHandler(std::string_view method, std::string_view path,
                            HttpHandler handler)
{
    if (running_.load()) {
        throw std::logic_error("HttpServer: SetHandler called after Start()");
    }
    handlers_[{std::string(method), std::string(path)}] = std::move(handler);
}

// ── SSE client management ──
HttpServer::SseClientId HttpServer::AddSseClient(
    std::function<void(std::string_view)> send_fn)
{
    auto impl = std::atomic_load(&impl_);
    return impl ? impl->AddSseClient(std::move(send_fn)) : 0;
}

void HttpServer::RemoveSseClient(SseClientId id) {
    auto impl = std::atomic_load(&impl_);
    if (impl) impl->RemoveSseClient(id, true);
}

void HttpServer::BroadcastSse(std::string_view event) {
    auto impl = std::atomic_load(&impl_);
    if (impl) impl->BroadcastSse(event);
}

} // namespace mcp
