// StdioServerTransport.cpp — stdio server transport implementation

#include <mcp/transport/StdioServerTransport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>

namespace mcp {

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
    if (running_.exchange(false)) {
        // Closing the pipes unblocks the read thread's blocking Read, so join completes
        if (stdin_pipe_) stdin_pipe_->Close();
        if (stdout_pipe_) stdout_pipe_->Close();
    }

    // Join unconditionally: the read thread may have exited on its own (EOF/error)
    detail::JoinThreadSafely(read_thread_);

    if (channel_) channel_->Close();
    SetDisconnected();
}

void StdioServerTransport::SendMessageAsync(JsonRpcMessage message) {
    if (!running_) return;

    auto line = SerializeMessage(std::move(message)) + "\n";

    if (stdout_pipe_ && stdout_pipe_->Write(line.data(), line.size()) != line.size()) {
        MCP_LOG(Error, "failed to write to stdout pipe");
        NotifyError("failed to write to stdout pipe");
    }
}

void StdioServerTransport::ReadLoop() {
    std::string buffer;

    while (running_) {
        char buf[detail::kReadBufferSize];

        size_t bytes_read = stdin_pipe_->Read(buf, sizeof(buf) - 1);
        if (bytes_read == 0) {
            if (stdin_pipe_->IsEof()) break;
            continue;
        }

        buf[bytes_read] = '\0';
        buffer.append(buf, bytes_read);

        if (buffer.size() > detail::kMaxMessageSize) {
            buffer.clear();
            NotifyError("message size exceeds maximum allowed size");
            break;
        }

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            if (line.size() > detail::kMaxMessageSize) {
                NotifyError("message size exceeds maximum allowed size");
                continue;
            }

            try {
                auto msg = DeserializeMessage(line);
                if (channel_ && !channel_->Send(std::move(msg)))
                    MCP_LOG(Warning, "message dropped: channel closed");
            } catch (const std::exception& e) {
                MCP_LOG(Error, std::string("stdio parse error: ") + e.what());
                NotifyError(e.what());
            }
        }
    }

    // The read thread exited on its own (EOF/error/oversize); leave running_
    // untouched so Close() still performs the full teardown and joins us.
    if (channel_) channel_->Close();
    SetDisconnected();
}

} // namespace mcp
