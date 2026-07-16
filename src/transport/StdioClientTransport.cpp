#include <mcp/transport/StdioClientTransport.hpp>
#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/JsonRpc.hpp>

#include <asio/post.hpp>
#include <nlohmann/json.hpp>

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
        std::shared_ptr<asio::io_context> io_ctx,
        std::unique_ptr<detail::ProcessHandle> process,
        std::unique_ptr<detail::PipeHandle> stdin_pipe,
        std::unique_ptr<detail::PipeHandle> stdout_pipe)
        : TransportBase(*io_ctx)
        , io_ctx_(std::move(io_ctx))
        , process_(std::move(process))
        , stdin_pipe_(std::move(stdin_pipe))
        , stdout_pipe_(std::move(stdout_pipe))
    {
        channel_ = std::make_unique<MessageChannel>(*io_ctx_, 64);
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
        io_thread_ = std::thread([this]() {
            detail::SetThreadName("mcp-worker");
            io_ctx_->run();
        });
        SetConnected();
    }

    void Close() override {
        if (!running_.exchange(false)) return;

        if (stdin_pipe_) stdin_pipe_->Close();
        if (process_) process_->Terminate(5000);
        if (stdout_pipe_) stdout_pipe_->Close();

        if (read_thread_.joinable()) read_thread_.join();
        io_ctx_->stop();
        if (io_thread_.joinable()) io_thread_.join();

        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_) return;

        nlohmann::json j = message;
        std::string line = j.dump() + "\n";
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
                    auto j = nlohmann::json::parse(line, nullptr, false, false);
                    JsonRpcMessage msg = j.get<JsonRpcMessage>();
                    asio::post(*io_ctx_, [this, msg = std::move(msg)]() {
                        if (channel_) channel_->Send(std::move(msg));
                    });
                } catch (const std::exception& e) {
                    NotifyError(e.what());
                }
            }
        }
    }

    std::shared_ptr<asio::io_context> io_ctx_;
    std::thread read_thread_;
    std::thread io_thread_;
    std::unique_ptr<detail::ProcessHandle> process_;
    std::unique_ptr<detail::PipeHandle> stdin_pipe_;
    std::unique_ptr<detail::PipeHandle> stdout_pipe_;
    std::atomic<bool> running_{false};
};

} // namespace

StdioClientTransport::StdioClientTransport(const StdioClientTransportOptions& options)
    : options_(options) {}

StdioClientTransport::~StdioClientTransport() = default;

std::string_view StdioClientTransport::Name() const { return options_.name.empty() ? "stdio" : options_.name; }

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

    auto io_ctx = std::make_shared<asio::io_context>();
    auto session = std::make_shared<StdioClientSessionTransport>(
        std::move(io_ctx),
        std::move(created.process),
        std::move(created.stdin_pipe),
        std::move(created.stdout_pipe));

    session->Start();
    return session;
}

} // namespace mcp
