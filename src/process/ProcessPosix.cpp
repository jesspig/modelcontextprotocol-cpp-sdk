#include <mcp/process/Process.hpp>
#include <mcp/McpError.hpp>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <crt_externs.h>
#endif

#if defined(__APPLE__)
#  define MCP_PROCESS_HAS_SPAWN_CHDIR 1
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
#  define MCP_PROCESS_HAS_SPAWN_CHDIR 1
#else
#  define MCP_PROCESS_HAS_SPAWN_CHDIR 0
#endif

namespace mcp {

namespace {

constexpr int kWaitPollIntervalMs = 10;

void ValidateOptions(const SpawnOptions& options) {
    if (options.command.empty())
        throw McpError(McpErrorCode::InvalidParams, "spawn command must not be empty");
    for (const auto& entry : options.environment) {
        if (entry.first.empty() || entry.first.find('=') != std::string::npos)
            throw McpError(McpErrorCode::InvalidParams,
                           "spawn environment key must be non-empty and must not contain '='");
    }
}

char** InheritedEnvironment() {
#ifdef __APPLE__
    return *_NSGetEnviron();
#else
    return ::environ;
#endif
}

std::string DefaultPath() {
#ifdef _CS_PATH
    char buffer[4096] = {};
    std::size_t length = confstr(_CS_PATH, buffer, sizeof(buffer));
    if (length > 0 && length < sizeof(buffer)) return std::string(buffer, length - 1);
#endif
    return "/bin:/usr/bin";
}

std::string EffectivePath(const SpawnOptions& options) {
    auto override_entry = options.environment.find("PATH");
    if (override_entry != options.environment.end()) return override_entry->second;
    if (options.inherit_environment) {
        const char* inherited = std::getenv("PATH");
        if (inherited != nullptr) return inherited;
    }
    return DefaultPath();
}

std::string ResolveCommand(const SpawnOptions& options) {
    if (options.command.find('/') != std::string::npos) return options.command;
    const std::string path = EffectivePath(options);
    std::size_t start = 0;
    while (true) {
        std::size_t end = path.find(':', start);
        std::string directory = path.substr(start, end == std::string::npos
            ? std::string::npos
            : end - start);
        if (directory.empty()) directory = ".";
        std::string candidate = directory + "/" + options.command;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return options.command;
}

void AppendEnvironment(const SpawnOptions& options, std::vector<std::string>& entries) {
    if (options.inherit_environment) {
        for (char** entry = InheritedEnvironment(); entry != nullptr && *entry != nullptr; ++entry) {
            std::string inherited(*entry);
            std::size_t separator = inherited.find('=');
            std::string name = separator == std::string::npos
                ? inherited
                : inherited.substr(0, separator);
            if (options.environment.find(name) == options.environment.end())
                entries.push_back(std::move(inherited));
        }
    }
    for (const auto& [key, value] : options.environment)
        entries.push_back(key + "=" + value);
}

int OpenRedirectFile(const std::string& path) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int descriptor = ::open(path.c_str(), flags, 0644);
    if (descriptor < 0)
        throw McpError(McpErrorCode::InternalError,
                       "failed to open spawn output file '" + path + "': " + std::strerror(errno));
    return descriptor;
}

void CloseDescriptor(int descriptor) {
    if (descriptor >= 0) ::close(descriptor);
}

void SignalProcessGroup(pid_t pid, int signal) {
    if (kill(-pid, signal) == 0) return;
    if (errno == ESRCH) kill(pid, signal);
}

bool ProcessGroupAlive(pid_t pid) {
    return kill(-pid, 0) == 0 || errno == EPERM;
}

class PosixSpawnedProcess : public SpawnedProcess {
public:
    PosixSpawnedProcess(pid_t pid, bool detached) : pid_(pid), detached_(detached) {}

    ~PosixSpawnedProcess() override {
        pid_t pid = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exited_ || pid_ <= 0) return;
            pid = pid_;
        }
        if (detached_) {
            std::thread([pid] {
                int status = 0;
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            }).detach();
            return;
        }
        kill(-pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }

    int64_t Pid() const override { return static_cast<int64_t>(pid_); }

    bool IsRunning() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (exited_) return false;
        TryReapLocked();
        return !exited_;
    }

    bool Wait(int timeout_ms) override {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            if (exited_) return true;
            TryReapLocked();
            if (exited_) return true;
            long long sleep_ms = kWaitPollIntervalMs;
            if (timeout_ms >= 0) {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0) return false;
                if (remaining < sleep_ms) sleep_ms = remaining;
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            lock.lock();
        }
    }

    std::optional<int> ExitCode() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!exited_) TryReapLocked();
        return exit_code_;
    }

    bool Terminate(int grace_ms) override {
        pid_t pid = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exited_) return true;
            pid = pid_;
        }
        if (grace_ms > 0) {
            SignalProcessGroup(pid, SIGTERM);
            if (Wait(grace_ms)) {
                if (ProcessGroupAlive(pid)) SignalProcessGroup(pid, SIGKILL);
                return true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exited_) return true;
        }
        SignalProcessGroup(pid, SIGKILL);
        return Wait(-1);
    }

private:
    void TryReapLocked() const {
        int status = 0;
        pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            exited_ = true;
            if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
            return;
        }
        if (result == 0) return;
        if (errno == EINTR) return;
        if (errno == ECHILD) {
            exited_ = true;
            return;
        }
        throw McpError(McpErrorCode::InternalError,
                       std::string("waitpid failed for spawned process: ") + std::strerror(errno));
    }

    mutable std::mutex mutex_;
    pid_t pid_ = -1;
    bool detached_ = false;
    mutable bool exited_ = false;
    mutable std::optional<int> exit_code_;
};

} // namespace

std::unique_ptr<SpawnedProcess> Spawn(const SpawnOptions& options) {
    ValidateOptions(options);

    std::vector<std::string> arguments;
    arguments.reserve(1 + options.arguments.size());
    arguments.push_back(options.command);
    arguments.insert(arguments.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);

    std::vector<std::string> environment_storage;
    std::vector<char*> envp;
    const bool custom_environment = !options.inherit_environment || !options.environment.empty();
    if (custom_environment) {
        AppendEnvironment(options, environment_storage);
        envp.reserve(environment_storage.size() + 1);
        for (auto& entry : environment_storage) envp.push_back(entry.data());
        envp.push_back(nullptr);
    }
    char** environment = custom_environment ? envp.data() : InheritedEnvironment();

    const std::string executable = ResolveCommand(options);

    int stdout_descriptor = -1;
    int stderr_descriptor = -1;
    try {
        if (!options.stdout_path.empty()) stdout_descriptor = OpenRedirectFile(options.stdout_path);
        if (!options.stderr_path.empty()) stderr_descriptor = OpenRedirectFile(options.stderr_path);
    } catch (...) {
        CloseDescriptor(stdout_descriptor);
        CloseDescriptor(stderr_descriptor);
        throw;
    }

    posix_spawn_file_actions_t actions;
    int actions_result = posix_spawn_file_actions_init(&actions);
    if (actions_result != 0) {
        CloseDescriptor(stdout_descriptor);
        CloseDescriptor(stderr_descriptor);
        throw McpError(McpErrorCode::InternalError,
                       std::string("posix_spawn_file_actions_init failed: ") +
                           std::strerror(actions_result));
    }

    posix_spawnattr_t attributes;
    int attributes_result = posix_spawnattr_init(&attributes);
    if (attributes_result != 0) {
        posix_spawn_file_actions_destroy(&actions);
        CloseDescriptor(stdout_descriptor);
        CloseDescriptor(stderr_descriptor);
        throw McpError(McpErrorCode::InternalError,
                       std::string("posix_spawnattr_init failed: ") +
                           std::strerror(attributes_result));
    }

    auto cleanup = [&]() {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        CloseDescriptor(stdout_descriptor);
        CloseDescriptor(stderr_descriptor);
    };

    auto check_spawn_call = [&](int result, const char* operation) {
        if (result == 0) return;
        cleanup();
        throw McpError(McpErrorCode::InternalError,
                       std::string(operation) + " failed: " + std::strerror(result));
    };

    if (stdout_descriptor >= 0) {
        check_spawn_call(
            posix_spawn_file_actions_adddup2(&actions, stdout_descriptor, STDOUT_FILENO),
            "posix_spawn_file_actions_adddup2 for stdout");
        check_spawn_call(posix_spawn_file_actions_addclose(&actions, stdout_descriptor),
                         "posix_spawn_file_actions_addclose for stdout");
    }
    if (stderr_descriptor >= 0) {
        check_spawn_call(
            posix_spawn_file_actions_adddup2(&actions, stderr_descriptor, STDERR_FILENO),
            "posix_spawn_file_actions_adddup2 for stderr");
        check_spawn_call(posix_spawn_file_actions_addclose(&actions, stderr_descriptor),
                         "posix_spawn_file_actions_addclose for stderr");
    }
    if (!options.working_directory.empty()) {
#if MCP_PROCESS_HAS_SPAWN_CHDIR
        check_spawn_call(
            posix_spawn_file_actions_addchdir_np(&actions, options.working_directory.c_str()),
            "posix_spawn_file_actions_addchdir_np");
#else
        cleanup();
        throw McpError(McpErrorCode::InternalError,
                       "spawning with a working directory requires posix_spawn_file_actions_addchdir_np");
#endif
    }

    sigset_t default_signals;
    sigemptyset(&default_signals);
    sigaddset(&default_signals, SIGPIPE);
    check_spawn_call(posix_spawnattr_setsigdefault(&attributes, &default_signals),
                     "posix_spawnattr_setsigdefault");

    short spawn_flags = POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
    if (options.detached) spawn_flags |= POSIX_SPAWN_SETSID;
    else spawn_flags |= POSIX_SPAWN_SETPGROUP;
#else
    spawn_flags |= POSIX_SPAWN_SETPGROUP;
#endif
    if ((spawn_flags & POSIX_SPAWN_SETPGROUP) != 0)
        check_spawn_call(posix_spawnattr_setpgroup(&attributes, 0), "posix_spawnattr_setpgroup");
    check_spawn_call(posix_spawnattr_setflags(&attributes, spawn_flags),
                     "posix_spawnattr_setflags");

    pid_t pid = -1;
    int spawn_result = posix_spawn(&pid, executable.c_str(), &actions, &attributes,
                                   argv.data(), environment);
    cleanup();
    if (spawn_result != 0)
        throw McpError(McpErrorCode::InternalError,
                       "posix_spawn failed for '" + options.command + "': " +
                           std::strerror(spawn_result));

    return std::make_unique<PosixSpawnedProcess>(pid, options.detached);
}

} // namespace mcp
