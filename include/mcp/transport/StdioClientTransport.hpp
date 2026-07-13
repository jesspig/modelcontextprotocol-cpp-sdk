#pragma once

#include <mcp/Transport.hpp>
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
};

class StdioClientTransport : public ClientTransport {
public:
    explicit StdioClientTransport(const StdioClientTransportOptions& options);
    ~StdioClientTransport() override;

    std::string_view Name() const override;
    std::unique_ptr<Transport> Connect() override;

private:
    StdioClientTransportOptions options_;
};

} // namespace mcp
