#include <process/detail/WindowsProcessSupport.hpp>

#include <windows.h>

#include <cwchar>
#include <stdexcept>
#include <utility>

namespace mcp { namespace process { namespace detail {

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return std::wstring();
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length == 0)
        throw std::runtime_error("invalid UTF-8 in process string: Windows error " +
            std::to_string(GetLastError()));
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string QuoteWindowsArgument(const std::string& argument) {
    if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos)
        return argument;
    std::string quoted = "\"";
    std::size_t backslashes = 0;
    for (char ch : argument) {
        if (ch == '\\') {
            ++backslashes;
        } else if (ch == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted += '"';
            backslashes = 0;
        } else {
            quoted.append(backslashes, '\\');
            backslashes = 0;
            quoted += ch;
        }
    }
    quoted.append(backslashes * 2, '\\');
    quoted += '"';
    return quoted;
}

std::string BuildWindowsCommandLine(const std::string& command, const std::vector<std::string>& arguments) {
    std::string line = QuoteWindowsArgument(command);
    for (const auto& argument : arguments) {
        line += ' ';
        line += QuoteWindowsArgument(argument);
    }
    return line;
}

std::wstring BuildWindowsEnvironmentBlock(const std::map<std::string, std::string>& overrides, bool inherit) {
    if (inherit && overrides.empty()) return std::wstring();

    std::vector<std::pair<std::wstring, std::wstring>> wide_overrides;
    wide_overrides.reserve(overrides.size());
    for (const auto& [key, value] : overrides)
        wide_overrides.emplace_back(Utf8ToWide(key), Utf8ToWide(value));

    std::wstring block;
    if (inherit) {
        LPWCH current = GetEnvironmentStringsW();
        if (current != nullptr) {
            for (LPWCH item = current; *item != L'\0'; item += std::wcslen(item) + 1) {
                std::wstring entry(item);
                std::wstring name = entry.substr(0, entry.find(L'='));
                bool replaced = false;
                for (const auto& override_entry : wide_overrides) {
                    if (_wcsicmp(name.c_str(), override_entry.first.c_str()) == 0) {
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) block.append(item, std::wcslen(item) + 1);
            }
            FreeEnvironmentStringsW(current);
        }
    }

    for (const auto& override_entry : wide_overrides) {
        block += override_entry.first;
        block += L'=';
        block += override_entry.second;
        block += L'\0';
    }
    block.append(2, L'\0');
    return block;
}

HANDLE OpenWindowsRedirectFile(const std::string& path) {
    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    attributes.lpSecurityDescriptor = nullptr;
    HANDLE handle = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("failed to open process output file '" + path +
            "': Windows error " + std::to_string(GetLastError()));
    return handle;
}

HANDLE CreateWindowsJobObject(bool kill_on_close, bool breakaway_ok) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    if (kill_on_close)
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (breakaway_ok)
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

bool AssignWindowsProcessToJob(HANDLE job, HANDLE process) {
    return AssignProcessToJobObject(job, process) != FALSE;
}

void TerminateWindowsJobOrProcess(HANDLE job, HANDLE process) {
    if (job != nullptr && TerminateJobObject(job, 1)) return;
    TerminateProcess(process, 1);
}

}}} // namespace mcp::process::detail
