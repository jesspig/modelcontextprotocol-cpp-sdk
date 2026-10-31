#pragma once

#include <mcp/Transport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <map>

namespace mcp {

struct StdioClientTransportOptions {
    std::string command;
    std::vector<std::string> arguments;
    std::string name;
    std::string working_directory;
    bool inherit_environment_variables = true;
    std::map<std::string, std::string> environment_variables;
    std::string stderr_path;
    std::function<void(std::string_view)> on_stderr;
    std::chrono::milliseconds shutdown_timeout{2000};
    bool terminate_process_tree = false;
    std::optional<std::size_t> max_frame_bytes = detail::kMaxStdioFrameBytes;
};

class StdioClientTransport : public IClientTransport {
public:
    explicit StdioClientTransport(const StdioClientTransportOptions& options);
    ~StdioClientTransport() override;

    std::string_view Name() const override;
    std::shared_ptr<ITransport> Connect() override;
    std::optional<int64_t> Pid() const;

private:
    StdioClientTransportOptions options_;
    std::optional<int64_t> pid_;
};

} // namespace mcp
