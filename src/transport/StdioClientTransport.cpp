#include <mcp/transport/StdioClientTransport.hpp>
#include <mcp/JsonRpc.hpp>

#include <asio/post.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <thread>
#include <vector>

namespace mcp {

// JSON parse safety limits
#define K_MAX_MESSAGE_SIZE (8 * 1024 * 1024)  // 8MB
#define K_MAX_JSON_DEPTH 32

#ifdef _WIN32
namespace {

class StdioClientSessionTransport : public TransportBase {
public:
    StdioClientSessionTransport(
        std::shared_ptr<asio::io_context> io_ctx,
        PROCESS_INFORMATION process_info,
        HANDLE stdin_write,
        HANDLE stdout_read)
        : TransportBase(*io_ctx)
        , io_ctx_(std::move(io_ctx))
        , process_info_(process_info)
        , stdin_write_(stdin_write)
        , stdout_read_(stdout_read)
    {
        channel_ = std::make_unique<MessageChannel>(*io_ctx_, 64);
    }

    ~StdioClientSessionTransport() override {
        Close();
    }

    void Start() {
        running_ = true;
        read_thread_ = std::thread([this]() { ReadThread(); });
        io_thread_ = std::thread([this]() { io_ctx_->run(); });
        SetConnected();
    }

    void Close() override {
        if (!running_.exchange(false)) return;

        // Close write handle so child sees EOF on stdin
        if (stdin_write_ != INVALID_HANDLE_VALUE) {
            CloseHandle(stdin_write_);
            stdin_write_ = INVALID_HANDLE_VALUE;
        }

        // Terminate child process
        if (process_info_.hProcess) {
            TerminateProcess(process_info_.hProcess, 1);
            WaitForSingleObject(process_info_.hProcess, 5000);
            CloseHandle(process_info_.hProcess);
            process_info_.hProcess = nullptr;
        }
        if (process_info_.hThread) {
            CloseHandle(process_info_.hThread);
            process_info_.hThread = nullptr;
        }

        // Close read handle — unblocks ReadFile in the read thread
        if (stdout_read_ != INVALID_HANDLE_VALUE) {
            CloseHandle(stdout_read_);
            stdout_read_ = INVALID_HANDLE_VALUE;
        }

        if (read_thread_.joinable()) {
            read_thread_.join();
        }

        io_ctx_->stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }

        if (channel_) channel_->Close();
        SetDisconnected();
    }

    void SendMessageAsync(JsonRpcMessage message) override {
        if (!running_) return;

        nlohmann::json j = message;
        std::string line = j.dump() + "\n";

        DWORD written = 0;
        WriteFile(stdin_write_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }

private:
    void ReadThread() {
        std::string buffer;
        char buf[4096];

        while (running_) {
            DWORD bytes_read = 0;
            if (!ReadFile(stdout_read_, buf, sizeof(buf) - 1, &bytes_read, nullptr)) {
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
                    auto j = nlohmann::json::parse(line, nullptr, false, K_MAX_JSON_DEPTH);
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
    PROCESS_INFORMATION process_info_{};
    HANDLE stdin_write_ = INVALID_HANDLE_VALUE;
    HANDLE stdout_read_ = INVALID_HANDLE_VALUE;
    std::atomic<bool> running_{false};
};

} // namespace
#endif

StdioClientTransport::StdioClientTransport(const StdioClientTransportOptions& options)
    : options_(options) {}

StdioClientTransport::~StdioClientTransport() = default;

std::string_view StdioClientTransport::Name() const { return options_.name.empty() ? "stdio" : options_.name; }

std::shared_ptr<ITransport> StdioClientTransport::Connect() {
#ifdef _WIN32
    // Build command line
    std::string cmd_line = "cmd.exe /c \"" + options_.command;
    for (const auto& arg : options_.arguments) {
        cmd_line += " " + arg;
    }
    cmd_line += "\"";

    // Security attributes for pipe inheritance
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    // Create stdout pipe: child writes, we read
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        return nullptr;
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    // Create stdin pipe: we write, child reads
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        return nullptr;
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    // Startup info: redirect child's std handles
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdOutput = stdout_write;
    si.hStdInput = stdin_read;
    si.dwFlags |= STARTF_USESTDHANDLES;

    // Build environment block
    void* env_block = nullptr;
    std::string env_block_str;
    if (!options_.environment_variables.empty() || !options_.inherit_environment_variables) {
        if (options_.inherit_environment_variables) {
            LPCH cur_env = GetEnvironmentStrings();
            if (cur_env) {
                env_block_str = cur_env;
                FreeEnvironmentStrings(cur_env);
            }
        }
        for (const auto& [key, val] : options_.environment_variables) {
            env_block_str += key + "=" + val + '\0';
        }
        env_block_str += '\0';
        env_block = static_cast<void*>(env_block_str.data());
    }

    // Mutable copy of command line for CreateProcessA
    PROCESS_INFORMATION pi = {};
    std::vector<char> cmd_buf(cmd_line.begin(), cmd_line.end());
    cmd_buf.push_back('\0');

    LPCSTR lp_working_dir = options_.working_directory.empty()
        ? nullptr
        : options_.working_directory.c_str();

    BOOL success = CreateProcessA(
        nullptr,
        cmd_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        env_block,
        lp_working_dir,
        &si,
        &pi);

    CloseHandle(stdin_read);
    CloseHandle(stdout_write);

    if (!success) {
        CloseHandle(stdout_read);
        CloseHandle(stdin_write);
        return nullptr;
    }

    auto io_ctx = std::make_shared<asio::io_context>();
    auto session = std::make_shared<StdioClientSessionTransport>(
        std::move(io_ctx), pi, stdin_write, stdout_read);

    session->Start();
    return session;
#else
    return nullptr;
#endif
}

} // namespace mcp
