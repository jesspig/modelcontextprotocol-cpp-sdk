// HttpServerImpl.hpp — 自研 HTTP/1.1 服务器实现（替换 libhv）

#pragma once

#include <mcp/http/HttpServer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mcp { namespace detail { namespace net {
class TcpSocket;
}}} // namespace mcp::detail::net

namespace mcp { namespace detail { namespace http_server_impl {

struct SseClientEntry {
    std::mutex write_mutex;
    std::function<void(std::string_view)> send_fn;
};

struct ConnEntry {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
};

class Impl {
public:
    Impl() = default;
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void Start(uint16_t port,
               const std::map<std::pair<std::string, std::string>, HttpHandler>& handlers,
               const HttpServerOptions& options);
    void Stop();

    uint64_t AddSseClient(std::function<void(std::string_view)> send_fn);
    bool RemoveSseClient(uint64_t id, bool call_on_disconnect);
    void BroadcastSse(std::string_view event);

private:
    using HandlerMap = std::map<std::pair<std::string, std::string>, HttpHandler>;

    enum class LineResult { Ok, Timeout, Eof, TooLong };
    enum class RequestLineResult { Ok, Close, BadRequest };
    enum class HeaderResult { Ok, Close, BadRequest, PayloadTooLarge };

    void AcceptLoop(uint16_t port, HandlerMap handlers);
    void HandleConnection(int fd, HandlerMap handlers,
                          std::shared_ptr<std::atomic<bool>> done);
    void HandleConnectionInner(int fd, HandlerMap handlers);

    LineResult ReadLine(net::TcpSocket& conn, std::string& buffer, std::string& line,
                        std::chrono::milliseconds timeout, std::size_t max_line_bytes);
    RequestLineResult ReadRequestLine(net::TcpSocket& conn, std::string& buffer,
                                      std::string& method, std::string& path,
                                      std::string& version,
                                      std::chrono::milliseconds timeout);
    HeaderResult ReadHeaderBlock(net::TcpSocket& conn, std::string& buffer,
                                 std::unordered_map<std::string, std::string>& headers,
                                 std::size_t& content_length_out,
                                 std::chrono::milliseconds timeout);
    bool ReadBody(net::TcpSocket& conn, std::string& buffer, std::size_t content_length,
                  std::string& body, std::chrono::milliseconds timeout);
    void WriteResponse(net::TcpSocket& conn, int status, std::string_view status_text,
                       const std::unordered_map<std::string, std::string>& headers,
                       std::string_view body, bool keep_alive);
    void WriteSseHeaders(net::TcpSocket& conn,
                         const std::unordered_map<std::string, std::string>& headers);
    void RemoveSseClientEntry(const std::shared_ptr<SseClientEntry>& entry,
                              bool call_on_disconnect);

    static bool IsRequestAllowed(const HttpRequest& req, const HttpServerOptions& options);
    static bool IsLocalhostHost(const std::string& host_with_port);

    HttpServerOptions options_;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    std::thread accept_thread_;
    std::mutex conns_mutex_;
    std::vector<ConnEntry> conn_threads_;
    std::vector<int> conn_fds_;
    std::mutex sse_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<SseClientEntry>> sse_clients_;
    uint64_t next_sse_id_{1};
};

}}} // namespace mcp::detail::http_server_impl
