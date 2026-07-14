#pragma once

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mcp { namespace detail {

inline std::string HttpGet(asio::io_context& io_ctx, const std::string& url) {
    // Parse URL
    auto sp = url.find("://");
    if (sp == std::string::npos)
        throw std::invalid_argument("Invalid URL: " + url);
    auto scheme = url.substr(0, sp);
    auto hs = sp + 3;
    auto ps = url.find('/', hs);
    std::string host, path;
    uint16_t port = 0;
    if (ps != std::string::npos) {
        auto hp = url.substr(hs, ps - hs);
        path = url.substr(ps);
        auto col = hp.find(':');
        if (col != std::string::npos) {
            host = hp.substr(0, col);
            port = static_cast<uint16_t>(std::stoul(hp.substr(col + 1)));
        } else {
            host = hp;
            port = (scheme == "https") ? 443 : 80;
        }
    } else {
        host = url.substr(hs);
        path = "/";
        port = (scheme == "https") ? 443 : 80;
    }

    asio::ip::tcp::resolver resolver(io_ctx);
    asio::ip::tcp::socket socket(io_ctx);

    auto endpoints = resolver.resolve(host, std::to_string(port));
    asio::connect(socket, endpoints);

    std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Connection: close\r\n"
        "\r\n";

    asio::write(socket, asio::buffer(request));

    std::string response;
    asio::error_code ec;
    while (true) {
        char buf[4096];
        size_t len = socket.read_some(asio::buffer(buf), ec);
        if (ec) break;
        response.append(buf, len);
    }

    auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos)
        return {};

    // Check status
    auto status_end = response.find("\r\n");
    if (status_end != std::string::npos) {
        auto status_line = response.substr(9, 3); // "HTTP/1.1 XXX"
        int code = std::stoi(std::string(status_line.data(), 3));
        if (code < 200 || code >= 300)
            throw std::runtime_error("HTTP GET " + url + " returned " + std::to_string(code));
    }

    return response.substr(body_start + 4);
}

inline std::string HttpPost(
    asio::io_context& io_ctx,
    const std::string& url,
    const std::string& body,
    const std::string& content_type = "application/json")
{
    auto sp = url.find("://");
    if (sp == std::string::npos)
        throw std::invalid_argument("Invalid URL: " + url);
    auto scheme = url.substr(0, sp);
    auto hs = sp + 3;
    auto ps = url.find('/', hs);
    std::string host, path;
    uint16_t port = 0;
    if (ps != std::string::npos) {
        auto hp = url.substr(hs, ps - hs);
        path = url.substr(ps);
        auto col = hp.find(':');
        if (col != std::string::npos) {
            host = hp.substr(0, col);
            port = static_cast<uint16_t>(std::stoul(hp.substr(col + 1)));
        } else {
            host = hp;
            port = (scheme == "https") ? 443 : 80;
        }
    } else {
        host = url.substr(hs);
        path = "/";
        port = (scheme == "https") ? 443 : 80;
    }

    asio::ip::tcp::resolver resolver(io_ctx);
    asio::ip::tcp::socket socket(io_ctx);

    auto endpoints = resolver.resolve(host, std::to_string(port));
    asio::connect(socket, endpoints);

    std::string request =
        "POST " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Content-Type: " + content_type + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body;

    asio::write(socket, asio::buffer(request));

    std::string response;
    asio::error_code ec;
    while (true) {
        char buf[4096];
        size_t len = socket.read_some(asio::buffer(buf), ec);
        if (ec) break;
        response.append(buf, len);
    }

    auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos)
        return {};

    return response.substr(body_start + 4);
}

}} // namespace mcp::detail
