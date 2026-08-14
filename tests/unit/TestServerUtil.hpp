#pragma once
// TestServerUtil.hpp — shared HTTP test helpers: free-port probing and
// readiness polling (used by HttpServerTests / OAuthTests).

#include <chrono>
#include <string>
#include <thread>
#include <transport/detail/net/HttpClient.hpp>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static const uint16_t kTestBasePort = 18765;

// Port availability probe: a fresh bind succeeds only when the port is free.
inline bool PortIsFree(uint16_t port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    closesocket(s);
    return rc == 0;
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(s);
    return rc == 0;
#endif
}

// Pick a free port near the preferred one so parallel test runs don't collide.
// Windows may reserve contiguous port ranges (Hyper-V/WSL/Docker excluded
// ranges); scan a wide window so the probe walks past such ranges.
inline uint16_t PickFreePort(uint16_t preferred) {
    for (uint16_t p = preferred; p < preferred + 2000; ++p) {
        if (PortIsFree(p)) return p;
    }
    return preferred;
}

// Poll until the server answers any request (ready) or the deadline passes.
inline bool WaitUntilReady(uint16_t port) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        mcp::detail::net::HttpClient client;
        mcp::detail::net::HttpRequestSpec req;
        req.method = "GET";
        req.url = "http://127.0.0.1:" + std::to_string(port) + "/ready";
        req.timeout = std::chrono::milliseconds(200);
        try {
            auto resp = client.Request(req);
            if (resp.status_code != 0) return true;
        } catch (...) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
