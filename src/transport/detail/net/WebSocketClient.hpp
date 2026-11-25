#pragma once

#include <transport/detail/net/TcpSocket.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mcp { namespace detail { namespace net {

class TlsSocket;

class WebSocketClient {
public:
    enum class HandshakeState {
        Connecting,
        Open,
        Closing,
        Closed
    };

    using MessageCallback = std::function<void(std::string_view text)>;
    using CloseCallback = std::function<void()>;
    using ErrorCallback = std::function<void(std::string_view message)>;

    WebSocketClient();
    ~WebSocketClient();
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    void SetCallbacks(MessageCallback on_message, CloseCallback on_close, ErrorCallback on_error);
    void SetOpenCallback(std::function<void()> on_open);

    [[nodiscard]] HandshakeState State() const;

    void Open(std::string_view url, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000),
              bool verify_tls = true);

    void Send(std::string_view text);

    void Close();

private:
    struct HandshakeResult {
        int status_code = 0;
        std::string location;
        std::string key;
    };

    void IoLoop(std::string url, std::chrono::milliseconds timeout, bool verify_tls);
    bool PerformHandshake(const std::string& url,
                          const std::chrono::steady_clock::time_point& deadline,
                          bool verify_tls, HandshakeResult& result);
    void CompleteOpen();
    void FlushQueuedMessages();
    void ReportDroppedMessages(std::vector<std::string> messages, std::string_view reason);
    bool ReadFrame(std::string& payload, int& opcode);
    void SendFrame(int opcode, std::string_view payload);

    bool IsConnected() const;
    bool IsEof() const;
    bool ReadExact(void* buf, std::size_t len, std::chrono::milliseconds timeout);
    int ReadByte(const std::chrono::steady_clock::time_point& deadline);
    std::string ReadLine(const std::chrono::steady_clock::time_point& deadline);
    void WriteAll(std::string_view data, std::chrono::milliseconds timeout);
    void WriteFrameLocked(int opcode, std::string_view payload);
    void CloseSockets(bool reset);
    void Terminate();
    void NotifyClose();
    void Fail(std::string_view message);

    std::unique_ptr<TcpSocket> tcp_;
    std::unique_ptr<TlsSocket> tls_;
    mutable std::mutex socket_mutex_;
    std::thread io_thread_;
    std::mutex send_mutex_;
    std::deque<std::string> pending_;
    std::size_t pending_bytes_ = 0;
    std::atomic<HandshakeState> state_{HandshakeState::Closed};
    std::atomic<bool> running_{false};
    std::atomic<bool> closed_{false};
    std::atomic<bool> close_notified_{false};
    std::atomic<bool> peer_closed_{false};
    std::atomic<bool> use_tls_{false};
    bool last_fin_ = true;
    MessageCallback on_message_;
    CloseCallback on_close_;
    ErrorCallback on_error_;
    std::function<void()> on_open_;
};

}}}