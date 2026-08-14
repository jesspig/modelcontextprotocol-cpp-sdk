// posix_platform.cpp — POSIX process and pipe implementations

#include <mcp/transport/detail/PlatformIO.hpp>
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

// 忽略 SIGPIPE 是进程级设置：向读端已关闭的管道写入时内核会发送 SIGPIPE，
// 默认动作是终止整个进程，这里改为忽略后 write 返回 EPIPE 错误码。
struct SigpipeIgnorer {
    SigpipeIgnorer() {
        struct sigaction sa = {};
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPIPE, &sa, nullptr);
    }
};

const SigpipeIgnorer kSigpipeIgnorer;

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
        if (rc <= 0 || !(pfd.revents & POLLIN)) return 0;
        ssize_t n = ::read(fd_, buffer, size);
        if (n == 0) eof_ = true;
        return n > 0 ? static_cast<size_t>(n) : 0;
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
            if (rc <= 0 || !(pfd.revents & POLLOUT)) return total;
            ssize_t n = ::write(fd_, data + total, size - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                return total;
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

    uintptr_t native_handle() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return (uintptr_t)fd_;
    }
};

class PosixProcess : public ProcessHandle {
    pid_t pid_ = -1;
public:
    explicit PosixProcess(pid_t pid) : pid_(pid) {}
    ~PosixProcess() override;

    bool IsRunning() override;
    bool Terminate(int timeout_ms) override;
    int WaitForExit(int timeout_ms) override;
    uintptr_t native_handle() const override { return (uintptr_t)pid_; }
};

PosixProcess::~PosixProcess() {
    if (pid_ > 0) {
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, WNOHANG);
    }
}

bool PosixProcess::IsRunning() {
    if (pid_ <= 0) return false;
    int status = 0;
    pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == 0) return true;   // still running
    if (result == pid_) {
        pid_ = -1;                   // reaped
        return false;
    }
    return false;
}

bool PosixProcess::Terminate(int timeout_ms) {
    if (pid_ <= 0) return true;
    kill(pid_, SIGTERM);

    int elapsed = 0;
    while (elapsed < timeout_ms) {
        if (!IsRunning()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        elapsed += 10;
    }

    // Force kill
    kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
    pid_ = -1;
    return true;
}

int PosixProcess::WaitForExit(int timeout_ms) {
    if (pid_ <= 0) return -1;

    if (timeout_ms <= 0) {
        int status = 0;
        waitpid(pid_, &status, 0);
        pid_ = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    // Poll with timeout
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int status = 0;
        pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            pid_ = -1;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        elapsed += 10;
    }
    return -1;
}

} // anonymous namespace

CreatedProcess CreateProcess(const ProcessStartInfo& info) {
    // Create pipes: stdout_pipe (child writes, parent reads)
    int stdout_pipefd[2];
    if (pipe(stdout_pipefd) < 0) {
        throw std::runtime_error("pipe creation failed for stdout");
    }

    // stdin_pipe: parent writes, child reads
    int stdin_pipefd[2];
    if (pipe(stdin_pipefd) < 0) {
        close(stdout_pipefd[0]);
        close(stdout_pipefd[1]);
        throw std::runtime_error("pipe creation failed for stdin");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipefd[0]);
        close(stdout_pipefd[1]);
        close(stdin_pipefd[0]);
        close(stdin_pipefd[1]);
        throw std::runtime_error("fork failed");
    }

    if (pid == 0) {
        // ── Child process ──
        close(stdin_pipefd[1]);   // close write end of stdin
        close(stdout_pipefd[0]);  // close read end of stdout

        dup2(stdin_pipefd[0], STDIN_FILENO);
        dup2(stdout_pipefd[1], STDOUT_FILENO);
        // stderr inherits from parent (same as Win32 behavior)

        close(stdin_pipefd[0]);
        close(stdout_pipefd[1]);

        // Change working directory if specified
        if (!info.working_directory.empty()) {
            if (chdir(info.working_directory.c_str()) != 0)
                _exit(127);
        }

        // Build argv
        std::vector<std::string> args;
        args.push_back(info.command);
        args.insert(args.end(), info.arguments.begin(), info.arguments.end());

        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);

        // Handle environment
        bool has_custom_env = !info.environment_variables.empty() || !info.inherit_environment;

        if (has_custom_env) {
            std::vector<std::string> env_strings;
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
            std::vector<char*> envp;
            for (auto& es : env_strings) envp.push_back(es.data());
            envp.push_back(nullptr);

            // execvpe is not available on macOS/BSD.
            // Search PATH manually, then use execve.
            std::string resolved = info.command;
            if (resolved.find('/') == std::string::npos) {
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
            execve(resolved.c_str(), argv.data(), envp.data());
        } else {
            execvp(info.command.c_str(), argv.data());
        }

        // If exec fails
        _exit(127);
    }

    // ── Parent process ──
    close(stdin_pipefd[0]);   // close read end of stdin
    close(stdout_pipefd[1]);  // close write end of stdout

    CreatedProcess result;
    result.process = std::make_unique<PosixProcess>(pid);
    result.stdin_pipe = std::make_unique<PosixPipe>(stdin_pipefd[1]);
    result.stdout_pipe = std::make_unique<PosixPipe>(stdout_pipefd[0]);
    // stderr_pipe: left null (inherited from parent, same as Win32 semantics)
    return result;
}

std::unique_ptr<PipeHandle> OpenStandardInput() {
    int fd = dup(STDIN_FILENO);
    return std::make_unique<PosixPipe>(fd >= 0 ? fd : -1);
}

std::unique_ptr<PipeHandle> OpenStandardOutput() {
    int fd = dup(STDOUT_FILENO);
    return std::make_unique<PosixPipe>(fd >= 0 ? fd : -1);
}

std::unique_ptr<PipeHandle> OpenStandardError() {
    int fd = dup(STDERR_FILENO);
    return std::make_unique<PosixPipe>(fd >= 0 ? fd : -1);
}

void SetThreadName(const char* name) {
    // POSIX limits thread names to 16 bytes including null terminator
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
