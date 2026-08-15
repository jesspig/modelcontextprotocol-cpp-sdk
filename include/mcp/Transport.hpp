#pragma once
// Transport.hpp — transport abstraction layer: ITransport, TransportBase, IClientTransport

#include <mcp/Export.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>
#include <mcp/protocol/MessageChannel.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <memory>
#include <string_view>
#include <functional>
#include <atomic>
#include <mutex>
#include <system_error>
#include <exception>

namespace mcp {

// ═══════════════════════════════════════════════════════════════════════
// ITransport — established bidirectional session
// ═══════════════════════════════════════════════════════════════════════
class MCP_API ITransport {
public:
    virtual ~ITransport() = default;
    virtual std::string_view SessionId() const = 0;
    virtual MessageChannel& GetMessageChannel() = 0;
    virtual void SendMessageAsync(JsonRpcMessage message) = 0;
    virtual void Close() = 0;
    virtual bool IsStateless() const { return false; }
    // Called by McpServer to bring up the transport's IO threads; idempotent by contract
    virtual void Start() {}
};

// ═══════════════════════════════════════════════════════════════════════
// TransportBase — 3-state machine base class
// ═══════════════════════════════════════════════════════════════════════
enum class TransportState { Initial, Connected, Disconnected };

class MCP_API TransportBase : public ITransport, public std::enable_shared_from_this<TransportBase> {
public:
    TransportBase();
    virtual ~TransportBase();

    // ITransport
    std::string_view SessionId() const override { return session_id_; }
    MessageChannel& GetMessageChannel() override { return *channel_; }
    bool IsStateless() const override { return false; }

    // Lifecycle
    void SetConnected();
    void SetDisconnected();
    TransportState GetState() const { return static_cast<TransportState>(state_.load()); }

    // Callbacks
    void SetOnClose(std::function<void()> cb) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        on_close_ = std::move(cb);
    }
    void SetOnError(std::function<void(std::string_view)> cb) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        on_error_ = std::move(cb);
    }

protected:
    void NotifyClose();
    void NotifyError(std::string_view msg);
    void WriteMessage(JsonRpcMessage message);

    std::unique_ptr<MessageChannel> channel_;
    // Set only by StreamableHttpServerTransport; empty for all other transports
    std::string session_id_;
    std::atomic<int> state_{0}; // 0=Initial, 1=Connected, 2=Disconnected
    std::function<void()> on_close_;
    std::function<void(std::string_view)> on_error_;
    std::mutex callback_mutex_;
};

// ═══════════════════════════════════════════════════════════════════════
// IClientTransport — connection factory
// ═══════════════════════════════════════════════════════════════════════
class MCP_API IClientTransport {
public:
    virtual ~IClientTransport() = default;
    virtual std::string_view Name() const = 0;
    virtual std::shared_ptr<ITransport> Connect() = 0;
};

// TransportBase inline implementation
inline TransportBase::TransportBase() {
    channel_ = std::make_unique<MessageChannel>(detail::kChannelCapacity);
}
inline TransportBase::~TransportBase() = default;

inline void TransportBase::SetConnected() {
    int expected = static_cast<int>(TransportState::Initial);
    if (!state_.compare_exchange_strong(expected,
            static_cast<int>(TransportState::Connected))) {
        MCP_LOG(Warning, "transport: SetConnected ignored (state is not Initial)");
    }
}

inline void TransportBase::SetDisconnected() {
    if (state_.exchange(static_cast<int>(TransportState::Disconnected)) ==
        static_cast<int>(TransportState::Disconnected))
        return;
    NotifyClose();
}

// Callback helpers
inline void TransportBase::NotifyClose() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = on_close_;
    }
    if (cb) cb();
}
inline void TransportBase::NotifyError(std::string_view msg) {
    std::function<void(std::string_view)> cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = on_error_;
    }
    if (cb) cb(msg);
}
inline void TransportBase::WriteMessage(JsonRpcMessage message) {
    if (channel_ && !channel_->Send(std::move(message)))
        MCP_LOG(Warning, "message dropped: channel closed");
}

} // namespace mcp
