// TransportBench.cpp — transport-layer performance baseline measurements

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <transport/detail/net/HttpClient.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <csignal>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using mcp::detail::net::HttpClient;
using mcp::detail::net::HttpRequestSpec;

#ifdef _WIN32
void EnsureWinsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) std::abort();
    });
}
#else
void EnsureWinsock() {}
#endif

void CloseSocket(int fd) {
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

void SendAll(int fd, std::string_view data) {
    while (!data.empty()) {
#ifdef _WIN32
        int n = ::send(static_cast<SOCKET>(fd), data.data(), static_cast<int>(data.size()), 0);
#elif defined(__APPLE__)
        ssize_t n = ::send(fd, data.data(), data.size(), 0);
#else
        ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
#endif
        if (n <= 0) return;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
}

std::size_t RecvSome(int fd, char* buf, std::size_t len) {
#ifdef _WIN32
    int n = ::recv(static_cast<SOCKET>(fd), buf, static_cast<int>(len), 0);
#else
    ssize_t n = ::recv(fd, buf, len, 0);
#endif
    if (n <= 0) return 0;
    return static_cast<std::size_t>(n);
}

bool ReadRequestHead(int fd, std::string& head) {
    char buf[1024];
    for (;;) {
        std::size_t n = RecvSome(fd, buf, sizeof(buf));
        if (n == 0) return false;
        head.append(buf, n);
        if (head.find("\r\n\r\n") != std::string::npos) return true;
        if (head.size() > 64 * 1024) return false;
    }
}

// HttpClient.cpp rejects a response once its header count exceeds kMaxHeaderCount = 100, and
// BuildResponse always emits Content-Type plus Content-Length on top of the X-Pad lines.
constexpr std::size_t kProductMaxResponseHeaders = 100;
constexpr std::size_t kFixedResponseHeaders = 2;

struct Options {
    std::size_t header_lines = 100;
    std::size_t header_pad = 64;
    std::size_t body_bytes = 1024 * 1024;
    std::size_t iterations = 200;
    std::size_t concurrency = 4;
    std::size_t deferred_ms = 50;
    std::size_t deferred_iterations = 20;
};

std::size_t EffectiveHeaderLines(const Options& opt) {
    constexpr std::size_t kMaxLines = kProductMaxResponseHeaders - kFixedResponseHeaders;
    return opt.header_lines < kMaxLines ? opt.header_lines : kMaxLines;
}

std::string BuildResponse(const Options& opt, std::size_t body_bytes) {
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n";
    const std::size_t header_lines = EffectiveHeaderLines(opt);
    for (std::size_t i = 0; i < header_lines; ++i) {
        resp += "X-Pad-";
        resp += std::to_string(i);
        resp += ": ";
        resp.append(opt.header_pad, 'p');
        resp += "\r\n";
    }
    resp += "Content-Length: ";
    resp += std::to_string(body_bytes);
    resp += "\r\n\r\n";
    resp.append(body_bytes, 'b');
    return resp;
}

class BenchServer {
public:
    explicit BenchServer(Options opt) : opt_(std::move(opt)) {
        EnsureWinsock();
#ifdef _WIN32
        SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET) std::abort();
#else
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) std::abort();
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
#ifdef _WIN32
        int len = static_cast<int>(sizeof(addr));
#else
        socklen_t len = sizeof(addr);
#endif
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) std::abort();
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) std::abort();
        if (::listen(fd, 64) != 0) std::abort();
        listen_fd_ = static_cast<int>(fd);
        port_ = ntohs(addr.sin_port);
        head_only_resp_ = BuildResponse(opt_, 0);
        body_resp_ = BuildResponse(opt_, opt_.body_bytes);
        accept_thread_ = std::thread([this] { AcceptLoop(); });
    }

    ~BenchServer() {
        stop_ = true;
#ifdef _WIN32
        ::shutdown(static_cast<SOCKET>(listen_fd_), SD_BOTH);
#else
        ::shutdown(listen_fd_, SHUT_RDWR);
#endif
        CloseSocket(listen_fd_);
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
    }

    uint16_t Port() const { return port_; }

private:
    void AcceptLoop() {
        while (!stop_) {
            sockaddr_in peer{};
#ifdef _WIN32
            int plen = static_cast<int>(sizeof(peer));
#else
            socklen_t plen = sizeof(peer);
#endif
            int fd = static_cast<int>(::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen));
            if (fd < 0) {
                if (stop_) return;
                continue;
            }
            conn_threads_.emplace_back([this, fd] { Serve(fd); });
        }
    }

    void Serve(int fd) {
        std::string head;
        for (;;) {
            head.clear();
            if (!ReadRequestHead(fd, head)) break;
            bool wants_body = head.find(" /body ") != std::string::npos;
            if (head.find(" /deferred ") != std::string::npos && opt_.deferred_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(opt_.deferred_ms));
            SendAll(fd, wants_body ? body_resp_ : head_only_resp_);
        }
        CloseSocket(fd);
    }

    Options opt_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    bool stop_ = false;
    std::string head_only_resp_;
    std::string body_resp_;
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
};

struct LatencyStats {
    double min_ms = 0;
    double mean_ms = 0;
    double p50_ms = 0;
    double p95_ms = 0;
    double max_ms = 0;
};

LatencyStats ComputeStats(std::vector<double> samples) {
    LatencyStats stats;
    if (samples.empty()) return stats;
    std::sort(samples.begin(), samples.end());
    stats.min_ms = samples.front();
    stats.max_ms = samples.back();
    double sum = 0;
    for (double v : samples) sum += v;
    stats.mean_ms = sum / static_cast<double>(samples.size());
    stats.p50_ms = samples[samples.size() / 2];
    std::size_t p95_index = (samples.size() * 95) / 100;
    stats.p95_ms = samples[p95_index < samples.size() ? p95_index : samples.size() - 1];
    return stats;
}

void Report(const char* scenario, const char* metric, double value, const char* unit) {
    std::printf("%s,%s,%.3f,%s\n", scenario, metric, value, unit);
    std::fflush(stdout);
}

std::string BenchUrl(uint16_t port, const char* path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
}

void RunHeaderParseScenario(BenchServer& server, const Options& opt) {
    HttpClient client;
    HttpRequestSpec req;
    req.method = "GET";
    req.url = BenchUrl(server.Port(), "/head");
    req.timeout = std::chrono::milliseconds(30000);

    std::vector<double> samples;
    samples.reserve(opt.iterations);
    for (std::size_t i = 0; i < opt.iterations; ++i) {
        auto t0 = Clock::now();
        auto resp = client.Request(req);
        auto t1 = Clock::now();
        if (resp.status_code != 200) {
            std::fprintf(stderr, "header_parse: unexpected status %d\n", resp.status_code);
            return;
        }
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    LatencyStats stats = ComputeStats(samples);
    double total_ms = 0;
    for (double v : samples) total_ms += v;
    Report("header_parse", "min_ms", stats.min_ms, "ms");
    Report("header_parse", "p50_ms", stats.p50_ms, "ms");
    Report("header_parse", "p95_ms", stats.p95_ms, "ms");
    Report("header_parse", "max_ms", stats.max_ms, "ms");
    Report("header_parse", "mean_ms", stats.mean_ms, "ms");
    Report("header_parse", "throughput", total_ms > 0 ? static_cast<double>(opt.iterations) * 1000.0 / total_ms : 0.0,
           "req/s");
}

void RunBodyThroughputScenario(BenchServer& server, const Options& opt) {
    HttpClient client;
    HttpRequestSpec req;
    req.method = "GET";
    req.url = BenchUrl(server.Port(), "/body");
    req.timeout = std::chrono::milliseconds(60000);

    const std::size_t body_iterations = std::min<std::size_t>(opt.iterations, 16);
    std::size_t total_bytes = 0;
    auto t0 = Clock::now();
    for (std::size_t i = 0; i < body_iterations; ++i) {
        auto resp = client.Request(req, [&total_bytes](std::string_view chunk) {
            total_bytes += chunk.size();
        });
        if (resp.status_code != 200) {
            std::fprintf(stderr, "body_throughput: unexpected status %d\n", resp.status_code);
            return;
        }
    }
    auto t1 = Clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();
    Report("body_throughput", "requests", static_cast<double>(body_iterations), "count");
    Report("body_throughput", "bytes", static_cast<double>(total_bytes), "bytes");
    Report("body_throughput", "elapsed_ms", seconds * 1000.0, "ms");
    Report("body_throughput", "throughput",
           seconds > 0 ? static_cast<double>(total_bytes) / seconds / (1024.0 * 1024.0) : 0.0, "MB/s");
}

void RunConcurrencyScenario(BenchServer& server, const Options& opt) {
    const std::size_t per_thread = opt.iterations / opt.concurrency;
    std::vector<std::vector<double>> per_thread_samples(opt.concurrency);
    std::vector<std::thread> workers;
    workers.reserve(opt.concurrency);

    auto start = Clock::now();
    for (std::size_t t = 0; t < opt.concurrency; ++t) {
        workers.emplace_back([&server, &per_thread_samples, t, per_thread] {
            HttpClient client;
            HttpRequestSpec req;
            req.method = "GET";
            req.url = BenchUrl(server.Port(), "/concurrent");
            req.timeout = std::chrono::milliseconds(30000);
            auto& samples = per_thread_samples[t];
            samples.reserve(per_thread);
            for (std::size_t i = 0; i < per_thread; ++i) {
                auto t0 = Clock::now();
                try {
                    auto resp = client.Request(req);
                    if (resp.status_code != 200) continue;
                } catch (...) {
                    continue;
                }
                auto t1 = Clock::now();
                samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
        });
    }
    for (auto& w : workers) w.join();
    auto end = Clock::now();

    std::vector<double> all;
    for (auto& s : per_thread_samples) all.insert(all.end(), s.begin(), s.end());

    LatencyStats stats = ComputeStats(all);
    Report("concurrency", "requests", static_cast<double>(all.size()), "count");
    Report("concurrency", "wall_ms", std::chrono::duration<double, std::milli>(end - start).count(), "ms");
    Report("concurrency", "min_ms", stats.min_ms, "ms");
    Report("concurrency", "p50_ms", stats.p50_ms, "ms");
    Report("concurrency", "p95_ms", stats.p95_ms, "ms");
    Report("concurrency", "max_ms", stats.max_ms, "ms");
    Report("concurrency", "max_over_p50", stats.p50_ms > 0 ? stats.max_ms / stats.p50_ms : 0.0, "ratio");
}

void RunDeferredScenario(BenchServer& server, const Options& opt) {
    HttpClient client;
    HttpRequestSpec req;
    req.method = "GET";
    req.url = BenchUrl(server.Port(), "/deferred");
    req.timeout = std::chrono::milliseconds(30000);

    std::clock_t cpu0 = std::clock();
    auto wall0 = Clock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < opt.deferred_iterations; ++i) {
        try {
            auto resp = client.Request(req);
            if (resp.status_code == 200) ++completed;
        } catch (...) {
        }
    }
    auto wall1 = Clock::now();
    std::clock_t cpu1 = std::clock();

    double wall_ms = std::chrono::duration<double, std::milli>(wall1 - wall0).count();
    double cpu_ms = 1000.0 * static_cast<double>(cpu1 - cpu0) / static_cast<double>(CLOCKS_PER_SEC);
    double expected_ms = static_cast<double>(opt.deferred_ms) * static_cast<double>(completed);
    Report("deferred_response", "requests", static_cast<double>(completed), "count");
    Report("deferred_response", "wall_ms", wall_ms, "ms");
    Report("deferred_response", "expected_wall_ms", expected_ms, "ms");
    Report("deferred_response", "wall_over_expected", expected_ms > 0 ? wall_ms / expected_ms : 0.0, "ratio");
    Report("deferred_response", "cpu_ms", cpu_ms, "ms");
    Report("deferred_response", "cpu_over_wall", wall_ms > 0 ? cpu_ms / wall_ms : 0.0, "ratio");
}

void RunConnectionReuseScenario(BenchServer& server, const Options& opt) {
    HttpRequestSpec req;
    req.method = "GET";
    req.url = BenchUrl(server.Port(), "/reuse");
    req.timeout = std::chrono::milliseconds(30000);

    const std::size_t reuse_iterations = std::min<std::size_t>(opt.iterations, 100);
    std::vector<double> reuse_samples;
    reuse_samples.reserve(reuse_iterations);
    {
        HttpClient client;
        for (std::size_t i = 0; i < reuse_iterations; ++i) {
            auto t0 = Clock::now();
            auto resp = client.Request(req);
            auto t1 = Clock::now();
            if (resp.status_code != 200) continue;
            reuse_samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
    }

    std::vector<double> fresh_samples;
    fresh_samples.reserve(reuse_iterations);
    for (std::size_t i = 0; i < reuse_iterations; ++i) {
        HttpClient client;
        auto t0 = Clock::now();
        auto resp = client.Request(req);
        auto t1 = Clock::now();
        if (resp.status_code != 200) continue;
        fresh_samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    LatencyStats reuse = ComputeStats(reuse_samples);
    LatencyStats fresh = ComputeStats(fresh_samples);
    Report("connection_reuse", "reuse_p50_ms", reuse.p50_ms, "ms");
    Report("connection_reuse", "fresh_p50_ms", fresh.p50_ms, "ms");
    Report("connection_reuse", "fresh_over_reuse", reuse.p50_ms > 0 ? fresh.p50_ms / reuse.p50_ms : 0.0, "ratio");
}

std::size_t ParseSize(const char* text, std::size_t fallback) {
    if (text == nullptr || *text == '\0') return fallback;
    char* end = nullptr;
    unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text) return fallback;
    return static_cast<std::size_t>(value);
}

const char* OptionValue(std::string_view arg, std::string_view prefix) {
    if (arg.size() <= prefix.size()) return nullptr;
    if (arg.substr(0, prefix.size()) != prefix) return nullptr;
    if (arg[prefix.size()] != '=') return nullptr;
    return arg.data() + prefix.size() + 1;
}

} // namespace

int main(int argc, char** argv) {
#ifdef __APPLE__
    std::signal(SIGPIPE, SIG_IGN);
#endif

    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (const char* v = OptionValue(arg, "--header-lines")) opt.header_lines = ParseSize(v, opt.header_lines);
        else if (const char* v = OptionValue(arg, "--header-pad")) opt.header_pad = ParseSize(v, opt.header_pad);
        else if (const char* v = OptionValue(arg, "--body-bytes")) opt.body_bytes = ParseSize(v, opt.body_bytes);
        else if (const char* v = OptionValue(arg, "--iterations")) opt.iterations = ParseSize(v, opt.iterations);
        else if (const char* v = OptionValue(arg, "--concurrency")) opt.concurrency = ParseSize(v, opt.concurrency);
        else if (const char* v = OptionValue(arg, "--deferred-ms")) opt.deferred_ms = ParseSize(v, opt.deferred_ms);
        else if (const char* v = OptionValue(arg, "--deferred-iterations"))
            opt.deferred_iterations = ParseSize(v, opt.deferred_iterations);
    }
    if (opt.concurrency == 0) opt.concurrency = 1;
    if (opt.iterations == 0) opt.iterations = 1;

    std::printf("scenario,metric,value,unit\n");
    std::printf("config,header_lines,%zu,count\n", opt.header_lines);
    std::printf("config,response_headers,%zu,count\n",
                EffectiveHeaderLines(opt) + kFixedResponseHeaders);
    std::printf("config,header_pad,%zu,bytes\n", opt.header_pad);
    std::printf("config,body_bytes,%zu,bytes\n", opt.body_bytes);
    std::printf("config,iterations,%zu,count\n", opt.iterations);
    std::printf("config,concurrency,%zu,count\n", opt.concurrency);
    std::fflush(stdout);

    try {
        BenchServer server(opt);
        RunHeaderParseScenario(server, opt);
        RunBodyThroughputScenario(server, opt);
        RunConcurrencyScenario(server, opt);
        RunDeferredScenario(server, opt);
        RunConnectionReuseScenario(server, opt);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bench failed: %s\n", ex.what());
        return 1;
    }
    return 0;
}
