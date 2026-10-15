#include <mcp/ext/Triggers.hpp>

#include <transport/detail/net/HttpClient.hpp>

#include <mcp/ErrorCodes.hpp>
#include <mcp/Log.hpp>
#include <mcp/McpError.hpp>

#include <thread>

namespace mcp::ext {

void TriggerRegistry::Register(const std::string& id,
                               std::vector<std::string> types,
                               const std::string& webhook_url) {
    if (id.empty()) {
        throw McpError(McpErrorCode::InvalidParams, "trigger id must not be empty");
    }
    if (types.empty()) {
        throw McpError(McpErrorCode::InvalidParams, "trigger types must not be empty");
    }
    TriggerRegistration reg;
    reg.types = std::move(types);
    reg.webhook_url = webhook_url;
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[id] = std::move(reg);
}

bool TriggerRegistry::Unregister(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.erase(id) > 0;
}

bool TriggerRegistry::Has(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(id) != entries_.end();
}

bool TriggerRegistry::Empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty();
}

std::size_t TriggerRegistry::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::vector<std::pair<std::string, TriggerRegistration>> TriggerRegistry::Match(
    const std::string& type) const {
    std::vector<std::pair<std::string, TriggerRegistration>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, reg] : entries_) {
        for (const auto& candidate : reg.types) {
            if (candidate == type) {
                out.emplace_back(id, reg);
                break;
            }
        }
    }
    return out;
}

JsonValue SerializeTriggerEvent(const std::string& type, const JsonValue& data) {
    JsonValue out(JsonValue::object_tag);
    out["type"] = JsonValue(type);
    out["data"] = data;
    return out;
}

void PostTriggerWebhook(const std::string& webhook_url,
                        const std::string& body,
                        std::chrono::milliseconds timeout) {
    constexpr int kMaxAttempts = 3;
    constexpr std::chrono::milliseconds kBaseBackoff(100);
    std::string last_error = "unknown error";
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(kBaseBackoff * (1 << (attempt - 1)));
        }
        try {
            detail::net::HttpRequestSpec req;
            req.method = "POST";
            req.url = webhook_url;
            req.headers["Content-Type"] = "application/json";
            req.body = body;
            req.timeout = timeout;
            detail::net::HttpClient client;
            auto resp = client.Request(req);
            if (resp.status_code >= 200 && resp.status_code < 300) return;
            last_error = "unexpected status " + std::to_string(resp.status_code);
        } catch (const std::exception& e) {
            last_error = e.what();
        }
    }
    MCP_LOG(Warning, "trigger webhook POST failed: " + webhook_url + ": " + last_error);
}

} // namespace mcp::ext
