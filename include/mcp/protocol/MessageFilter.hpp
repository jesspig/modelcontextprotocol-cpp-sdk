#pragma once
#include <mcp/JsonRpc.hpp>
#include <memory>
#include <functional>
#include <vector>

#include <cstddef>

namespace mcp {

using MessageFilterFunc = std::function<void(
    const JsonRpcMessage&,
    std::function<void(const JsonRpcMessage&)>)>;

using MessageFilterNext = std::function<void(const JsonRpcMessage&)>;

class MessageFilter {
public:
    virtual ~MessageFilter() = default;
    virtual void Filter(const JsonRpcMessage& message, MessageFilterNext next) = 0;
};

using Interceptor = MessageFilter;

class MessageFilterFuncAdapter : public MessageFilter {
public:
    explicit MessageFilterFuncAdapter(MessageFilterFunc func) : func_(std::move(func)) {}
    void Filter(const JsonRpcMessage& message, MessageFilterNext next) override {
        func_(message, std::move(next));
    }
private:
    MessageFilterFunc func_;
};

class FilterPipeline {
private:
    struct State {
        std::vector<std::shared_ptr<MessageFilter>> filters;
        std::size_t index{0};
        MessageFilterNext final;
        bool final_called{false};
    };

    struct Chain {
        std::shared_ptr<State> state;
        MessageFilterNext next;
    };

public:
    void AddFilter(std::shared_ptr<MessageFilter> filter) {
        filters_.push_back(std::move(filter));
    }

    void AddInterceptor(std::shared_ptr<Interceptor> interceptor) {
        AddFilter(std::move(interceptor));
    }

    void Execute(const JsonRpcMessage& message, MessageFilterNext final_handler) {
        if (filters_.empty()) {
            final_handler(message);
            return;
        }

        auto chain = std::make_shared<Chain>();
        chain->state = std::make_shared<State>();
        chain->state->filters = filters_;
        chain->state->final = std::move(final_handler);
        std::weak_ptr<Chain> weak_chain = chain;
        chain->next = [weak_chain](const JsonRpcMessage& msg) {
            auto current = weak_chain.lock();
            if (!current) return;
            State& state = *current->state;
            if (state.index < state.filters.size()) {
                auto filter = state.filters[state.index++];
                filter->Filter(msg, [current](const JsonRpcMessage& filtered) {
                    current->next(filtered);
                });
            } else if (!state.final_called) {
                state.final_called = true;
                state.final(msg);
            }
        };
        chain->next(message);
    }

private:
    std::vector<std::shared_ptr<MessageFilter>> filters_;
};

} // namespace mcp
