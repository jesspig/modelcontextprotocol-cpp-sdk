#pragma once

#include <mcp/JsonValue.hpp>

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mcp::ext {

struct TriggerRegistration {
    std::vector<std::string> types;
    std::string webhook_url;
};

class TriggerRegistry {
public:
    void Register(const std::string& id,
                  std::vector<std::string> types,
                  const std::string& webhook_url);
    bool Unregister(const std::string& id);
    bool Has(const std::string& id) const;
    bool Empty() const;
    std::size_t Size() const;
    std::vector<std::pair<std::string, TriggerRegistration>> Match(
        const std::string& type) const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, TriggerRegistration> entries_;
};

JsonValue SerializeTriggerEvent(const std::string& type, const JsonValue& data);

void PostTriggerWebhook(const std::string& webhook_url,
                        const std::string& body,
                        std::chrono::milliseconds timeout =
                            std::chrono::milliseconds(5000));

} // namespace mcp::ext
