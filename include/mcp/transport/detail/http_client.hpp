#pragma once

#include "Url.hpp"

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include <sys/socket.h>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mcp { namespace detail {

inline std::string HttpGet(asio::io_context& io_ctx, const std::string& url) {
    auto u = ParseUrl(url);

    asio::ip::tcp::resolver resolver(io_ctx);
    asio::ip::tcp::socket socket(io_ctx);

    auto endpoints = resolver.resolve(u.host, std::to_string(u.port));
    asio::connect(socket, endpoints);

    {
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    std::string request =
        "GET " + u.path + " HTTP/1.1\r\n"
        "Host: " + u.host + "\r\n"
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
    auto u = ParseUrl(url);

    asio::ip::tcp::resolver resolver(io_ctx);
    asio::ip::tcp::socket socket(io_ctx);

    auto endpoints = resolver.resolve(u.host, std::to_string(u.port));
    asio::connect(socket, endpoints);

    {
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    std::string request =
        "POST " + u.path + " HTTP/1.1\r\n"
        "Host: " + u.host + "\r\n"
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
