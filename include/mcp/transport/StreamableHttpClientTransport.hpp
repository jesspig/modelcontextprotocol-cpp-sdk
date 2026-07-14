#pragma once

#include <mcp/Transport.hpp>
#include <mcp/transport/HttpTransportMode.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace mcp {

struct HttpClientTransportOptions {
    std::string endpoint;
    HttpTransportMode transport_mode = HttpTransportMode::AutoDetect;
    std::string name;
    std::string known_session_id;
    std::map<std::string, std::string> additional_headers;
};

class StreamableHttpClientTransport : public ClientTransport {
public:
    explicit StreamableHttpClientTransport(const HttpClientTransportOptions& options);
    ~StreamableHttpClientTransport() override;

    std::string_view Name() const override;
    std::unique_ptr<Transport> Connect() override;

private:
    HttpClientTransportOptions options_;
};

} // namespace mcp
