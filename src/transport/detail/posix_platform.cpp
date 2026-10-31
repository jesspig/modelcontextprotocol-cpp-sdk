#include <mcp/transport/detail/PlatformIO.hpp>
#include <mcp/McpError.hpp>
#include <mcp/ErrorCodes.hpp>
#include <process/detail/PosixProcessSupport.hpp>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <exception>
#include <string>
#ifdef __APPLE__
#include <crt_externs.h>
#endif
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>

namespace mcp { namespace detail {

namespace {

struct SigpipeIgnorer {
    SigpipeIgnorer() {
        struct sigaction sa = {};
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPIPE, &sa, nullptr);
    }
};

const SigpipeIgnorer kSigpipeIgnorer;

void SetCloseOnExec(int fd) {
    if (fd < 0) return;
    int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

constexpr int kPipeReadPollTimeoutMs = 100;

class PosixPipe : public PipeHandle {
    mutable std::mutex mutex_;
    int fd_ = -1;
    bool eof_ = false;
    std::atomic<bool> closed_{false};
public:
    PosixPipe(int fd) : fd_(fd) {}
    ~PosixPipe() override { Close(); }

    size_t Read(char* buffer, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || fd_ < 0) return 0;
        struct pollfd pfd = { fd_, POLLIN, 0 };
        int rc;
        do { rc = ::poll(&pfd, 1, kPipeReadPollTimeoutMs); } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
            eof_ = true;
            throw McpError(McpErrorCode::ConnectionClosed,
                           std::string("pipe poll failed: ") + std::strerror(errno));
        }
        if (rc == 0) return 0;
        if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
            eof_ = true;
            throw McpError(McpErrorCode::ConnectionClosed,
                           "pipe read failed: poll reported an unrecoverable condition");
        }
        if ((pfd.revents & (POLLIN | POLLHUP)) == 0) {
            eof_ = true;
            return 0;
        }
        ssize_t n = ::read(fd_, buffer, size);
        if (n < 0) {
            if (errno == EINTR) return 0;
            eof_ = true;
            throw McpError(McpErrorCode::ConnectionClosed,
                           std::string("pipe read failed: ") + std::strerror(errno));
        }
        if (n == 0) {
            eof_ = true;
            return 0;
        }
        return static_cast<size_t>(n);
    }

    bool IsEof() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ || fd_ < 0 || eof_;
    }

    size_t Write(const char* data, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || fd_ < 0) return 0;
        size_t total = 0;
        while (total < size && !closed_) {
            struct pollfd pfd = { fd_, POLLOUT, 0 };
            int rc;
            do { rc = ::poll(&pfd, 1, kPipeReadPollTimeoutMs); } while (rc < 0 && errno == EINTR);
            if (rc < 0) {
                throw McpError(McpErrorCode::ConnectionClosed,
                               std::string("pipe poll failed: ") + std::strerror(errno));
            }
            if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
                throw McpError(McpErrorCode::ConnectionClosed,
                               "pipe write failed: poll reported an unrecoverable condition");
            }
            if (rc == 0 || !(pfd.revents & POLLOUT)) return total;
            ssize_t n = ::write(fd_, data + total, size - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw McpError(McpErrorCode::ConnectionClosed,
                               std::string("pipe write failed: ") + std::strerror(errno));
            }
            if (n == 0) return total;
            total += static_cast<size_t>(n);
        }
        return total;
    }

    void Close() override {
        closed_ = true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
};

class PosixProcess : public ProcessHandle {
    pid_t pid_ = -1;
    bool tree_ = false;
public:
    PosixProcess(pid_t pid, bool tree) : pid_(pid), tree_(tree) {}
    ~PosixProcess() override;

    bool IsRunning() override;
    bool Terminate(int timeout_ms) override;
    int64_t Pid() const override { return static_cast<int64_t>(pid_); }

private:
    void Signal(int signal_number);
};

PosixProcess::~PosixProcess() {
    if (pid_ <= 0) return;
    Signal(SIGKILL);
    try { process::detail::ReapPosixProcess(pid_, true); } catch (...) {}
}

void PosixProcess::Signal(int signal_number) {
    if (tree_) process::detail::SignalPosixProcessGroup(pid_, signal_number);
    else if (pid_ > 0) kill(pid_, signal_number);
}

bool PosixProcess::IsRunning() {
    if (pid_ <= 0) return false;
    process::detail::PosixProcessExit exit;
    try {
        exit = process::detail::ReapPosixProcess(pid_, false);
    } catch (const std::runtime_error&) {
        return false;
    }
    if (exit.exited) {
        pid_ = -1;
        return false;
    }
    return true;
}

bool PosixProcess::Terminate(int timeout_ms) {
    if (pid_ <= 0) return true;
    Signal(SIGTERM);

    int elapsed = 0;
    while (elapsed < timeout_ms) {
        if (!IsRunning()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        elapsed += 10;
    }

    Signal(SIGKILL);
    try { process::detail::ReapPosixProcess(pid_, true); } catch (...) {}
    pid_ = -1;
    return true;
}

} // anonymous namespace

bool WriteWholeMessage(PipeHandle& pipe, const char* data, size_t size) {
    constexpr int kMaxIdleAttempts = 64;
    constexpr std::chrono::milliseconds kIdleDelay(5);
    size_t total = 0;
    int idle_attempts = 0;
    while (total < size) {
        size_t written = 0;
        try {
            written = pipe.Write(data + total, size - total);
        } catch (const std::exception&) {
            return false;
        }
        if (written == 0) {
            if (++idle_attempts > kMaxIdleAttempts) return false;
            std::this_thread::sleep_for(kIdleDelay);
            continue;
        }
        idle_attempts = 0;
        total += written;
    }
    return true;
}

CreatedProcess CreateProcess(const ProcessStartInfo& info) {
    int stdout_pipefd[2];
    if (pipe(stdout_pipefd) < 0) {
        throw std::runtime_error("pipe creation failed for stdout");
    }
    SetCloseOnExec(stdout_pipefd[0]);
    SetCloseOnExec(stdout_pipefd[1]);

    int stdin_pipefd[2];
    if (pipe(stdin_pipefd) < 0) {
        close(stdout_pipefd[0]);
        close(stdout_pipefd[1]);
        throw std::runtime_error("pipe creation failed for stdin");
    }
    SetCloseOnExec(stdin_pipefd[0]);
    SetCloseOnExec(stdin_pipefd[1]);

    int stderr_pipefd[2] = { -1, -1 };
    if (info.capture_stderr) {
        if (pipe(stderr_pipefd) < 0) {
            close(stdout_pipefd[0]);
            close(stdout_pipefd[1]);
            close(stdin_pipefd[0]);
            close(stdin_pipefd[1]);
            throw std::runtime_error("pipe creation failed for stderr");
        }
        SetCloseOnExec(stderr_pipefd[0]);
        SetCloseOnExec(stderr_pipefd[1]);
    }

    int stderr_descriptor = -1;
    try {
        if (!info.capture_stderr && !info.stderr_path.empty())
            stderr_descriptor = process::detail::OpenPosixRedirectFile(info.stderr_path);
    } catch (...) {
        close(stdout_pipefd[0]);
        close(stdout_pipefd[1]);
        close(stdin_pipefd[0]);
        close(stdin_pipefd[1]);
        if (stderr_pipefd[0] >= 0) close(stderr_pipefd[0]);
        if (stderr_pipefd[1] >= 0) close(stderr_pipefd[1]);
        throw;
    }

    std::vector<std::string> args;
    args.push_back(info.command);
    args.insert(args.end(), info.arguments.begin(), info.arguments.end());

    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    bool has_custom_env = !info.environment_variables.empty() || !info.inherit_environment;
    std::vector<std::string> env_strings;
    std::vector<char*> envp;
    if (has_custom_env) {
        if (info.inherit_environment) {
#ifdef __APPLE__
            for (char** e = *_NSGetEnviron(); *e; ++e) {
#else
            for (char** e = ::environ; *e; ++e) {
#endif
                env_strings.push_back(*e);
            }
        }
        for (const auto& [key, val] : info.environment_variables) {
            env_strings.push_back(key + "=" + val);
        }
        for (auto& es : env_strings) envp.push_back(es.data());
        envp.push_back(nullptr);
    }

    std::string resolved = info.command;
    if (has_custom_env && resolved.find('/') == std::string::npos) {
        const char* path_env = getenv("PATH");
        if (path_env) {
            std::string path(path_env);
            size_t start = 0, end;
            while ((end = path.find(':', start)) != std::string::npos) {
                std::string dir = path.substr(start, end - start);
                if (dir.empty()) dir = ".";
                std::string candidate = dir + "/" + info.command;
                if (access(candidate.c_str(), X_OK) == 0) {
                    resolved = candidate;
                    break;
                }
                start = end + 1;
            }
            if (resolved == info.command) {
                std::string dir = path.substr(start);
                if (dir.empty()) dir = ".";
                std::string candidate = dir + "/" + info.command;
                if (access(candidate.c_str(), X_OK) == 0)
                    resolved = candidate;
            }
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipefd[0]);
        close(stdout_pipefd[1]);
        close(stdin_pipefd[0]);
        close(stdin_pipefd[1]);
        if (stderr_pipefd[0] >= 0) close(stderr_pipefd[0]);
        if (stderr_pipefd[1] >= 0) close(stderr_pipefd[1]);
        if (stderr_descriptor >= 0) close(stderr_descriptor);
        throw std::runtime_error("fork failed");
    }

    if (pid == 0) {
        close(stdin_pipefd[1]);
        close(stdout_pipefd[0]);

        dup2(stdin_pipefd[0], STDIN_FILENO);
        dup2(stdout_pipefd[1], STDOUT_FILENO);

        close(stdin_pipefd[0]);
        close(stdout_pipefd[1]);

        if (info.capture_stderr) {
            close(stderr_pipefd[0]);
            dup2(stderr_pipefd[1], STDERR_FILENO);
            close(stderr_pipefd[1]);
        } else if (stderr_descriptor >= 0) {
            dup2(stderr_descriptor, STDERR_FILENO);
            close(stderr_descriptor);
        }

        if (info.terminate_process_tree) setpgid(0, 0);

        if (!info.working_directory.empty()) {
            if (chdir(info.working_directory.c_str()) != 0)
                _exit(127);
        }

        if (has_custom_env)
            execve(resolved.c_str(), argv.data(), envp.data());
        else
            execvp(info.command.c_str(), argv.data());

        _exit(127);
    }

    close(stdin_pipefd[0]);
    close(stdout_pipefd[1]);
    if (info.capture_stderr) close(stderr_pipefd[1]);
    if (stderr_descriptor >= 0) close(stderr_descriptor);

    CreatedProcess result;
    result.process = std::make_unique<PosixProcess>(pid, info.terminate_process_tree);
    result.stdin_pipe = std::make_unique<PosixPipe>(stdin_pipefd[1]);
    result.stdout_pipe = std::make_unique<PosixPipe>(stdout_pipefd[0]);
    if (info.capture_stderr)
        result.stderr_pipe = std::make_unique<PosixPipe>(stderr_pipefd[0]);
    return result;
}

std::unique_ptr<PipeHandle> OpenStandardInput() {
    int fd = dup(STDIN_FILENO);
    SetCloseOnExec(fd);
    return std::make_unique<PosixPipe>(fd >= 0 ? fd : -1);
}

std::unique_ptr<PipeHandle> OpenStandardOutput() {
    int fd = dup(STDOUT_FILENO);
    SetCloseOnExec(fd);
    return std::make_unique<PosixPipe>(fd >= 0 ? fd : -1);
}

std::unique_ptr<PipeHandle> CreatePipeFromFd(int fd) {
    return std::make_unique<PosixPipe>(fd);
}

void SetThreadName(const char* name) {
    char truncated[16];
    std::strncpy(truncated, name, sizeof(truncated) - 1);
    truncated[sizeof(truncated) - 1] = '\0';
#ifdef __APPLE__
    pthread_setname_np(truncated);
#else
    pthread_setname_np(pthread_self(), truncated);
#endif
}

}} // namespace mcp::detail
