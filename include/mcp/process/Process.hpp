#pragma once

#include <mcp/Export.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

struct MCP_API SpawnOptions {
    std::string command;
    std::vector<std::string> arguments;
    std::string working_directory;
    std::map<std::string, std::string> environment;
    bool inherit_environment = true;
    std::string stdout_path;
    std::string stderr_path;
    bool detached = false;
};

class MCP_API SpawnedProcess {
public:
    virtual ~SpawnedProcess() = default;

    virtual int64_t Pid() const = 0;
    virtual bool IsRunning() const = 0;
    virtual bool Wait(int timeout_ms) = 0;
    virtual std::optional<int> ExitCode() const = 0;
    virtual bool Terminate(int grace_ms) = 0;
};

MCP_API std::unique_ptr<SpawnedProcess> Spawn(const SpawnOptions& options);

} // namespace mcp
