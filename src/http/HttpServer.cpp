// HttpServer.cpp - HTTP server implementation (libhv PIMPL)

#include <mcp/http/HttpServer.hpp>
#include <mcp/Log.hpp>

#include <hv/HttpService.h>
#include <hv/HttpServer.h>

#include <cctype>
#include <mutex>
#include <thread>

namespace mcp {

// ── PIMPL: libhv state ──
struct HttpServerImpl {
    std::shared_ptr<hv::HttpService> service;
    std::unique_ptr<hv::HttpServer> server;

    // Copy of options so SSE disconnect callbacks outlive the HttpServer object
    HttpServerOptions options;

    // SSE clients
    std::mutex sse_mutex;
    std::unordered_map<HttpServer::SseClientId,
                       std::function<void(std::string_view)>> sse_clients;
    HttpServer::SseClientId next_sse_id{1};
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

    impl_ = std::make_shared<HttpServerImpl>();
    impl_->options = options_;
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
                for (auto& [k, v] : req->headers) {
                    std::string key_lower = k;
                    for (auto& c : key_lower)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    our_req.headers[key_lower] = v;
                }

                if (options_.on_request) {
                    try {
                        options_.on_request(our_req);
                    } catch (const std::exception& e) {
                        MCP_LOG(Warning, std::string("on_request callback threw: ") + e.what());
                    }
                }

                // Call the registered handler
                HttpResponse our_resp;
                try {
                    h(our_req, our_resp);
                } catch (const std::exception& e) {
                    MCP_LOG(Error, std::string("HTTP handler threw: ") + e.what());
                    our_resp.status_code = 500;
                    our_resp.status_text = "Internal Server Error";
                }

                writer->Begin();
                writer->WriteStatus((http_status)our_resp.status_code);

                if (our_resp.is_sse) {
                    // SSE: flush headers, write the endpoint event, keep connection alive.
                    // libhv does NOT send the response body for async handlers, so the
                    // endpoint event must be written explicitly here.
                    writer->WriteHeader("Content-Type", "text/event-stream");
                    writer->WriteHeader("Cache-Control", "no-cache");
                    writer->WriteHeader("Connection", "keep-alive");
                    for (auto& [k, v] : our_resp.headers)
                        writer->WriteHeader(k.c_str(), v.c_str());
                    writer->EndHeaders();
                    if (!our_resp.body.empty())
                        writer->write(our_resp.body.data(), (int)our_resp.body.size());

                    // Register SSE client
                    auto id = AddSseClient([w = writer](std::string_view data) mutable {
                        w->write(data.data(), (int)data.size());
                    });

                    // Cleanup on disconnect. Capture the impl shared_ptr (not `this`)
                    // so the callback stays valid even if the server is stopped.
                    auto impl = impl_;
                    writer->onclose = [impl, id]() {
                        HttpDisconnectCallback on_disconnect;
                        {
                            std::lock_guard<std::mutex> lock(impl->sse_mutex);
                            impl->sse_clients.erase(id);
                            on_disconnect = std::move(impl->options.on_disconnect);
                        }
                        if (on_disconnect) {
                            try {
                                on_disconnect();
                            } catch (const std::exception& e) {
                                MCP_LOG(Warning, std::string("on_disconnect callback threw: ") + e.what());
                            }
                        }
                    };

                    if (impl->options.on_connect) {
                        try {
                            impl->options.on_connect();
                        } catch (const std::exception& e) {
                            MCP_LOG(Warning, std::string("on_connect callback threw: ") + e.what());
                        }
                    }

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
            // stop() joins all event-loop threads; SSE onclose callbacks run
            // synchronously inside, safely before impl_ is released.
            impl_->server->stop();
            impl_->server.reset();
        }
        impl_->service.reset();
        impl_.reset();
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
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->sse_mutex);
    auto id = impl_->next_sse_id++;
    impl_->sse_clients[id] = std::move(send_fn);
    return id;
}

void HttpServer::RemoveSseClient(SseClientId id) {
    if (!impl_) return;
    HttpDisconnectCallback on_disconnect;
    {
        std::lock_guard<std::mutex> lock(impl_->sse_mutex);
        impl_->sse_clients.erase(id);
        on_disconnect = std::move(impl_->options.on_disconnect);
    }
    if (on_disconnect) {
        try {
            on_disconnect();
        } catch (const std::exception& e) {
            MCP_LOG(Warning, std::string("on_disconnect callback threw: ") + e.what());
        }
    }
}

void HttpServer::BroadcastSse(std::string_view event) {
    if (!impl_) return;
    std::vector<std::function<void(std::string_view)>> fns;
    {
        std::lock_guard<std::mutex> lock(impl_->sse_mutex);
        fns.reserve(impl_->sse_clients.size());
        for (auto& [id, fn] : impl_->sse_clients)
            fns.push_back(fn);
    }
    for (auto& fn : fns) {
        try {
            fn(event);
        } catch (const std::exception& e) {
            MCP_LOG(Warning, std::string("SSE send failed: ") + e.what());
        }
    }
}

} // namespace mcp
