#include <mcp/ext/Triggers.hpp>

#include <transport/detail/net/HttpClient.hpp>

#include <mcp/ErrorCodes.hpp>
#include <mcp/Log.hpp>
#include <mcp/McpError.hpp>

#include <thread>

namespace mcp::ext {

namespace {

std::string RedactedWebhookTarget(std::string_view webhook_url) {
    auto scheme_end = webhook_url.find("://");
    if (scheme_end == std::string_view::npos) return "<redacted-webhook>";
    auto rest = webhook_url.substr(scheme_end + 3);
    auto authority_end = rest.find_first_of("/?#");
    std::string authority(rest.substr(0, authority_end));
    auto at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    if (authority.empty()) return "<redacted-webhook>";
    return std::string(webhook_url.substr(0, scheme_end + 3)) + authority + "/***";
}

std::string StripWebhookSecrets(std::string text, std::string_view webhook_url) {
    if (webhook_url.empty()) return text;
    std::string redacted = RedactedWebhookTarget(webhook_url);
    std::string out;
    size_t pos = 0;
    for (;;) {
        auto found = text.find(webhook_url, pos);
        if (found == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        out.append(text, pos, found - pos);
        out += redacted;
        pos = found + webhook_url.size();
    }
    return out;
}

} // anonymous namespace

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
    MCP_LOG(Warning, "trigger webhook POST failed: " +
        RedactedWebhookTarget(webhook_url) + ": " +
        StripWebhookSecrets(last_error, webhook_url));
}

} // namespace mcp::ext
