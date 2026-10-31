#pragma once

#include <optional>
#include <string>

#include <sys/types.h>

namespace mcp { namespace process { namespace detail {

int OpenPosixRedirectFile(const std::string& path);
void ClosePosixDescriptor(int descriptor);
void SignalPosixProcessGroup(pid_t pid, int signal);
bool PosixProcessGroupAlive(pid_t pid);

struct PosixProcessExit {
    bool exited = false;
    std::optional<int> code;
};

PosixProcessExit ReapPosixProcess(pid_t pid, bool blocking);

}}} // namespace mcp::process::detail
