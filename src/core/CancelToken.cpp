#include <mcp/CancelToken.hpp>

#include <mutex>
#include <unordered_map>
#include <utility>

namespace mcp {

struct CancelToken::State {
    mutable std::mutex mutex;
    bool cancelled{false};
    std::string reason;
    std::size_t next_handler_id{1};
    std::unordered_map<std::size_t, CancelHandler> handlers;
};

CancelToken::CancelToken()
    : state_(std::make_shared<State>())
{
}

void CancelToken::Cancel(std::string_view reason) {
    std::unordered_map<std::size_t, CancelHandler> handlers;
    std::string effective_reason;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->cancelled) return;
        state_->cancelled = true;
        state_->reason = std::string(reason);
        handlers = std::move(state_->handlers);
        state_->handlers.clear();
        effective_reason = state_->reason;
    }
    for (const auto& entry : handlers) {
        if (entry.second) entry.second(effective_reason);
    }
}

bool CancelToken::IsCancelled() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->cancelled;
}

std::string CancelToken::Reason() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->reason;
}

std::size_t CancelToken::AddCancelHandler(CancelHandler handler) {
    std::string reason;
    std::size_t handler_id = 0;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->cancelled) {
            handler_id = state_->next_handler_id++;
            state_->handlers.emplace(handler_id, std::move(handler));
            return handler_id;
        }
        reason = state_->reason;
    }
    if (handler) handler(reason);
    return 0;
}

void CancelToken::RemoveCancelHandler(std::size_t handler_id) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->handlers.erase(handler_id);
}

} // namespace mcp
