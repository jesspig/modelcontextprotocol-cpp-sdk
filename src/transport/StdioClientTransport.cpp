#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/McpError.hpp>
#include <mcp/transport/StdioClientTransport.hpp>
#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mcp {

namespace {

class StdioClientSessionTransport : public TransportBase {
public:
    StdioClientSessionTransport(
        std::unique_ptr<detail::ProcessHandle> process,
        std::unique_ptr<detail::PipeHandle> stdin_pipe,
        std::unique_ptr<detail::PipeHandle> stdout_pipe,
        std::unique_ptr<detail::PipeHandle> stderr_pipe,
        std::function<void(std::string_view)> on_stderr,
        std::chrono::milliseconds shutdown_timeout,
        std::optional<std::size_t> max_frame_bytes)
        : TransportBase()
        , process_(std::move(process))
        , stdin_pipe_(std::move(stdin_pipe))
        , stdout_pipe_(std::move(stdout_pipe))
        , stderr_pipe_(std::move(stderr_pipe))
        , on_stderr_(std::move(on_stderr))
        , shutdown_timeout_(shutdown_timeout)
        , max_frame_bytes_(max_frame_bytes.value_or(
            detail::kMaxStdioFrameBytes))
    {
    }

    ~StdioClientSessionTransport() override {
        Close();
    }

    void Start() override {
        running_ = true;
        read_thread_ = std::thread([this]() {
            detail::SetThreadName("mcp-worker");
            ReadThread();
        });
        if (stderr_pipe_) {
            stderr_thread_ = std::thread([this]() {
                detail::SetThreadName("mcp-stderr");
                StderrThread();
            });
        }
        SetConnected();
    }

    void Close() override {
        if (!running_.exchange(false)) return;

        if (stdin_pipe_) stdin_pipe_->Close();
        if (process_) process_->Terminate(
            static_cast<int>(shutdown_timeout_.count()));
        if (stdout_pipe_) stdout_pipe_->Close();
        if (stderr_pipe_) stderr_pipe_->Close();

        detail::JoinThreadSafely(read_thread_);
        detail::JoinThreadSafely(stderr_thread_);

        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_) return;

        auto line = SerializeMessage(std::move(message)) + "\n";
        if (!stdin_pipe_) return;
        if (!detail::WriteWholeMessage(*stdin_pipe_, line.data(), line.size())) {
            MCP_LOG(Error, "failed to write complete message to child stdin pipe");
            NotifyError("failed to write complete message to child stdin pipe");
            Close();
        }
    }

private:
    void StderrThread() {
        char buffer[detail::kReadBufferSize];
        while (running_) {
            size_t bytes_read = 0;
            try {
                bytes_read = stderr_pipe_->Read(buffer, sizeof(buffer));
            } catch (const std::exception& e) {
                MCP_LOG(Error, std::string("stdio stderr thread failed: ") + e.what());
                break;
            }
            if (bytes_read == 0) {
                if (stderr_pipe_->IsEof()) break;
                continue;
            }
            if (!on_stderr_) continue;
            try {
                on_stderr_(std::string_view(buffer, bytes_read));
            } catch (const std::exception& e) {
                MCP_LOG(Error, std::string("stdio stderr callback threw: ") + e.what());
            } catch (...) {
                MCP_LOG(Error, "stdio stderr callback threw an unknown exception");
            }
        }
    }

    void ReadThread() {
        std::string buffer;
        char buf[detail::kReadBufferSize];

        while (running_) {
            size_t bytes_read = 0;
            try {
                bytes_read = stdout_pipe_->Read(buf, sizeof(buf) - 1);
            } catch (const std::exception& e) {
                MCP_LOG(Error, std::string("stdio read thread failed: ") + e.what());
                break;
            }
            if (bytes_read == 0) {
                if (stdout_pipe_->IsEof()) break;
                continue;
            }

            buf[bytes_read] = '\0';
            buffer.append(buf, bytes_read);

            if (buffer.size() > max_frame_bytes_) {
                buffer.clear();
                NotifyError("stdio frame size exceeds maximum allowed size");
                break;
            }

            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);

                if (line.empty()) continue;

                if (line.size() > max_frame_bytes_) {
                    NotifyError("stdio frame size exceeds maximum allowed size");
                    continue;
                }

                try {
                    auto msg = DeserializeMessage(line);
                    if (channel_ && !channel_->Send(std::move(msg)))
                        MCP_LOG(Warning, "message dropped: channel closed");
                } catch (const std::exception& e) {
                    NotifyError(e.what());
                }
            }
        }
    }

    std::thread read_thread_;
    std::thread stderr_thread_;
    std::unique_ptr<detail::ProcessHandle> process_;
    std::unique_ptr<detail::PipeHandle> stdin_pipe_;
    std::unique_ptr<detail::PipeHandle> stdout_pipe_;
    std::unique_ptr<detail::PipeHandle> stderr_pipe_;
    std::function<void(std::string_view)> on_stderr_;
    std::chrono::milliseconds shutdown_timeout_;
    std::size_t max_frame_bytes_;
    std::atomic<bool> running_{false};
};

} // namespace

StdioClientTransport::StdioClientTransport(const StdioClientTransportOptions& options)
    : options_(options) {}

StdioClientTransport::~StdioClientTransport() = default;

std::string_view StdioClientTransport::Name() const { return options_.name.empty() ? std::string_view{"stdio"} : options_.name; }

std::shared_ptr<ITransport> StdioClientTransport::Connect() {
    if (!options_.stderr_path.empty() && options_.on_stderr) {
        throw McpError(McpErrorCode::InvalidParams,
                       "stderr_path and on_stderr are mutually exclusive");
    }

    detail::ProcessStartInfo info;
    info.command = options_.command;
    info.arguments = options_.arguments;
    info.working_directory = options_.working_directory;
    info.inherit_environment = options_.inherit_environment_variables;
    for (const auto& [key, val] : options_.environment_variables) {
        info.environment_variables.emplace_back(key, val);
    }
    info.stderr_path = options_.stderr_path;
    info.capture_stderr = static_cast<bool>(options_.on_stderr);
    info.terminate_process_tree = options_.terminate_process_tree;

    auto created = detail::CreateProcess(info);
    pid_ = created.process->Pid();

    auto session = std::make_shared<StdioClientSessionTransport>(
        std::move(created.process),
        std::move(created.stdin_pipe),
        std::move(created.stdout_pipe),
        std::move(created.stderr_pipe),
        options_.on_stderr,
        options_.shutdown_timeout,
        options_.max_frame_bytes);

    session->Start();
    return session;
}

std::optional<int64_t> StdioClientTransport::Pid() const {
    return pid_;
}

} // namespace mcp
