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

// JSON parse safety limits
#define K_MAX_MESSAGE_SIZE (8 * 1024 * 1024)  // 8MB
#define K_MAX_JSON_DEPTH 32

StdioServerTransport::StdioServerTransport(asio::io_context& io_ctx)
    : TransportBase(io_ctx)
{
    channel_ = std::make_unique<MessageChannel>(io_ctx, 64);
}

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

    if (channel_) channel_->Close();
    SetDisconnected();
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

void StdioServerTransport::ReadLoop() {
#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
#else
    int fd = STDIN_FILENO;
#endif

    std::string buffer;

    while (running_) {
        char buf[4096];

#ifdef _WIN32
        DWORD bytes_read = 0;
        if (!ReadFile(hIn, buf, sizeof(buf) - 1, &bytes_read, nullptr)) {
            break;
        }
#else
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        size_t bytes_read = static_cast<size_t>(n);
#endif
        if (bytes_read == 0) break;

        buf[bytes_read] = '\0';
        buffer.append(buf, bytes_read);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            if (line.size() > K_MAX_MESSAGE_SIZE) {
                NotifyError("message size exceeds maximum allowed size");
                continue;
            }

            try {
                auto j = nlohmann::json::parse(line, nullptr, false, K_MAX_JSON_DEPTH);
                JsonRpcMessage msg = j.get<JsonRpcMessage>();
                asio::post(IoContext(), [this, msg = std::move(msg)]() {
                    if (!running_) return;
                    if (channel_) channel_->Send(std::move(msg));
                });
            } catch (const std::exception& e) {
                NotifyError(e.what());
            }
        }
    }
}

} // namespace mcp
