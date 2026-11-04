#include <mcp/Implementation.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Transport.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/process/Process.hpp>
#include <mcp/transport/StdioClientTransport.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace mcp;

namespace manage_subprocesses {

// region spawn-plain
std::unique_ptr<SpawnedProcess> StartHelper() {
    SpawnOptions options;
    options.command = "mcp-echo-server";
    options.arguments = {"--stdio"};
    options.working_directory = "build/debug/bin";
    options.environment = {{"MCP_LOG_LEVEL", "2"}};
    options.inherit_environment = false;
    options.stderr_path = "helper.stderr.log";
    options.detached = false;
    return Spawn(options);
}

bool DrainWithin(std::unique_ptr<SpawnedProcess>& process, int timeout_ms) {
    if (process->Wait(timeout_ms)) {
        std::optional<int> code = process->ExitCode();
        return code.has_value() && *code == 0;
    }
    return process->Terminate(300);
}
// endregion

// region stdio-options
StdioClientTransportOptions TransportOptions() {
    StdioClientTransportOptions options;
    options.command = "mcp-echo-server";
    options.arguments = {"--stdio", "--profile", "conformance"};
    options.name = "echo";
    options.working_directory = "build/debug/bin";
    options.inherit_environment_variables = false;
    options.environment_variables = {{"MCP_LOG_LEVEL", "3"}};
    options.shutdown_timeout = std::chrono::milliseconds(2000);
    options.terminate_process_tree = true;
    return options;
}
// endregion

// region stderr-file
StdioClientTransportOptions RedirectStderrToFile() {
    StdioClientTransportOptions options = TransportOptions();
    options.stderr_path = "echo-server.stderr.log";
    options.on_stderr = nullptr;
    return options;
}
// endregion

// region stderr-drain
class StderrSink {
public:
    void Push(std::string_view line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lines_.size() >= kKeep) lines_.pop_front();
        lines_.emplace_back(line);
    }

    std::vector<std::string> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<std::string>(lines_.begin(), lines_.end());
    }

private:
    static constexpr std::size_t kKeep = 64;
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
};

std::shared_ptr<StderrSink> AttachStderr(StdioClientTransportOptions& options) {
    auto sink = std::make_shared<StderrSink>();
    options.on_stderr = [sink](std::string_view line) {
        sink->Push(line);
    };
    return sink;
}
// endregion

// region connect-and-pid
std::unique_ptr<McpClient> ConnectEcho(
    StdioClientTransport& transport, const std::shared_ptr<StderrSink>& sink) {
    std::shared_ptr<ITransport> session = transport.Connect();
    ClientOptions options;
    options.client_info = Implementation{"release-bot", "1.4.0"};
    (void)sink;
    return McpClient::Create(std::move(session), options);
}

std::optional<int64_t> ChildPid(const StdioClientTransport& transport) {
    return transport.Pid();
}
// endregion

// region shutdown-order
void ShutdownInOrder(
    std::unique_ptr<McpClient>& client,
    StdioClientTransport& transport) {
    client->Close();
    client.reset();
    const std::optional<int64_t> pid = transport.Pid();
    (void)pid;
}
// endregion

// region watchdog
bool StillHealthy(const StdioClientTransport& transport) {
    std::optional<int64_t> pid = transport.Pid();
    return pid.has_value() && *pid > 0;
}
// endregion

// region full
std::unique_ptr<McpClient> StartEchoServer(std::shared_ptr<StdioClientTransport>& out_transport) {
    StdioClientTransportOptions options = TransportOptions();
    auto sink = AttachStderr(options);

    auto transport = std::make_shared<StdioClientTransport>(options);
    auto client = ConnectEcho(*transport, sink);
    out_transport = transport;
    return client;
}
// endregion

} // namespace manage_subprocesses
