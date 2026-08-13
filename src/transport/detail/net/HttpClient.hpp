#pragma once

// HttpClient.hpp — blocking HTTP/1.1 client over TcpSocket/TlsSocket

#include <transport/detail/net/TcpSocket.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mcp { namespace detail { namespace net {

struct HttpRequestSpec {
    std::string method;
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::chrono::milliseconds timeout{std::chrono::milliseconds(30000)};
    bool verify_tls{true};
};

struct HttpResponseInfo {
    int status_code{0};
    std::string status_text;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

class TlsSocket;

class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponseInfo Request(const HttpRequestSpec& req,
                             const std::function<void(std::string_view chunk)>& body_cb = {});
    void Close();

private:
    void CloseConnection();
    bool IsEof() const;
    void WriteAll(std::string_view data, const std::chrono::steady_clock::time_point& deadline);
    int ReadByte(const std::chrono::steady_clock::time_point& deadline);
    std::string ReadLine(const std::chrono::steady_clock::time_point& deadline);
    std::size_t ReadRaw(void* buf, std::size_t len, const std::chrono::steady_clock::time_point& deadline);
    void ReadFixedBody(HttpResponseInfo& resp, std::size_t length,
                       const std::function<void(std::string_view)>& body_cb,
                       const std::chrono::steady_clock::time_point& deadline);
    void ReadChunkedBody(HttpResponseInfo& resp,
                         const std::function<void(std::string_view)>& body_cb,
                         const std::chrono::steady_clock::time_point& deadline);
    void ReadUntilEof(HttpResponseInfo& resp, const std::chrono::steady_clock::time_point& deadline);
    void StreamBody(const std::function<void(std::string_view)>& body_cb,
                    std::chrono::milliseconds idle_timeout);

    std::unique_ptr<TcpSocket> tcp_;
    std::unique_ptr<TlsSocket> tls_;
    bool use_tls_ = false;
    std::string host_;
    uint16_t port_ = 0;
    bool connected_ = false;
    bool closed_ = false;
};

}}} // namespace mcp::detail::net
