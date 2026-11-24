#pragma once

#ifndef _WIN32

#include <mcp/transport/detail/PlatformIO.hpp>

#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#endif

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp { namespace detail {

inline char** PosixEnviron() {
#ifdef __APPLE__
    return *_NSGetEnviron();
#else
    return ::environ;
#endif
}

inline void SetPosixEnvironmentEntry(std::vector<std::string>& entries,
                                     std::string_view key, std::string_view value) {
    std::string prefix(key);
    prefix.push_back('=');
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->compare(0, prefix.size(), prefix) == 0) it = entries.erase(it);
        else ++it;
    }
    entries.push_back(prefix + std::string(value));
}

inline std::vector<std::string> BuildPosixEnvironment(const ProcessStartInfo& info) {
    std::vector<std::string> entries;
    if (info.inherit_environment) {
        for (char** entry = PosixEnviron(); entry != nullptr && *entry != nullptr; ++entry) {
            std::string inherited(*entry);
            std::size_t separator = inherited.find('=');
            if (separator == std::string::npos) {
                entries.push_back(std::move(inherited));
                continue;
            }
            std::string_view key(inherited.data(), separator);
            std::string_view value(inherited.data() + separator + 1,
                                   inherited.size() - separator - 1);
            SetPosixEnvironmentEntry(entries, key, value);
        }
    }
    for (const auto& [key, value] : info.environment_variables)
        SetPosixEnvironmentEntry(entries, key, value);
    return entries;
}

[[nodiscard]] inline std::optional<std::string> PosixEnvironmentValue(
    const std::vector<std::string>& entries, std::string_view key) {
    std::string prefix(key);
    prefix.push_back('=');
    std::optional<std::string> result;
    for (const auto& entry : entries) {
        if (entry.compare(0, prefix.size(), prefix) == 0)
            result = entry.substr(prefix.size());
    }
    return result;
}

[[nodiscard]] inline std::string ResolvePosixExecutable(
    const std::vector<std::string>& environment, const std::string& command) {
    if (command.find('/') != std::string::npos) return command;
    auto path_value = PosixEnvironmentValue(environment, "PATH");
    if (!path_value) return command;
    const std::string& path = *path_value;
    std::size_t start = 0;
    for (;;) {
        std::size_t end = path.find(':', start);
        std::string directory = path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (directory.empty()) directory = ".";
        std::string candidate = directory + "/" + command;
        struct stat info{};
        if (::stat(candidate.c_str(), &info) == 0 && !S_ISDIR(info.st_mode) &&
            ::access(candidate.c_str(), X_OK) == 0)
            return candidate;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return command;
}

}} // namespace mcp::detail

#endif
