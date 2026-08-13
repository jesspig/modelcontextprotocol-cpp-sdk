#pragma once

// WebSocketClient.hpp — RFC 6455 WebSocket 客户端

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

    // 异步连接：起 IO 线程执行 连接+TLS+HTTP 握手 → 成功触发 on_message/on_close/on_error；
    // 失败（连接拒绝/超时/TLS/握手 4xx）→ on_error(原因) → on_close；线程结束。
    // Open 本身不抛（错误走回调），返回 void。
    void Open(std::string_view url, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000),
              bool verify_tls = true);

    // 线程安全：发送文本帧（客户端掩码）。未连接/关闭时静默丢弃。
    void Send(std::string_view text);

    // 中断 IO 线程并 join（防 self-join：若当前线程即 IO 线程则 detach——参照项目 JoinThreadSafely 模式）。
    // 触发 on_close（若尚未触发）。
    void Close();

    bool IsRunning() const;   // IO 线程存活且未请求关闭

private:
    void IoLoop(std::string url, std::chrono::milliseconds timeout, bool verify_tls);
    bool ReadFrame(std::string& payload, int& opcode);   // 读一帧（阻塞），失败返回 false
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
