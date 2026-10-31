#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

std::string g_executable;

void SleepMilliseconds(int milliseconds) {
    if (milliseconds > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

int EchoLoop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::cout << line << '\n';
        std::cout.flush();
    }
    return 0;
}

int WriteStderrLine(const std::string& text) {
    std::fprintf(stderr, "%s\n", text.c_str());
    std::fflush(stderr);
    return 0;
}

#ifdef _WIN32
int SpawnGrandchild(const std::string& pid_file, int milliseconds) {
    std::string command_line = "\"" + g_executable + "\" --sleep " +
        std::to_string(milliseconds);
    std::vector<char> buffer(command_line.begin(), command_line.end());
    buffer.push_back('\0');
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info = {};
    if (!CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process_info)) {
        std::fprintf(stderr, "GRANDCHILD:failed\n");
        std::fflush(stderr);
        return 3;
    }
    std::ofstream pid_output(pid_file, std::ios::binary | std::ios::trunc);
    pid_output << process_info.dwProcessId << "\n";
    pid_output.flush();
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    SleepMilliseconds(milliseconds);
    return 0;
}
#else
int SpawnGrandchild(const std::string& pid_file, int milliseconds) {
    pid_t child = fork();
    if (child == 0) {
        execl(g_executable.c_str(), g_executable.c_str(), "--sleep",
              std::to_string(milliseconds).c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) {
        std::fprintf(stderr, "GRANDCHILD:failed\n");
        std::fflush(stderr);
        return 3;
    }
    std::ofstream pid_output(pid_file, std::ios::binary | std::ios::trunc);
    pid_output << child << "\n";
    pid_output.flush();
    SleepMilliseconds(milliseconds);
    return 0;
}
#endif

int IgnoreSigterm(int milliseconds) {
#ifndef _WIN32
    std::signal(SIGTERM, SIG_IGN);
#endif
    SleepMilliseconds(milliseconds);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    g_executable = argv[0];
    std::vector<std::string> arguments(argv + 1, argv + argc);
    if (arguments.empty()) return 2;
    const std::string mode = arguments[0];
    std::vector<std::string> rest(arguments.begin() + 1, arguments.end());

    if (mode == "--echo") return EchoLoop();
    if (mode == "--stderr-line") {
        if (rest.empty()) return 2;
        return WriteStderrLine(rest[0]);
    }
    if (mode == "--sleep") {
        if (rest.empty()) return 2;
        SleepMilliseconds(std::atoi(rest[0].c_str()));
        return 0;
    }
    if (mode == "--spawn-grandchild") {
        if (rest.size() < 2) return 2;
        return SpawnGrandchild(rest[0], std::atoi(rest[1].c_str()));
    }
    if (mode == "--ignore-sigterm") {
        if (rest.empty()) return 2;
        return IgnoreSigterm(std::atoi(rest[0].c_str()));
    }
    return 2;
}
