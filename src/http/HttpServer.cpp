#include <mcp/http/HttpServer.hpp>

#include <hv/HttpService.h>
#include <hv/HttpServer.h>

#include <sstream>
#include <thread>

namespace mcp {

// ── PIMPL: libhv state ──
struct HttpServerImpl {
    std::shared_ptr<hv::HttpService> service;
    std::unique_ptr<hv::HttpServer> server;
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
    if (running_) return;
    running_ = true;

    impl_ = std::make_unique<HttpServerImpl>();
    impl_->service = std::make_shared<hv::HttpService>();
    impl_->service->AllowCORS();
    impl_->server = std::make_unique<hv::HttpServer>(impl_->service.get());
    impl_->server->setPort(port_);
    impl_->server->setThreadNum(2);

    // Register all stored handlers on the HttpService
    for (auto& [key, handler] : handlers_) {
        const auto& method_name = key.first;
        const auto& path = key.second;

        http_handler hv_h;
        hv_h.async_handler =
            [this, h = std::move(handler)](
                const HttpRequestPtr& req,
                const HttpResponseWriterPtr& writer) {
                // Convert hv::HttpRequest -> our HttpRequest
                HttpRequest our_req;
                our_req.method = req->Method();
                our_req.path = req->path;
                our_req.body = req->body;
                for (auto& [k, v] : req->headers)
                    our_req.headers[k] = v;

                if (options_.on_request)
                    options_.on_request(our_req);

                // Call the registered handler
                HttpResponse our_resp;
                h(our_req, our_resp);

                writer->Begin();
                writer->WriteStatus((http_status)our_resp.status_code);

                if (our_resp.is_sse) {
                    // SSE: flush headers, keep connection alive
                    writer->WriteHeader("Content-Type", "text/event-stream");
                    writer->WriteHeader("Cache-Control", "no-cache");
                    writer->WriteHeader("Connection", "keep-alive");
                    for (auto& [k, v] : our_resp.headers)
                        writer->WriteHeader(k.c_str(), v.c_str());
                    writer->EndHeaders();

                    // Register SSE client
                    auto id = next_sse_id_++;
                    {
                        std::lock_guard<std::mutex> lock(sse_mutex_);
                        auto w = writer;
                        sse_clients_[id] = [w](std::string_view data) mutable {
                            w->write(data.data(), (int)data.size());
                        };
                    }

                    // Cleanup on disconnect
                    writer->onclose = [this, id]() {
                        RemoveSseClient(id);
                    };

                    // NOT calling End() - connection stays open for SSE
                } else {
                    for (auto& [k, v] : our_resp.headers)
                        writer->WriteHeader(k.c_str(), v.c_str());
                    writer->response->body = our_resp.body;
                    writer->End();
                }
            };

        impl_->service->AddRoute(
            path.c_str(),
            http_method_enum(method_name.c_str()),
            hv_h);
    }

    // Start (non-blocking)
    impl_->server->start();
}

void HttpServer::Stop() {
    if (!running_) return;
    running_ = false;
    if (impl_) {
        if (impl_->server) {
            impl_->server->stop();
            impl_->server.reset();
        }
        impl_->service.reset();
        impl_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        sse_clients_.clear();
    }
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
