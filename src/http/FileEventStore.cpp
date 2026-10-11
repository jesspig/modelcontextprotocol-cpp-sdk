#include <mcp/storage/FileEventStore.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/detail/AtomicJsonFile.hpp>

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mcp {

namespace {

std::string SanitizeSessionId(std::string_view session_id) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string name = "sess-";
    for (char c : session_id) {
        auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0 || c == '-' || c == '_') {
            name += static_cast<char>(u);
        } else {
            name += '~';
            name += kHex[u >> 4];
            name += kHex[u & 0x0F];
        }
    }
    return name;
}

class SessionFileLock {
public:
    explicit SessionFileLock(const std::filesystem::path& lock_path) {
#ifdef _WIN32
        handle_ = CreateFileW(lock_path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) handle_ = nullptr;
        if (handle_ == nullptr) {
            throw std::runtime_error(
                "event store: failed to open lock file " + lock_path.string());
        }
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr) {
            CloseHandle(handle_);
            handle_ = nullptr;
            throw std::runtime_error(
                "event store: failed to create lock event for " +
                lock_path.string());
        }
        BOOL locked =
            LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov);
        if (locked) {
            locked = WaitForSingleObject(ov.hEvent, INFINITE) == WAIT_OBJECT_0;
        }
        CloseHandle(ov.hEvent);
        if (!locked) {
            CloseHandle(handle_);
            handle_ = nullptr;
            throw std::runtime_error("event store: failed to lock " +
                                     lock_path.string());
        }
#else
        fd_ = ::open(lock_path.string().c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ < 0) {
            throw std::runtime_error(
                "event store: failed to open lock file " + lock_path.string());
        }
        while (::flock(fd_, LOCK_EX) != 0) {
            if (errno != EINTR) {
                ::close(fd_);
                fd_ = -1;
                throw std::runtime_error("event store: failed to lock " +
                                         lock_path.string());
            }
        }
#endif
    }

    ~SessionFileLock() {
#ifdef _WIN32
        if (handle_ == nullptr) return;
        OVERLAPPED ov{};
        UnlockFileEx(handle_, 0, 1, 0, &ov);
        CloseHandle(handle_);
#else
        if (fd_ < 0) return;
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
#endif
    }

    SessionFileLock(const SessionFileLock&) = delete;
    SessionFileLock& operator=(const SessionFileLock&) = delete;

private:
#ifdef _WIN32
    HANDLE handle_{nullptr};
#else
    int fd_{-1};
#endif
};

std::string SerializeEventLine(uint64_t id, const std::string& data) {
    JsonValue::Object obj;
    obj["id"] = JsonValue(static_cast<int64_t>(id));
    obj["data"] = JsonValue(data);
    return JsonValue(std::move(obj)).Dump(-1) + "\n";
}

struct LoadedEvents {
    std::vector<std::pair<uint64_t, std::string>> items;
    bool ends_with_newline;
};

struct ScannedEvents {
    size_t count{0};
    uint64_t max_id{0};
    bool ends_with_newline{true};
};

std::optional<std::string> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return std::nullopt;
    auto size = file.tellg();
    if (size <= 0) return std::string();
    std::string content(static_cast<size_t>(size), '\0');
    file.seekg(0);
    file.read(content.data(), size);
    content.resize(static_cast<size_t>(file.gcount()));
    return content;
}

bool ParseCanonicalEventId(std::string_view line, uint64_t& id_out)
{
    static constexpr std::string_view kIdPrefix = "{\"id\":";
    static constexpr std::string_view kDataPrefix = ",\"data\":\"";
    if (line.size() <= kIdPrefix.size() ||
        line.compare(0, kIdPrefix.size(), kIdPrefix) != 0)
        return false;
    uint64_t value = 0;
    size_t pos = kIdPrefix.size();
    size_t digits = 0;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        uint64_t digit = static_cast<uint64_t>(line[pos] - '0');
        if (value > ((std::numeric_limits<uint64_t>::max)() - digit) / 10)
            return false;
        value = value * 10 + digit;
        ++pos;
        ++digits;
    }
    if (digits == 0 || line.compare(pos, kDataPrefix.size(), kDataPrefix) != 0)
        return false;
    pos += kDataPrefix.size();
    while (pos < line.size() && line[pos] != '"') {
        if (line[pos] == '\\') ++pos;
        ++pos;
    }
    if (pos + 1 != line.size() || line[pos] != '"')
        return false;
    id_out = value;
    return true;
}

void CountEventLine(std::string_view line, ScannedEvents& scanned)
{
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) return;
    uint64_t id = 0;
    if (ParseCanonicalEventId(line, id)) {
        ++scanned.count;
        if (id > scanned.max_id) scanned.max_id = id;
        return;
    }
    try {
        auto json = JsonValue::Parse(line);
        auto* id_value = json.Find("id");
        auto* data = json.Find("data");
        if (!id_value || !data) return;
        auto parsed = static_cast<uint64_t>(id_value->GetInt());
        ++scanned.count;
        if (parsed > scanned.max_id) scanned.max_id = parsed;
    } catch (const std::exception&) {
        return;
    }
}

ScannedEvents ScanEvents(const std::filesystem::path& path)
{
    ScannedEvents scanned;
    auto bytes = ReadFileBytes(path);
    if (!bytes) return scanned;
    scanned.ends_with_newline = !bytes->empty() && bytes->back() == '\n';
    std::string_view rest(*bytes);
    while (!rest.empty()) {
        auto end = rest.find('\n');
        auto line = rest.substr(0, end);
        rest.remove_prefix(
            end == std::string_view::npos ? rest.size() : end + 1);
        CountEventLine(line, scanned);
    }
    return scanned;
}

LoadedEvents LoadEvents(const std::filesystem::path& path)
{
    LoadedEvents loaded{{}, true};
    auto bytes = ReadFileBytes(path);
    if (!bytes) return loaded;
    const std::string& content = *bytes;
    loaded.ends_with_newline = !content.empty() && content.back() == '\n';
    std::string_view rest(content);
    while (!rest.empty()) {
        auto end = rest.find('\n');
        auto line = rest.substr(0, end);
        rest.remove_prefix(
            end == std::string_view::npos ? rest.size() : end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        try {
            auto json = JsonValue::Parse(line);
            auto* id = json.Find("id");
            auto* data = json.Find("data");
            if (!id || !data) continue;
            loaded.items.emplace_back(
                static_cast<uint64_t>(id->GetInt()), data->GetString());
        } catch (const std::exception&) {
            continue;
        }
    }
    return loaded;
}

void RewriteTrimmed(const std::filesystem::path& path,
                    std::vector<std::pair<uint64_t, std::string>>& events)
{
    events.erase(events.begin(),
                 events.end() - static_cast<std::ptrdiff_t>(
                                     EventStore::kMaxEventsPerSession));
    std::string content;
    for (const auto& [id, data] : events) {
        content += SerializeEventLine(id, data);
    }
    if (!detail::WriteAtomic(path, content)) {
        throw std::runtime_error("event store: failed to trim " +
                                 path.string());
    }
}

} // anonymous namespace

FileEventStore::FileEventStore(std::filesystem::path storage_dir)
    : storage_dir_(std::move(storage_dir))
{
    std::error_code ec;
    std::filesystem::create_directories(storage_dir_, ec);
    if (ec) {
        throw std::runtime_error("event store: failed to create directory " +
                                 storage_dir_.string() + ": " + ec.message());
    }
}

std::filesystem::path FileEventStore::SessionFilePath(
    std::string_view session_id) const
{
    return storage_dir_ / (SanitizeSessionId(session_id) + ".jsonl");
}

std::filesystem::path FileEventStore::SessionLockPath(
    std::string_view session_id) const
{
    return storage_dir_ / (SanitizeSessionId(session_id) + ".lock");
}

uint64_t FileEventStore::Append(
    std::string_view session_id, std::string event_data)
{
    auto file_path = SessionFilePath(session_id);
    SessionFileLock lock(SessionLockPath(session_id));

    auto scanned = ScanEvents(file_path);
    uint64_t next_id = scanned.max_id + 1;

    {
        std::ofstream file(file_path, std::ios::binary | std::ios::app);
        if (!file.is_open()) {
            throw std::runtime_error("event store: failed to open " +
                                     file_path.string() + " for append");
        }
        if (!scanned.ends_with_newline) file << '\n';
        file << SerializeEventLine(next_id, event_data);
        file.flush();
        if (!file.good()) {
            throw std::runtime_error("event store: failed to append to " +
                                     file_path.string());
        }
    }

    if (scanned.count + 1 > kMaxEventsPerSession) {
        auto loaded = LoadEvents(file_path);
        RewriteTrimmed(file_path, loaded.items);
    }
    return next_id;
}

std::vector<std::pair<uint64_t, std::string>> FileEventStore::GetEventsSince(
    std::string_view session_id, uint64_t last_event_id) const
{
    SessionFileLock lock(SessionLockPath(session_id));
    auto loaded = LoadEvents(SessionFilePath(session_id));
    std::vector<std::pair<uint64_t, std::string>> result;
    for (auto& [id, data] : loaded.items) {
        if (id > last_event_id) result.push_back({id, std::move(data)});
    }
    return result;
}

void FileEventStore::Clear(std::string_view session_id) {
    auto file_path = SessionFilePath(session_id);
    SessionFileLock lock(SessionLockPath(session_id));
    std::error_code ec;
    std::filesystem::remove(file_path, ec);
    if (ec) {
        throw std::runtime_error("event store: failed to remove " +
                                 file_path.string() + ": " + ec.message());
    }
}

} // namespace mcp
