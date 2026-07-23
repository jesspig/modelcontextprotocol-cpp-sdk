#include <mcp/transport/StdioClientTransport.hpp>
#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/JsonRpc.hpp>

#include <atomic>
#include <thread>
#include <vector>

namespace mcp {

// JSON parse safety limits
#define K_MAX_MESSAGE_SIZE (8 * 1024 * 1024)  // 8MB
// K_MAX_JSON_DEPTH removed — nlohmann-json v3.11.3 parse() accepts 4 args max

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

        if (read_thread_.joinable()) read_thread_.join();

        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_) return;

        auto line = SerializeMessage(message) + "\n";
        if (stdin_pipe_) stdin_pipe_->Write(line.data(), line.size());
    }

private:
    void ReadThread() {
        std::string buffer;
        char buf[4096];

        while (running_) {
            size_t bytes_read = 0;
            try {
                bytes_read = stdout_pipe_->Read(buf, sizeof(buf) - 1);
            } catch (...) {
                break;
            }
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
    if (!created.process) return nullptr;

    auto session = std::make_shared<StdioClientSessionTransport>(
        std::move(created.process),
        std::move(created.stdin_pipe),
        std::move(created.stdout_pipe));

    session->Start();
    return session;
}

} // namespace mcp
