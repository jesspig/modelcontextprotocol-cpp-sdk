#include <process/detail/PosixProcessSupport.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace mcp { namespace process { namespace detail {

int OpenPosixRedirectFile(const std::string& path) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int descriptor = ::open(path.c_str(), flags, 0644);
    if (descriptor < 0)
        throw std::runtime_error("failed to open process output file '" + path +
                                 "': " + std::strerror(errno));
    return descriptor;
}

void ClosePosixDescriptor(int descriptor) {
    if (descriptor >= 0) ::close(descriptor);
}

void SignalPosixProcessGroup(pid_t pid, int signal) {
    if (pid <= 0) return;
    if (kill(-pid, signal) == 0) return;
    if (errno == ESRCH) kill(pid, signal);
}

bool PosixProcessGroupAlive(pid_t pid) {
    if (pid <= 0) return false;
    return kill(-pid, 0) == 0 || errno == EPERM;
}

PosixProcessExit ReapPosixProcess(pid_t pid, bool blocking) {
    int status = 0;
    pid_t result = -1;
    do {
        result = waitpid(pid, &status, blocking ? 0 : WNOHANG);
    } while (result < 0 && errno == EINTR);

    PosixProcessExit exit;
    if (result == pid) {
        exit.exited = true;
        if (WIFEXITED(status)) exit.code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) exit.code = 128 + WTERMSIG(status);
        return exit;
    }
    if (result == 0) return exit;
    if (errno == ECHILD) {
        exit.exited = true;
        return exit;
    }
    throw std::runtime_error(std::string("waitpid failed: ") + std::strerror(errno));
}

}}} // namespace mcp::process::detail
