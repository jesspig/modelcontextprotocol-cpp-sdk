#include <mcp/transport/StdioServerTransport.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>

namespace mcp {

// JSON parse safety limits
#define K_MAX_MESSAGE_SIZE (8 * 1024 * 1024)  // 8MB
// K_MAX_JSON_DEPTH removed — nlohmann-json v3.11.3 parse() accepts 4 args max
// The constant was being passed as ignore_comments=true, enabling JSON comment parsing

StdioServerTransport::StdioServerTransport()
    : TransportBase()
{
}

StdioServerTransport::~StdioServerTransport() {
    Close();
}

void StdioServerTransport::Start() {
    if (running_.exchange(true)) return;

    stdin_pipe_ = detail::OpenStandardInput();
    stdout_pipe_ = detail::OpenStandardOutput();

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

    auto line = SerializeMessage(message) + "\n";

    stdout_pipe_->Write(line.data(), line.size());
}

void StdioServerTransport::ReadLoop() {
    std::string buffer;

    while (running_) {
        char buf[4096];

        size_t bytes_read = stdin_pipe_->Read(buf, sizeof(buf) - 1);
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
                auto msg = DeserializeMessage(line);
                if (channel_) channel_->Send(std::move(msg));
            } catch (const std::exception& e) {
                MCP_LOG(Error, std::string("stdio parse error: ") + e.what());
                NotifyError(e.what());
            }
        }
    }

    if (running_.exchange(false)) {
        if (channel_) channel_->Close();
        SetDisconnected();
    }
}

} // namespace mcp
