// HttpServer.cpp - HTTP server implementation (libhv PIMPL)

#include <mcp/http/HttpServer.hpp>
#include <mcp/Log.hpp>

#include <hv/HttpService.h>
#include <hv/HttpServer.h>

#include <cctype>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

constexpr int kHttpWorkerThreads = 8;

} // namespace

namespace mcp {

// ── PIMPL: libhv state ──
struct HttpServerImpl {
    std::shared_ptr<hv::HttpService> service;
    std::unique_ptr<hv::HttpServer> server;

    // Copy of options so SSE disconnect callbacks outlive the HttpServer object
    HttpServerOptions options;

    // SSE clients
    struct SseClientEntry {
        std::mutex write_mutex;
        std::function<void(std::string_view)> send_fn;
    };
    std::mutex sse_mutex;
    std::unordered_map<HttpServer::SseClientId, std::shared_ptr<SseClientEntry>> sse_clients;
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

    auto impl = std::make_shared<HttpServerImpl>();
    impl->options = options_;
    impl->service = std::make_shared<hv::HttpService>();
    impl->service->AllowCORS();
    impl->server = std::make_unique<hv::HttpServer>(impl->service.get());
    impl->server->setPort(port_);
    impl->server->setThreadNum(kHttpWorkerThreads);

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

                    // Register SSE client. The connection may already be gone
                    // (closed between request dispatch and this handler): skip
                    // registration entirely in that case. A write failure later
                    // also removes the client (see BroadcastSse).
                    if (writer->isClosed()) {
                        return;
                    }
                    auto id = AddSseClient([w = writer](std::string_view data) mutable {
                        if (w->write(data.data(), (int)data.size()) < 0) {
                            throw std::runtime_error("SSE write failed");
                        }
                    });

                    // Cleanup on disconnect. Capture the impl shared_ptr (not `this`)
                    // so the callback stays valid even if the server is stopped.
                    auto impl = std::atomic_load(&impl_);
                    writer->onclose = [impl, id]() {
                        HttpDisconnectCallback on_disconnect;
                        {
                            std::lock_guard<std::mutex> lock(impl->sse_mutex);
                            impl->sse_clients.erase(id);
                            on_disconnect = impl->options.on_disconnect;
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

        impl->service->AddRoute(
            path.c_str(),
            http_method_enum(method_name.c_str()),
            hv_h);
    }

    std::atomic_store(&impl_, impl);

    // Start (non-blocking)
    impl->server->start();
}

void HttpServer::Stop() {
    if (!running_) return;
    running_ = false;
    auto impl = std::atomic_load(&impl_);
    if (impl) {
        if (impl->server) {
            // stop() joins all event-loop threads; SSE onclose callbacks run
            // synchronously inside, safely before impl_ is released.
            impl->server->stop();
            impl->server.reset();
        }
        impl->service.reset();
    }
    std::atomic_store(&impl_, std::shared_ptr<HttpServerImpl>());
}

void HttpServer::SetHandler(std::string_view method, std::string_view path,
                            HttpHandler handler)
{
    if (running_) {
        throw std::logic_error("HttpServer: SetHandler called after Start()");
    }
    handlers_[{std::string(method), std::string(path)}] = std::move(handler);
}

// ── SSE client management ──
HttpServer::SseClientId HttpServer::AddSseClient(
    std::function<void(std::string_view)> send_fn)
{
    auto impl = std::atomic_load(&impl_);
    if (!impl) return 0;
    auto entry = std::make_shared<HttpServerImpl::SseClientEntry>();
    entry->send_fn = std::move(send_fn);
    std::lock_guard<std::mutex> lock(impl->sse_mutex);
    auto id = impl->next_sse_id++;
    impl->sse_clients[id] = std::move(entry);
    return id;
}

void HttpServer::RemoveSseClient(SseClientId id) {
    auto impl = std::atomic_load(&impl_);
    if (!impl) return;
    HttpDisconnectCallback on_disconnect;
    {
        std::lock_guard<std::mutex> lock(impl->sse_mutex);
        impl->sse_clients.erase(id);
        on_disconnect = impl->options.on_disconnect;
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
    auto impl = std::atomic_load(&impl_);
    if (!impl) return;
    std::vector<std::shared_ptr<HttpServerImpl::SseClientEntry>> entries;
    {
        std::lock_guard<std::mutex> lock(impl->sse_mutex);
        entries.reserve(impl->sse_clients.size());
        for (auto& [id, entry] : impl->sse_clients)
            entries.push_back(entry);
    }
    for (auto& entry : entries) {
        try {
            std::lock_guard<std::mutex> write_lock(entry->write_mutex);
            entry->send_fn(event);
        } catch (const std::exception& e) {
            MCP_LOG(Warning, std::string("SSE send failed: ") + e.what());
            std::lock_guard<std::mutex> lock(impl->sse_mutex);
            for (auto it = impl->sse_clients.begin(); it != impl->sse_clients.end(); ++it) {
                if (it->second == entry) {
                    impl->sse_clients.erase(it);
                    break;
                }
            }
        }
    }
}

} // namespace mcp
