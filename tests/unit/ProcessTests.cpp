#include <mcp/process/Process.hpp>
#include <mcp/McpError.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace mcp;

namespace {

constexpr int kHelperTimeoutMs = 10000;

std::string ToUtf8(const std::filesystem::path& path) {
    auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

std::filesystem::path MakeTempDirectory() {
    static std::atomic<unsigned long long> counter{0};
    auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    auto directory = std::filesystem::temp_directory_path() /
        ("mcp_process_test_" + std::to_string(stamp) + "_" +
         std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(directory);
    return directory;
}

struct TempDirectory {
    std::filesystem::path path = MakeTempDirectory();

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path File(const std::string& name) const { return path / name; }
};

const std::filesystem::path& HelperPath() {
    static const std::filesystem::path path = MCP_PROCESS_TEST_HELPER_PATH;
    return path;
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

void WriteAll(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

std::vector<std::string> SplitLines(std::string text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos
            ? std::string::npos
            : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

std::string WaitForContent(const std::filesystem::path& path, const std::string& needle,
                           int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    std::string content;
    while (std::chrono::steady_clock::now() < deadline) {
        content = ReadAll(path);
        if (content.find(needle) != std::string::npos) return content;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return ReadAll(path);
}

bool ProcessAlive(int64_t pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                 static_cast<DWORD>(pid));
    if (process == nullptr) return false;
    DWORD result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return result == WAIT_TIMEOUT;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

bool WaitUntilGone(int64_t pid, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!ProcessAlive(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !ProcessAlive(pid);
}

int64_t ParseGrandchildPid(const std::string& content) {
    const std::string marker = "GRANDCHILD:";
    std::size_t position = content.find(marker);
    if (position == std::string::npos) return -1;
    position += marker.size();
    std::size_t end = position;
    while (end < content.size() && content[end] >= '0' && content[end] <= '9') ++end;
    if (end == position) return -1;
    return std::stoll(content.substr(position, end - position));
}

SpawnOptions HelperOptions(const std::vector<std::string>& arguments, const TempDirectory& temp,
                           const std::string& log_name = "stdout.log") {
    SpawnOptions options;
    options.command = ToUtf8(HelperPath());
    options.arguments = arguments;
    options.stdout_path = ToUtf8(temp.File(log_name));
    options.stderr_path = ToUtf8(temp.File("stderr.log"));
    return options;
}

} // namespace

TEST(SpawnTest, PreservesArgumentBoundariesWithoutShell) {
    TempDirectory temp;
    const std::vector<std::string> expected = {
        "plain",
        "with space",
        "quote\"inside",
        "back\\slash",
        "trailing\\",
        "space and trailing\\",
        "",
        "tab\tchar",
        "$PATH",
        "a&b|c>d<e",
        "%PATH%",
    };
    std::vector<std::string> arguments = {"--echo"};
    arguments.insert(arguments.end(), expected.begin(), expected.end());

    auto process = Spawn(HelperOptions(arguments, temp));
    ASSERT_TRUE(process->Wait(kHelperTimeoutMs));
    ASSERT_EQ(process->ExitCode().value_or(-1), 0);

    auto lines = SplitLines(ReadAll(temp.File("stdout.log")));
    ASSERT_EQ(lines.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
        EXPECT_EQ(lines[index], "ARG:" + expected[index]);
}

TEST(SpawnTest, InheritsEnvironmentAndAppliesOverrides) {
    TempDirectory temp;
    auto options = HelperOptions(
        {"--print-env", "PATH", "MCP_PROCESS_UNSET_SENTINEL"}, temp);
    options.environment["PATH"] = "mcp-process-override";
    auto process = Spawn(options);
    ASSERT_TRUE(process->Wait(kHelperTimeoutMs));
    ASSERT_EQ(process->ExitCode().value_or(-1), 0);

    const std::string content = ReadAll(temp.File("stdout.log"));
    EXPECT_NE(content.find("ENV:PATH=mcp-process-override"), std::string::npos) << content;
    EXPECT_NE(content.find("ENV:MCP_PROCESS_UNSET_SENTINEL=<absent>"), std::string::npos)
        << content;
}

TEST(SpawnTest, UsesOnlyProvidedEnvironmentWhenNotInheriting) {
    TempDirectory temp;
    auto options = HelperOptions({"--print-env", "PATH", "MCP_PROCESS_ONLY"}, temp);
    options.inherit_environment = false;
    options.environment["MCP_PROCESS_ONLY"] = "1";
    auto process = Spawn(options);
    ASSERT_TRUE(process->Wait(kHelperTimeoutMs));
    ASSERT_EQ(process->ExitCode().value_or(-1), 0);

    const std::string content = ReadAll(temp.File("stdout.log"));
    EXPECT_NE(content.find("ENV:PATH=<absent>"), std::string::npos) << content;
    EXPECT_NE(content.find("ENV:MCP_PROCESS_ONLY=1"), std::string::npos) << content;
}

TEST(SpawnTest, AppliesWorkingDirectory) {
    TempDirectory temp;
    auto options = HelperOptions({"--print-cwd"}, temp);
    options.working_directory = ToUtf8(temp.path);
    auto process = Spawn(options);
    ASSERT_TRUE(process->Wait(kHelperTimeoutMs));
    ASSERT_EQ(process->ExitCode().value_or(-1), 0);

    auto lines = SplitLines(ReadAll(temp.File("stdout.log")));
    ASSERT_EQ(lines.size(), 1u);
    ASSERT_EQ(lines[0].rfind("CWD:", 0), 0u);
    const std::string reported = lines[0].substr(4);
    EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::u8path(reported), temp.path))
        << reported;
}

TEST(SpawnTest, RedirectsAndTruncatesStandardStreams) {
    TempDirectory temp;
    WriteAll(temp.File("stdout.log"), "stale-stdout");
    WriteAll(temp.File("stderr.log"), "stale-stderr");
    auto process = Spawn(HelperOptions({"--stdio"}, temp));
    ASSERT_TRUE(process->Wait(kHelperTimeoutMs));
    ASSERT_EQ(process->ExitCode().value_or(-1), 0);

    const std::string out = ReadAll(temp.File("stdout.log"));
    const std::string err = ReadAll(temp.File("stderr.log"));
    EXPECT_NE(out.find("STDOUT-LINE"), std::string::npos) << out;
    EXPECT_EQ(out.find("stale-stdout"), std::string::npos) << out;
    EXPECT_EQ(out.find("STDERR-LINE"), std::string::npos) << out;
    EXPECT_NE(err.find("STDERR-LINE"), std::string::npos) << err;
    EXPECT_EQ(err.find("stale-stderr"), std::string::npos) << err;
}

TEST(SpawnTest, WaitTimesOutThenReportsExit) {
    TempDirectory temp;
    auto process = Spawn(HelperOptions({"--sleep", "1500"}, temp));
    EXPECT_FALSE(process->Wait(100));
    EXPECT_TRUE(process->IsRunning());
    EXPECT_FALSE(process->ExitCode().has_value());
    EXPECT_TRUE(process->Wait(kHelperTimeoutMs));
    EXPECT_EQ(process->ExitCode().value_or(-1), 0);
    EXPECT_FALSE(process->IsRunning());
    EXPECT_TRUE(process->Wait(0));
}

TEST(SpawnTest, ReportsNonZeroExitCode) {
    TempDirectory temp;
    auto process = Spawn(HelperOptions({"--exit", "7"}, temp));
    ASSERT_TRUE(process->Wait(kHelperTimeoutMs));
    EXPECT_EQ(process->ExitCode().value_or(-1), 7);
    EXPECT_FALSE(process->IsRunning());
}

TEST(SpawnTest, TerminatesProcessTree) {
    TempDirectory temp;
    auto process = Spawn(HelperOptions({"--spawn-grandchild", "30000"}, temp));
    const std::string content = WaitForContent(temp.File("stdout.log"), "GRANDCHILD:", 10000);
    const int64_t grandchild = ParseGrandchildPid(content);
    ASSERT_GT(grandchild, 0) << content << ReadAll(temp.File("stderr.log"));

    EXPECT_TRUE(ProcessAlive(grandchild));
    EXPECT_TRUE(process->Terminate(3000));
    EXPECT_FALSE(process->IsRunning());
    EXPECT_TRUE(process->ExitCode().has_value());
    EXPECT_TRUE(WaitUntilGone(grandchild, 5000)) << content;
}

TEST(SpawnTest, DetachedChildSurvivesLauncherExit) {
    TempDirectory temp;
    const auto marker = temp.File("detached.marker");
    auto options = HelperOptions(
        {"--launch-detached", ToUtf8(HelperPath()), ToUtf8(marker), "1200"}, temp,
        "launcher.log");
    auto launcher = Spawn(options);
    ASSERT_TRUE(launcher->Wait(kHelperTimeoutMs));
    ASSERT_EQ(launcher->ExitCode().value_or(-1), 0);
    EXPECT_NE(ReadAll(temp.File("launcher.log")).find("DETACHED:"), std::string::npos);

    const std::string content = WaitForContent(marker, "alive", 10000);
    EXPECT_NE(content.find("alive"), std::string::npos) << ReadAll(temp.File("stderr.log"));
    launcher.reset();
}

TEST(SpawnTest, RejectsEmptyCommand) {
    SpawnOptions options;
    EXPECT_THROW(Spawn(options), McpError);
}

TEST(SpawnTest, SpawnsConcurrentlyFromMultipleThreads) {
    TempDirectory temp;
    constexpr int kSpawnThreads = 8;
    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kSpawnThreads);
    for (int index = 0; index < kSpawnThreads; ++index) {
        threads.emplace_back([&, index] {
            while (!start.load()) std::this_thread::yield();
            auto options = HelperOptions({"--exit", "0"}, temp,
                                         "thread-" + std::to_string(index) + ".log");
            try {
                auto process = Spawn(options);
                if (!process->Wait(kHelperTimeoutMs) ||
                    process->ExitCode().value_or(-1) != 0)
                    ++failures;
            } catch (...) {
                ++failures;
            }
        });
    }
    start.store(true);
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(failures.load(), 0);
}

#ifndef _WIN32
TEST(SpawnTest, PosixChildLeadsOwnProcessGroupAndIsReaped) {
    TempDirectory temp;
    auto process = Spawn(HelperOptions({"--sleep", "30000"}, temp));
    const pid_t pid = static_cast<pid_t>(process->Pid());
    ASSERT_GT(pid, 0);
    EXPECT_EQ(getpgid(pid), pid);

    EXPECT_TRUE(process->Terminate(500));
    int status = 0;
    errno = 0;
    EXPECT_EQ(waitpid(pid, &status, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(SpawnTest, PosixDestructorReapsExitedChild) {
    TempDirectory temp;
    auto process = Spawn(HelperOptions({"--sleep", "100"}, temp));
    const int64_t pid = process->Pid();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    process.reset();

    int status = 0;
    errno = 0;
    EXPECT_EQ(waitpid(static_cast<pid_t>(pid), &status, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(SpawnTest, PosixDetachedChildRunsInNewProcessGroup) {
    TempDirectory temp;
    auto options = HelperOptions({"--sleep", "30000"}, temp);
    options.detached = true;
    auto process = Spawn(options);
    const pid_t pid = static_cast<pid_t>(process->Pid());
    ASSERT_GT(pid, 0);
    EXPECT_EQ(getpgid(pid), pid);
#ifdef __linux__
    EXPECT_EQ(getsid(pid), pid);
#endif
    EXPECT_TRUE(process->Terminate(500));
}
#endif
