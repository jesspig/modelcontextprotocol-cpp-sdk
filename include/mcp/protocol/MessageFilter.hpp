#pragma once
// MessageFilter.hpp
// Filter pipeline for intercepting incoming and outgoing JSON-RPC messages
#include <mcp/JsonRpc.hpp>
#include <memory>
#include <functional>
#include <vector>

namespace mcp {

// ═══════════════════════════════════════════════════════════════════════
// MessageFilter pipeline — similar to C# McpSessionHandler filters
// ═══════════════════════════════════════════════════════════════════════

using MessageFilterFunc = std::function<void(
    const JsonRpcMessage&,
    std::function<void(const JsonRpcMessage&)>)>;

using MessageFilterNext = std::function<void(const JsonRpcMessage&)>;

class MessageFilter {
public:
    virtual ~MessageFilter() = default;
    virtual void Filter(const JsonRpcMessage& message, MessageFilterNext next) = 0;
};

// Concrete filter wrapping a MessageFilterFunc (for ease of use)
class MessageFilterFuncAdapter : public MessageFilter {
public:
    explicit MessageFilterFuncAdapter(MessageFilterFunc func) : func_(std::move(func)) {}
    void Filter(const JsonRpcMessage& message, MessageFilterNext next) override {
        func_(message, std::move(next));
    }
private:
    MessageFilterFunc func_;
};

// Filter pipeline — chains multiple filters together
class FilterPipeline {
public:
    void AddFilter(std::shared_ptr<MessageFilter> filter) {
        filters_.push_back(std::move(filter));
    }

    void Execute(const JsonRpcMessage& message, MessageFilterNext final_handler) {
        if (filters_.empty()) {
            final_handler(message);
            return;
        }

        auto index = std::make_shared<size_t>(0);
        std::function<void(const JsonRpcMessage&)> execute_next;
        execute_next = [this, index, &execute_next, final_handler](const JsonRpcMessage& msg) {
            if (*index < filters_.size()) {
                auto filter = filters_[(*index)++];
                filter->Filter(msg, execute_next);
            } else {
                final_handler(msg);
            }
        };
        execute_next(message);
    }

private:
    std::vector<std::shared_ptr<MessageFilter>> filters_;
};

} // namespace mcp
