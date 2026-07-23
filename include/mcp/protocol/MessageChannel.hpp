#pragma once
#include <mcp/Export.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <system_error>

namespace mcp {

// ═══════════════════════════════════════════════════════════════════════
// MessageChannel — bounded async message queue with backpressure
// Replaces asio::experimental::channel with pure C++17 primitives
// ═══════════════════════════════════════════════════════════════════════
class MCP_API MessageChannel {
public:
    explicit MessageChannel(size_t max_buffer = 64)
        : max_buffer_(max_buffer) {}

    // Async receive — calls callback when a message is available
    // The callback is invoked from within the lock; caller should not block.
    template <typename Callback>
    void AsyncReceive(Callback&& cb) {
        JsonRpcMessage msg;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !queue_.empty() || closed_; });
            if (closed_) {
                lock.unlock();
                cb(std::make_error_code(std::errc::operation_canceled),
                   JsonRpcMessage{});
                return;
            }
            msg = std::move(queue_.front());
            queue_.pop();
        }
        // Notify any blocked sender
        cv_send_.notify_one();
        cb(std::error_code{}, std::move(msg));
    }

    // Send a message into the channel (blocks if buffer is full)
    void Send(JsonRpcMessage message) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_send_.wait(lock, [this] { return queue_.size() < max_buffer_ || closed_; });
            if (closed_) return;
            queue_.push(std::move(message));
        }
        cv_.notify_one();
    }

    // Try to send without blocking; returns false if buffer is full
    bool TrySend(JsonRpcMessage message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || queue_.size() >= max_buffer_) return false;
            queue_.push(std::move(message));
        }
        cv_.notify_one();
        return true;
    }

    // Close the channel — wakes all waiters
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
        cv_send_.notify_all();
    }

    // Check if open
    bool IsOpen() const { return !closed_.load(); }

    // Check if empty
    bool Empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    size_t max_buffer_;
    std::queue<JsonRpcMessage> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable cv_send_;
    std::atomic<bool> closed_{false};
};

} // namespace mcp
