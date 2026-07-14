#include <mcp/transport/StdioServerTransport.hpp>
#include <mcp/JsonRpc.hpp>

#include <asio/post.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace mcp {

StdioServerTransport::StdioServerTransport(asio::io_context& io_ctx)
    : io_ctx_(io_ctx)
    , channel_(io_ctx, 64)
{}

StdioServerTransport::~StdioServerTransport() {
    Close();
}

void StdioServerTransport::Start() {
    if (running_.exchange(true)) return;
    read_thread_ = std::thread([this]() { ReadLoop(); });
}

void StdioServerTransport::Close() {
    if (!running_.exchange(false)) return;

    // Read thread will break on read failure after CloseHandle
    if (read_thread_.joinable())
        read_thread_.join();

    channel_.close();
    NotifyClose();
}

void StdioServerTransport::SendMessageAsync(JsonRpcMessage message) {
    if (!running_) return;

    nlohmann::json j = message;
    std::string line = j.dump() + "\n";

#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut && hOut != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hOut, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
#else
    write(STDOUT_FILENO, line.data(), line.size());
#endif
}

Transport::MessageChannel& StdioServerTransport::GetMessageChannel() {
    return channel_;
}

asio::io_context& StdioServerTransport::IoContext() {
    return io_ctx_;
}

void StdioServerTransport::ReadLoop() {
#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
#else
    int fd = STDIN_FILENO;
#endif

    std::string buffer;

    while (running_) {
        char buf[4096];
        DWORD bytes_read = 0;

#ifdef _WIN32
        if (!ReadFile(hIn, buf, sizeof(buf) - 1, &bytes_read, nullptr)) {
            break;
        }
#else
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        bytes_read = static_cast<DWORD>(n);
#endif
        if (bytes_read == 0) break;

        buf[bytes_read] = '\0';
        buffer.append(buf, bytes_read);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            try {
                auto j = nlohmann::json::parse(line);
                JsonRpcMessage msg = j.get<JsonRpcMessage>();
                asio::post(io_ctx_, [this, msg = std::move(msg)]() {
                    if (!running_) return;
                    channel_.async_send(asio::error_code{}, std::move(msg),
                        [](asio::error_code) {});
                });
            } catch (const std::exception& e) {
                NotifyError(e.what());
            }
        }
    }
}

} // namespace mcp
