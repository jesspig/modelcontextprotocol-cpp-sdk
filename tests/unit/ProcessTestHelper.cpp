#include <mcp/process/Process.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

std::string ToUtf8(const std::filesystem::path& path) {
    auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

void SleepMilliseconds(int milliseconds) {
    if (milliseconds > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

int EchoArguments(const std::vector<std::string>& arguments) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        std::printf("ARG:%s\n", arguments[index].c_str());
        std::fflush(stdout);
    }
    return 0;
}

int PrintEnvironment(const std::vector<std::string>& arguments) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const char* value = std::getenv(arguments[index].c_str());
        std::printf("ENV:%s=%s\n", arguments[index].c_str(), value == nullptr ? "<absent>" : value);
        std::fflush(stdout);
    }
    return 0;
}

int PrintWorkingDirectory() {
    std::printf("CWD:%s\n", ToUtf8(std::filesystem::current_path()).c_str());
    std::fflush(stdout);
    return 0;
}

int PrintStandardStreams() {
    std::printf("STDOUT-LINE\n");
    std::fflush(stdout);
    std::fprintf(stderr, "STDERR-LINE\n");
    std::fflush(stderr);
    return 0;
}

#ifdef _WIN32
std::wstring SelfPath() {
    std::vector<wchar_t> buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::wstring(buffer.data(), static_cast<std::size_t>(length));
}

int SpawnGrandchild(int milliseconds) {
    std::wstring command_line = L"\"" + SelfPath() + L"\" --sleep " +
        std::to_wstring(milliseconds);
    std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info = {};
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process_info)) {
        std::printf("GRANDCHILD:failed\n");
        std::fflush(stdout);
        return 3;
    }
    std::printf("GRANDCHILD:%llu\n",
                static_cast<unsigned long long>(process_info.dwProcessId));
    std::fflush(stdout);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    SleepMilliseconds(milliseconds);
    return 0;
}
#else
int SpawnGrandchild(int milliseconds) {
    pid_t child = fork();
    if (child == 0) {
        execl("/bin/sleep", "sleep", "30", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) {
        std::printf("GRANDCHILD:failed\n");
        std::fflush(stdout);
        return 3;
    }
    std::printf("GRANDCHILD:%lld\n", static_cast<long long>(child));
    std::fflush(stdout);
    SleepMilliseconds(milliseconds);
    return 0;
}
#endif

int LaunchDetached(const std::vector<std::string>& arguments) {
    if (arguments.size() < 3) return 2;
    mcp::SpawnOptions options;
    options.command = arguments[0];
    options.arguments = {"--write-marker", arguments[1], arguments[2]};
    options.detached = true;
    auto process = mcp::Spawn(options);
    std::printf("DETACHED:%lld\n", static_cast<long long>(process->Pid()));
    std::fflush(stdout);
    return 0;
}

int WriteMarker(const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) return 2;
    SleepMilliseconds(std::atoi(arguments[1].c_str()));
    std::ofstream marker(arguments[0], std::ios::binary | std::ios::trunc);
    marker << "alive\n";
    marker.flush();
    return marker ? 0 : 4;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> arguments(argv + 1, argv + argc);
    if (arguments.empty()) return 2;
    const std::string mode = arguments[0];
    std::vector<std::string> rest(arguments.begin() + 1, arguments.end());
    if (mode == "--echo") return EchoArguments(rest);
    if (mode == "--print-env") return PrintEnvironment(rest);
    if (mode == "--print-cwd") return PrintWorkingDirectory();
    if (mode == "--stdio") return PrintStandardStreams();
    if (mode == "--sleep") {
        if (rest.empty()) return 2;
        SleepMilliseconds(std::atoi(rest[0].c_str()));
        return 0;
    }
    if (mode == "--exit") {
        if (rest.empty()) return 2;
        return std::atoi(rest[0].c_str());
    }
    if (mode == "--spawn-grandchild") {
        if (rest.empty()) return 2;
        return SpawnGrandchild(std::atoi(rest[0].c_str()));
    }
    if (mode == "--launch-detached") return LaunchDetached(rest);
    if (mode == "--write-marker") return WriteMarker(rest);
    return 2;
}
