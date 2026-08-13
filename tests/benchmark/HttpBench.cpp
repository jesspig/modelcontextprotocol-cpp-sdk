// HttpBench.cpp — HTTP 基准（GET RTT / POST 吞吐 / SSE 流 / 并发）

#include <mcp/http/HttpServer.hpp>
#include <transport/detail/net/HttpClient.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

using mcp::HttpServer;
using mcp::detail::net::HttpClient;
using mcp::detail::net::HttpRequestSpec;
using mcp::detail::net::HttpResponseInfo;

namespace {

constexpr int kGetIterations = 200;
constexpr int kGetWarmup = 20;
constexpr int kPostIterations = 2000;
constexpr int kPostWarmup = 100;
constexpr int kPostBodyBytes = 1024;
constexpr int kSseEventCount = 1000;
constexpr int kConcurrentThreads = 8;
constexpr int kConcurrentPerThread = 50;

struct SseState {
    std::atomic<bool> handler_called{false};
};

bool PortIsFree(uint16_t port) {
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

uint16_t PickFreePort() {
    for (uint16_t p = 20000; p < 22000; ++p) {
        if (PortIsFree(p)) return p;
    }
    return 19876;
}

std::string ServerUrl(uint16_t port, std::string_view path) {
    return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
}

bool WaitUntilReady(uint16_t port) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            HttpClient client;
            HttpRequestSpec spec;
            spec.method = "GET";
            spec.url = ServerUrl(port, "/ping");
            spec.timeout = std::chrono::milliseconds(200);
            if (client.Request(spec).status_code == 200) return true;
        } catch (...) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void RunGetRtt(uint16_t port) {
    std::printf("== GetRtt: %d requests (warmup %d), keep-alive reuse\n", kGetIterations, kGetWarmup);
    HttpClient client;
    HttpRequestSpec spec;
    spec.method = "GET";
    spec.url = ServerUrl(port, "/ping");

    for (int i = 0; i < kGetWarmup; ++i) client.Request(spec);

    int failures = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kGetIterations; ++i) {
        const HttpResponseInfo resp = client.Request(spec);
        if (resp.status_code != 200) ++failures;
    }
    const auto end = std::chrono::steady_clock::now();

    const double avg_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / static_cast<double>(kGetIterations) / 1000.0;
    std::printf("GetRtt: %.3f ms/op\n", avg_ms);
    if (failures > 0) std::printf("GetRtt: %d non-200 responses\n", failures);
}

void RunPostThroughput(uint16_t port) {
    std::printf("== PostThroughput: %d x %d B (warmup %d)\n", kPostIterations, kPostBodyBytes, kPostWarmup);
    HttpClient client;
    HttpRequestSpec spec;
    spec.method = "POST";
    spec.url = ServerUrl(port, "/echo");
    spec.body.assign(kPostBodyBytes, 'x');

    for (int i = 0; i < kPostWarmup; ++i) client.Request(spec);

    int failures = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kPostIterations; ++i) {
        const HttpResponseInfo resp = client.Request(spec);
        if (resp.status_code != 200) ++failures;
    }
    const auto end = std::chrono::steady_clock::now();

    const double total_bytes = static_cast<double>(kPostIterations) * kPostBodyBytes;
    const double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
    const double mbps = total_bytes / seconds / 1e6;
    const double us_per_op = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / static_cast<double>(kPostIterations);
    std::printf("PostThroughput: %.2f MB/s (%.2f us/op)\n", mbps, us_per_op);
    if (failures > 0) std::printf("PostThroughput: %d non-200 responses\n", failures);
}

void RunSseStream(HttpServer& server, uint16_t port, const std::shared_ptr<SseState>& state) {
    std::printf("== SseStream: %d events via BroadcastSse\n", kSseEventCount);
    state->handler_called.store(false);

    struct SseCounter {
        std::string pending;
        int events{0};

        void Feed(std::string_view chunk) {
            pending.append(chunk.data(), chunk.size());
            std::size_t pos = 0;
            while ((pos = pending.find("\n\n", pos)) != std::string::npos) {
                ++events;
                pos += 2;
            }
            if (pending.size() > 1) pending.erase(0, pending.size() - 1);
        }
    };

    SseCounter counter;
    std::atomic<int> received{0};

    const auto start = std::chrono::steady_clock::now();
    std::thread client_thread([&] {
        try {
            HttpClient client;
            HttpRequestSpec spec;
            spec.method = "GET";
            spec.url = ServerUrl(port, "/sse");
            spec.timeout = std::chrono::seconds(10);
            client.Request(spec, [&](std::string_view chunk) {
                counter.Feed(chunk);
                received.store(counter.events);
            });
        } catch (const std::exception& e) {
            std::printf("SseStream: client aborted: %s\n", e.what());
        }
    });

    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!state->handler_called.load() && std::chrono::steady_clock::now() < wait_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    for (int i = 0; i < kSseEventCount && received.load() < kSseEventCount; ++i) {
        server.BroadcastSse("data: " + std::to_string(i) + "\n\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    server.Stop();
    client_thread.join();
    const auto end = std::chrono::steady_clock::now();

    const double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
    const int n = received.load();
    const double per_s = ms > 0 ? n / (ms / 1000.0) : 0.0;
    std::printf("SseStream: %d events in %.1f ms (%.0f events/s)\n", n, ms, per_s);
}

void RunConcurrent(uint16_t port) {
    std::printf("== Concurrent: %d threads x %d GET /ping\n", kConcurrentThreads, kConcurrentPerThread);
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    threads.reserve(kConcurrentThreads);
    for (int t = 0; t < kConcurrentThreads; ++t) {
        threads.emplace_back([port] {
            HttpClient client;
            HttpRequestSpec spec;
            spec.method = "GET";
            spec.url = ServerUrl(port, "/ping");
            for (int i = 0; i < kConcurrentPerThread; ++i) {
                const HttpResponseInfo resp = client.Request(spec);
                if (resp.status_code != 200)
                    std::printf("Concurrent: non-200 response\n");
            }
        });
    }
    for (auto& th : threads) th.join();
    const auto end = std::chrono::steady_clock::now();

    const double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
    std::printf("Concurrent(%dx%d): %.1f ms total\n", kConcurrentThreads, kConcurrentPerThread, ms);
}

} // namespace

int main() {
    const uint16_t port = PickFreePort();
    std::printf("HttpBench: server on 127.0.0.1:%u\n", static_cast<unsigned>(port));

    HttpServer server(port);
    server.SetHandler("GET", "/ping", [](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
        resp.status_code = 200;
        resp.body = "pong";
    });
    server.SetHandler("POST", "/echo", [](const mcp::HttpRequest& req, mcp::HttpResponse& resp) {
        resp.status_code = 200;
        resp.body = req.body;
    });

    auto sse_state = std::make_shared<SseState>();
    server.SetHandler("GET", "/sse", [sse_state](const mcp::HttpRequest&, mcp::HttpResponse& resp) {
        resp.status_code = 200;
        resp.is_sse = true;
        sse_state->handler_called.store(true);
    });

    server.Start();
    if (!WaitUntilReady(port)) std::printf("HttpBench: server not ready\n");
    RunGetRtt(port);
    RunPostThroughput(port);
    RunConcurrent(port);
    RunSseStream(server, port, sse_state);

    std::printf("HttpBench: done\n");
    return 0;
}
