#include <mcp/http/HttpServer.hpp>

#include <asio/read.hpp>
#include <asio/write.hpp>
#include <asio/read_until.hpp>

#include <sstream>
#include <thread>

namespace mcp {

HttpServer::HttpServer(asio::io_context& io_ctx, uint16_t port)
    : io_ctx_(io_ctx)
    , acceptor_(io_ctx, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port))
    , port_(port)
{
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
}

HttpServer::~HttpServer() {
    Stop();
}

void HttpServer::Start() {
    running_ = true;
    DoAccept();
}

void HttpServer::Stop() {
    running_ = false;
    asio::error_code ec;
    acceptor_.close(ec);
}

void HttpServer::SetHandler(std::string_view method, std::string_view path,
                            HttpHandler handler)
{
    handlers_[{std::string(method), std::string(path)}] = std::move(handler);
}

// ── Accept connections ──
void HttpServer::DoAccept() {
    if (!running_) return;

    auto socket = std::make_shared<asio::ip::tcp::socket>(io_ctx_);
    acceptor_.async_accept(*socket, [this, socket](asio::error_code ec) {
        if (!ec) {
            HandleConnection(socket);
        }
        DoAccept();
    });
}

// ── Parse HTTP request ──
bool HttpServer::ParseRequest(
    std::shared_ptr<asio::ip::tcp::socket> socket, HttpRequest& req)
{
    try {
        asio::error_code ec;
        std::string buf;
        asio::read_until(*socket, asio::dynamic_buffer(buf), "\r\n\r\n", ec);
        if (ec) return false;

        auto& header_data = buf;

        // Parse request line: METHOD PATH HTTP/1.1
        auto first_line = header_data.substr(0, header_data.find("\r\n"));
        auto method_end = first_line.find(' ');
        auto path_end = first_line.find(' ', method_end + 1);
        req.method = first_line.substr(0, method_end);
        req.path = first_line.substr(method_end + 1, path_end - method_end - 1);

        // Parse headers
        auto header_start = header_data.find("\r\n") + 2;
        auto header_end = header_data.find("\r\n\r\n");
        auto header_section = header_data.substr(
            header_start, header_end - header_start);

        std::istringstream header_stream(header_section);
        std::string line;
        while (std::getline(header_stream, line)) {
            if (line.empty() || line == "\r") continue;
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            auto key = line.substr(0, colon);
            auto val = line.substr(colon + 1);
            // Trim whitespace
            val.erase(0, val.find_first_not_of(" \t\r\n"));
            val.erase(val.find_last_not_of(" \t\r\n") + 1);
            // Normalize header keys to lowercase
            for (auto& c : key) c = std::tolower(c);
            req.headers[std::move(key)] = std::move(val);
        }

        // Read body
        auto content_length_it = req.headers.find("content-length");
        if (content_length_it != req.headers.end()) {
            size_t body_size = std::stoul(content_length_it->second);
            auto header_end_pos = header_data.find("\r\n\r\n") + 4;
            size_t body_already_read = header_data.size() - header_end_pos;
            if (body_already_read >= body_size) {
                req.body = header_data.substr(header_end_pos, body_size);
            } else {
                req.body.resize(body_size);
                std::copy(header_data.begin() + header_end_pos,
                          header_data.end(), req.body.begin());
                size_t remaining = body_size - body_already_read;
                asio::read(*socket,
                    asio::buffer(req.body.data() + body_already_read, remaining),
                    asio::transfer_exactly(remaining), ec);
                if (ec) return false;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

// ── Send HTTP response ──
void HttpServer::SendResponse(
    std::shared_ptr<asio::ip::tcp::socket> socket,
    const HttpResponse& resp)
{
    std::string response;
    response += "HTTP/1.1 " + std::to_string(resp.status_code) + " " +
                resp.status_text + "\r\n";

    for (const auto& [key, val] : resp.headers) {
        response += key + ": " + val + "\r\n";
    }

    if (!resp.is_sse) {
        response += "content-length: " + std::to_string(resp.body.size()) + "\r\n";
        response += "connection: close\r\n";
        response += "\r\n";
        response += resp.body;
        asio::write(*socket, asio::buffer(response));
        socket->close();
    } else {
        // SSE mode: keep connection open
        response += "content-type: text/event-stream\r\n";
        response += "cache-control: no-cache\r\n";
        response += "connection: keep-alive\r\n";
        response += "access-control-allow-origin: *\r\n";
        response += "\r\n";

        // Write headers
        asio::write(*socket, asio::buffer(response));

        // Register SSE client for future events
        auto sse_id = std::make_shared<SseClientId>();
        *sse_id = AddSseClient([this, socket, sse_id](std::string_view event) {
            try {
                asio::write(*socket, asio::buffer(std::string(event)));
            } catch (...) {
                RemoveSseClient(*sse_id);
            }
        });
    }
}

// ── Handle a connection ──
void HttpServer::HandleConnection(
    std::shared_ptr<asio::ip::tcp::socket> socket)
{
    HttpRequest req;
    if (!ParseRequest(socket, req)) {
        return;
    }

    // Find handler
    auto it = handlers_.find({req.method, req.path});
    if (it == handlers_.end()) {
        HttpResponse resp;
        resp.status_code = 404;
        resp.status_text = "Not Found";
        resp.body = R"({"error":"not found"})";
        resp.headers["content-type"] = "application/json";
        SendResponse(socket, resp);
        return;
    }

    HttpResponse resp;
    it->second(req, resp);
    SendResponse(socket, resp);
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
