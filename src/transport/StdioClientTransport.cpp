// StdioClientTransport.cpp — stdio client transport implementation

#include <mcp/detail/ThreadUtils.hpp>
#include <mcp/transport/StdioClientTransport.hpp>
#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <mcp/JsonRpc.hpp>
#include <mcp/Log.hpp>

#include <atomic>
#include <exception>
#include <thread>
#include <vector>

namespace mcp {

namespace {

class StdioClientSessionTransport : public TransportBase {
public:
    StdioClientSessionTransport(
        std::unique_ptr<detail::ProcessHandle> process,
        std::unique_ptr<detail::PipeHandle> stdin_pipe,
        std::unique_ptr<detail::PipeHandle> stdout_pipe)
        : TransportBase()
        , process_(std::move(process))
        , stdin_pipe_(std::move(stdin_pipe))
        , stdout_pipe_(std::move(stdout_pipe))
    {
    }

    ~StdioClientSessionTransport() override {
        Close();
    }

    void Start() {
        running_ = true;
        read_thread_ = std::thread([this]() {
            detail::SetThreadName("mcp-worker");
            ReadThread();
        });
        SetConnected();
    }

    void Close() override {
        if (!running_.exchange(false)) return;

        if (stdin_pipe_) stdin_pipe_->Close();
        if (process_) process_->Terminate(5000);
        if (stdout_pipe_) stdout_pipe_->Close();

        detail::JoinThreadSafely(read_thread_);

        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_) return;

        auto line = SerializeMessage(std::move(message)) + "\n";
        if (stdin_pipe_ && stdin_pipe_->Write(line.data(), line.size()) != line.size()) {
            MCP_LOG(Error, "failed to write to child stdin pipe");
            NotifyError("failed to write to child stdin pipe");
        }
    }

private:
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
                    NotifyError(e.what());
                }
            }
        }
    }

    std::thread read_thread_;
    std::unique_ptr<detail::ProcessHandle> process_;
    std::unique_ptr<detail::PipeHandle> stdin_pipe_;
    std::unique_ptr<detail::PipeHandle> stdout_pipe_;
    std::atomic<bool> running_{false};
};

} // namespace

StdioClientTransport::StdioClientTransport(const StdioClientTransportOptions& options)
    : options_(options) {}

StdioClientTransport::~StdioClientTransport() = default;

std::string_view StdioClientTransport::Name() const { return options_.name.empty() ? std::string_view{"stdio"} : options_.name; }

std::shared_ptr<ITransport> StdioClientTransport::Connect() {
    detail::ProcessStartInfo info;
    info.command = options_.command;
    info.arguments = options_.arguments;
    info.working_directory = options_.working_directory;
    info.inherit_environment = options_.inherit_environment_variables;
    for (const auto& [key, val] : options_.environment_variables) {
        info.environment_variables.emplace_back(key, val);
    }

    auto created = detail::CreateProcess(info);

    auto session = std::make_shared<StdioClientSessionTransport>(
        std::move(created.process),
        std::move(created.stdin_pipe),
        std::move(created.stdout_pipe));

    session->Start();
    return session;
}

} // namespace mcp
