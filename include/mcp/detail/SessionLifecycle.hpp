#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace mcp { namespace detail {

enum class SessionPhase { Initial, Connecting, Open, Closing, Closed };

class SessionLifecycle {
public:
    SessionLifecycle() = default;

    uint64_t BeginConnect(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (phase_ == SessionPhase::Closing) {
            cv_.wait_for(lock, timeout, [this] { return phase_ != SessionPhase::Closing; });
        }
        ++generation_;
        phase_ = SessionPhase::Connecting;
        return generation_;
    }

    void CompleteConnect(uint64_t generation) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) return;
            if (phase_ == SessionPhase::Initial || phase_ == SessionPhase::Connecting)
                phase_ = SessionPhase::Open;
        }
        cv_.notify_all();
    }

    bool BeginClose(uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_) return false;
        if (phase_ == SessionPhase::Closing || phase_ == SessionPhase::Closed) return false;
        phase_ = SessionPhase::Closing;
        return true;
    }

    void CompleteClose(uint64_t generation) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) return;
            if (phase_ == SessionPhase::Closed) return;
            phase_ = SessionPhase::Closed;
        }
        cv_.notify_all();
    }

    bool WaitClosed(uint64_t generation, std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(mutex_);
        if (generation != generation_) return true;
        return cv_.wait_for(lock, timeout, [this] { return phase_ == SessionPhase::Closed; });
    }

    bool IsActive(uint64_t generation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation == generation_ &&
               (phase_ == SessionPhase::Connecting || phase_ == SessionPhase::Open);
    }

    uint64_t Generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }

    SessionPhase Phase() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return phase_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    SessionPhase phase_ = SessionPhase::Initial;
    uint64_t generation_ = 0;
};

}} // namespace mcp::detail
