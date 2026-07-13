#include <mcp/transport/StreamableHttpServerTransport.hpp>

#include <asio/post.hpp>

#include <sstream>

namespace mcp {

StreamableHttpServerTransport::StreamableHttpServerTransport(
    asio::io_context& io_ctx,
    StreamableHttpServerOptions options)
    : io_ctx_(io_ctx)
    , options_(std::move(options))
    , http_server_(std::make_unique<HttpServer>(io_ctx_, options_.port))
    , event_store_(options_.event_store
        ? options_.event_store
        : std::make_shared<EventStore>())
{
    session_id_ = "srv-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());

    // Wire HTTP handlers
    http_server_->SetHandler("POST", options_.endpoint,
        [this](const HttpRequest& req, HttpResponse& resp) {
            HandlePost(req, resp);
        });

    if (options_.enable_legacy_sse) {
        http_server_->SetHandler("GET", options_.endpoint,
            [this](const HttpRequest& req, HttpResponse& resp) {
                HandleGet(req, resp);
            });
    }
}

StreamableHttpServerTransport::~StreamableHttpServerTransport() {
    Close();
}

void StreamableHttpServerTransport::Start() {
    if (http_server_) http_server_->Start();
}

void StreamableHttpServerTransport::Close() {
    if (http_server_) http_server_->Stop();
}

// ── Handle POST ──
void StreamableHttpServerTransport::HandlePost(
    const HttpRequest& req, HttpResponse& resp)
{
    // Extract MCP headers
    auto proto_ver = GetMcpHeader(req, "mcp-protocol-version");

    // Parse JSON-RPC message from body
    JsonRpcMessage msg;
    try {
        auto body_json = nlohmann::json::parse(req.body);
        msg = body_json.get<JsonRpcMessage>();
    } catch (...) {
        resp.status_code = 400;
        resp.status_text = "Bad Request";
        resp.body = R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error"}})";
        resp.headers["content-type"] = "application/json";
        return;
    }

    // Check if this is a request (needs response) or notification (no response)
    bool needs_response = IsRequest(msg);

    if (needs_response) {
        // Send message to protocol and wait for response via channel
        auto* channel = &GetMessageChannel();
        if (channel && channel->is_open()) {
            // Write message to channel for protocol processing
            channel->async_send(
                asio::error_code{}, std::move(msg),
                [this, &resp, &req](asio::error_code ec) {
                    if (ec) return;

                    // For now, respond with a placeholder
                    // The actual response will come through the proper protocol path
                    resp.status_code = 202;
                    resp.status_text = "Accepted";
                    resp.body = R"({"jsonrpc":"2.0","result":{"resultType":"complete"}})";
                    resp.headers["content-type"] = "application/json";
                    resp.headers["mcp-protocol-version"] =
                        GetMcpHeader(req, "mcp-protocol-version");
                });
        }
    } else {
        // Notification: fire-and-forget
        auto* channel = &GetMessageChannel();
        if (channel && channel->is_open()) {
            channel->async_send(asio::error_code{}, std::move(msg),
                [](asio::error_code) {});
        }
        resp.status_code = 202;
        resp.status_text = "Accepted";
        resp.body = "{}";
        resp.headers["content-type"] = "application/json";
    }
}

// ── Handle GET (SSE stream) ──
void StreamableHttpServerTransport::HandleGet(
    const HttpRequest& req, HttpResponse& resp)
{
    // SSE stream response
    resp.is_sse = true;
    resp.headers["content-type"] = "text/event-stream";
    resp.headers["cache-control"] = "no-cache";

    // Send initial endpoint event
    std::string endpoint_event = "event: endpoint\ndata: " +
        options_.endpoint + "\n\n";
    (void)endpoint_event; // Will be sent via SSE client mechanism
}

// ── Send message (server-initiated notification via SSE) ──
void StreamableHttpServerTransport::SendMessageAsync(JsonRpcMessage message) {
    // Store event
    auto event_data = BuildSseEvent(message);
    if (!options_.stateless) {
        event_store_->Append(session_id_, event_data);
    }

    // Broadcast to SSE clients
    if (http_server_) {
        http_server_->BroadcastSse(event_data);
    }
}

// ── Build SSE event ──
std::string StreamableHttpServerTransport::BuildSseEvent(
    const JsonRpcMessage& msg)
{
    nlohmann::json j = msg;
    std::string data = "event: message\ndata: " + j.dump() + "\n\n";
    return data;
}

// ── Channel access ──
Transport::MessageChannel& StreamableHttpServerTransport::GetMessageChannel() {
    if (!channel_) {
        channel_ = std::make_unique<MessageChannel>(io_ctx_, 64);
    }
    return *channel_;
}

asio::io_context& StreamableHttpServerTransport::IoContext() {
    return io_ctx_;
}

// ── Header helper ──
std::string StreamableHttpServerTransport::GetMcpHeader(
    const HttpRequest& req, std::string_view header_name) const
{
    auto key = std::string(header_name);
    for (auto& c : key) c = std::tolower(c);
    auto it = req.headers.find(key);
    if (it != req.headers.end()) return it->second;
    return {};
}

} // namespace mcp
