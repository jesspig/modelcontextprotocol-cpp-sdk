#pragma once

#include <mcp/Transport.hpp>
#include <mcp/transport/detail/Limits.hpp>
#include <cstddef>
#include <optional>
#include <string>
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
    std::optional<std::size_t> max_frame_bytes = detail::kMaxStdioFrameBytes;
};

class StdioClientTransport : public IClientTransport {
public:
    explicit StdioClientTransport(const StdioClientTransportOptions& options);
    ~StdioClientTransport() override;

    std::string_view Name() const override;
    std::shared_ptr<ITransport> Connect() override;

private:
    StdioClientTransportOptions options_;
};

} // namespace mcp
