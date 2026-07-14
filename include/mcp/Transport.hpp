#pragma once
#include <mcp/JsonRpc.hpp>
#include <mcp/protocol/MessageChannel.hpp>
#include <asio/io_context.hpp>
#include <memory>
#include <string_view>
#include <functional>
#include <atomic>
#include <system_error>
#include <exception>
#include <mutex>

namespace mcp {

// ═══════════════════════════════════════════════════════════════════════
// IStatelessTransport — marker for stateless transport (no session persistence)
// ═══════════════════════════════════════════════════════════════════════
class IStatelessTransport {};

// ═══════════════════════════════════════════════════════════════════════
// ITransport — established bidirectional session
// ═══════════════════════════════════════════════════════════════════════
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual std::string_view SessionId() const = 0;
    virtual MessageChannel& GetMessageChannel() = 0;
    virtual void SendMessageAsync(JsonRpcMessage message) = 0;
    virtual void Close() = 0;
    virtual bool IsStateless() const { return false; }
};

// ═══════════════════════════════════════════════════════════════════════
// TransportBase — 3-state machine base class
// ═══════════════════════════════════════════════════════════════════════
enum class TransportState { Initial, Connected, Disconnected };

class TransportBase : public ITransport, public std::enable_shared_from_this<TransportBase> {
public:
    TransportBase(asio::io_context& io_ctx);
    virtual ~TransportBase();

    // ITransport
    std::string_view SessionId() const override { return session_id_; }
    MessageChannel& GetMessageChannel() override { return *channel_; }
    bool IsStateless() const override { return false; }

    // Lifecycle
    void SetConnected();
    void SetDisconnected(std::exception_ptr error = nullptr);
    TransportState GetState() const { return static_cast<TransportState>(state_.load()); }

    // Callbacks
    void SetOnClose(std::function<void()> cb) { on_close_ = std::move(cb); }
    void SetOnError(std::function<void(std::string_view)> cb) { on_error_ = std::move(cb); }

    asio::io_context& IoContext() { return io_ctx_; }

protected:
    void NotifyClose();
    void NotifyError(std::string_view msg);
    void WriteMessage(JsonRpcMessage message);

    asio::io_context& io_ctx_;
    std::unique_ptr<MessageChannel> channel_;
    std::string session_id_;
    std::atomic<int> state_{0}; // 0=Initial, 1=Connected, 2=Disconnected
    std::function<void()> on_close_;
    std::function<void(std::string_view)> on_error_;
};

// ═══════════════════════════════════════════════════════════════════════
// IClientTransport — connection factory
// ═══════════════════════════════════════════════════════════════════════
class IClientTransport {
public:
    virtual ~IClientTransport() = default;
    virtual std::string_view Name() const = 0;
    virtual std::shared_ptr<ITransport> Connect() = 0;
};

// ═══════════════════════════════════════════════════════════════════════
// Transport — backward-compatible base class (pre-refactoring name)
// ═══════════════════════════════════════════════════════════════════════
// Inherits ITransport and provides the pre-refactoring interface:
// Start(), IoContext(), NotifyClose/NotifyError helpers, callback storage.
// New implementations should prefer ITransport or TransportBase.
class Transport : public ITransport {
public:
    virtual ~Transport() = default;

    // Transport-specific additions (not part of ITransport)
    virtual void Start() = 0;
    virtual asio::io_context& IoContext() = 0;

    // ITransport defaults
    std::string_view SessionId() const override { return {}; }
    bool IsStateless() const override { return false; }

    // Backward-compatible inner typedef
    using MessageChannel = mcp::MessageChannel;

    // Callbacks
    void SetOnClose(std::function<void()> cb) { on_close_ = std::move(cb); }
    void SetOnError(std::function<void(std::string_view)> cb) { on_error_ = std::move(cb); }

protected:
    void NotifyClose() { if (on_close_) on_close_(); }
    void NotifyError(std::string_view msg) { if (on_error_) on_error_(msg); }

    std::function<void()> on_close_;
    std::function<void(std::string_view)> on_error_;
};

// ═══════════════════════════════════════════════════════════════════════
// ClientTransport — backward-compatible base class
// ═══════════════════════════════════════════════════════════════════════
class ClientTransport : public IClientTransport {
public:
    ~ClientTransport() override = default;
};

// TransportBase inline implementation
inline TransportBase::TransportBase(asio::io_context& io_ctx) : io_ctx_(io_ctx) {}
inline TransportBase::~TransportBase() = default;

inline void TransportBase::SetConnected() { state_ = static_cast<int>(TransportState::Connected); }

inline void TransportBase::SetDisconnected(std::exception_ptr /*error*/) {
    state_ = static_cast<int>(TransportState::Disconnected);
    NotifyClose();
}

// Callback helpers
inline void TransportBase::NotifyClose() { if (on_close_) on_close_(); }
inline void TransportBase::NotifyError(std::string_view msg) { if (on_error_) on_error_(msg); }
inline void TransportBase::WriteMessage(JsonRpcMessage message) { if (channel_) channel_->Send(std::move(message)); }

} // namespace mcp
