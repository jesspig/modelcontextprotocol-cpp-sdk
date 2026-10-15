#pragma once

#include <transport/detail/net/TcpSocket.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace mcp { namespace detail { namespace net {

class TlsSocket;

class WebSocketClient {
public:
    using MessageCallback = std::function<void(std::string_view text)>;
    using CloseCallback = std::function<void()>;
    using ErrorCallback = std::function<void(std::string_view message)>;

    WebSocketClient();
    ~WebSocketClient();
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    void SetCallbacks(MessageCallback on_message, CloseCallback on_close, ErrorCallback on_error);

    void Open(std::string_view url, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000),
              bool verify_tls = true);

    void Send(std::string_view text);

    void Close();

private:
    void IoLoop(std::string url, std::chrono::milliseconds timeout, bool verify_tls);
    bool ReadFrame(std::string& payload, int& opcode);
    void SendFrame(int opcode, std::string_view payload);

    bool IsConnected() const;
    bool IsEof() const;
    bool ReadExact(void* buf, std::size_t len, std::chrono::milliseconds timeout);
    int ReadByte(const std::chrono::steady_clock::time_point& deadline);
    std::string ReadLine(const std::chrono::steady_clock::time_point& deadline);
    void WriteAll(std::string_view data, std::chrono::milliseconds timeout);
    void WriteFrameLocked(int opcode, std::string_view payload);
    void NotifyClose();
    void Fail(std::string_view message);

    std::unique_ptr<TcpSocket> tcp_;
    std::unique_ptr<TlsSocket> tls_;
    std::thread io_thread_;
    std::mutex send_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> closed_{false};
    std::atomic<bool> close_notified_{false};
    std::atomic<bool> peer_closed_{false};
    bool use_tls_ = false;
    bool last_fin_ = true;
    MessageCallback on_message_;
    CloseCallback on_close_;
    ErrorCallback on_error_;
};

}}}
