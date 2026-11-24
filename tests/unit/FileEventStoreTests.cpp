#include <mcp/storage/FileEventStore.hpp>

#include <gtest/gtest.h>
#include <mcp/test/McpTestInfo.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace mcp;

namespace {

std::vector<std::pair<uint64_t, std::string>> AppendN(
    FileEventStore& store, const std::string& session, int count, int base)
{
    std::vector<std::pair<uint64_t, std::string>> appended;
    for (int i = 0; i < count; ++i) {
        auto data = "data-" + std::to_string(base + i);
        appended.emplace_back(store.Append(session, data), data);
    }
    return appended;
}

} // namespace

struct FileEventStoreTest : ::testing::Test {
    std::filesystem::path store_dir;

    void SetUp() override {
        store_dir = std::filesystem::temp_directory_path() /
            ("mcp_test_events_" + std::string(mcp::test::CurrentTestName()));
        RemoveDir();
    }

    void TearDown() override {
        RemoveDir();
    }

private:
    void RemoveDir() {
        std::error_code ec;
        std::filesystem::remove_all(store_dir, ec);
    }
};

TEST_F(FileEventStoreTest, AppendAndRetrieve) {
    FileEventStore store(store_dir);
    auto id1 = store.Append("sess1", "event1");
    store.Append("sess1", "event2");
    store.Append("sess2", "event3");

    auto events = store.GetEventsSince("sess1", id1);
    ASSERT_EQ(events.size(), size_t{1});
    EXPECT_EQ(events[0].first, id1 + 1);
    EXPECT_EQ(events[0].second, "event2");

    EXPECT_FALSE(store.GetEventsSince("sess1", 0).empty());
    store.Clear("sess1");
    EXPECT_TRUE(store.GetEventsSince("sess1", 0).empty());
}

TEST_F(FileEventStoreTest, SessionsAreIsolated) {
    FileEventStore store(store_dir);
    store.Append("sess1", "a1");
    store.Append("sess2", "b1");
    store.Append("sess1", "a2");

    auto a = store.GetEventsSince("sess1", 0);
    ASSERT_EQ(a.size(), size_t{2});
    EXPECT_EQ(a[0].second, "a1");
    EXPECT_EQ(a[1].second, "a2");

    auto b = store.GetEventsSince("sess2", 0);
    ASSERT_EQ(b.size(), size_t{1});
    EXPECT_EQ(b[0].second, "b1");
}

TEST_F(FileEventStoreTest, MaxCapacityTrimsOldest) {
    FileEventStore store(store_dir);
    constexpr int kPrefill = static_cast<int>(EventStore::kMaxEventsPerSession) + 10;
    constexpr int kAppends = 10;
    {
        std::ofstream file(store_dir / "sess-sess1.jsonl", std::ios::binary);
        ASSERT_TRUE(file.is_open());
        for (int i = 0; i < kPrefill; ++i) {
            file << "{\"id\":" << (i + 1) << ",\"data\":\"data-" << i << "\"}\n";
        }
    }
    for (int i = 0; i < kAppends; ++i) {
        store.Append("sess1", "data-" + std::to_string(kPrefill + i));
    }

    auto events = store.GetEventsSince("sess1", 0);
    ASSERT_EQ(events.size(), EventStore::kMaxEventsPerSession);
    EXPECT_EQ(events.front().first, static_cast<uint64_t>(kAppends + 11));
    EXPECT_EQ(events.front().second, "data-" + std::to_string(kAppends + 10));
    EXPECT_EQ(events.back().first,
              static_cast<uint64_t>(kPrefill + kAppends));
    EXPECT_EQ(events.back().second,
              "data-" + std::to_string(kPrefill + kAppends - 1));
}

TEST_F(FileEventStoreTest, MaxCapacityMatches10000) {
    EXPECT_EQ(EventStore::kMaxEventsPerSession, size_t{10000});
}

TEST_F(FileEventStoreTest, CursorMatchesLogKeepsAppendStable) {
    FileEventStore store(store_dir);
    AppendN(store, "sess1", 3, 0);
    auto file_path = store_dir / "sess-sess1.jsonl";
    auto meta_path = store_dir / "sess-sess1.meta.json";
    {
        std::ofstream file(file_path, std::ios::binary | std::ios::app);
        ASSERT_TRUE(file.is_open());
        for (int i = 0; i < 50; ++i) {
            file << "{\"id\":" << (4 + i) << ",\"data\":\"bulk-"
                 << i << "\"}\n";
        }
    }
    {
        std::ofstream file(meta_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        file << "{\"next_id\":54,\"count\":53}";
    }

    EXPECT_EQ(store.Append("sess1", "after-bulk"), uint64_t{54});
    EXPECT_EQ(store.Append("sess1", "after-bulk-2"), uint64_t{55});

    auto events = store.GetEventsSince("sess1", 0);
    ASSERT_EQ(events.size(), size_t{55});
    EXPECT_EQ(events.front().second, "data-0");
}

TEST_F(FileEventStoreTest, CorruptCursorFallsBackToLogScan) {
    FileEventStore store(store_dir);
    AppendN(store, "sess1", 2, 0);
    {
        std::ofstream file(store_dir / "sess-sess1.meta.json", std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << "{corrupt";
    }

    EXPECT_EQ(store.Append("sess1", "data-2"), uint64_t{3});
    EXPECT_EQ(store.GetEventsSince("sess1", 0).size(), size_t{3});
}

TEST_F(FileEventStoreTest, PersistsAcrossInstances) {
    uint64_t last_id = 0;
    {
        FileEventStore store(store_dir);
        auto appended = AppendN(store, "sess1", 3, 0);
        last_id = appended[1].first;
    }

    FileEventStore restored(store_dir);
    auto all = restored.GetEventsSince("sess1", 0);
    ASSERT_EQ(all.size(), size_t{3});
    EXPECT_EQ(all[0].second, "data-0");
    EXPECT_EQ(all[2].second, "data-2");

    auto replay = restored.GetEventsSince("sess1", last_id);
    ASSERT_EQ(replay.size(), size_t{1});
    EXPECT_EQ(replay[0].second, "data-2");

    auto next_id = restored.Append("sess1", "data-3");
    EXPECT_EQ(next_id, all[2].first + 1);
}

TEST_F(FileEventStoreTest, TwoInstancesShareDirectory) {
    FileEventStore store_a(store_dir);
    FileEventStore store_b(store_dir);

    auto id_a1 = store_a.Append("sess1", "from-a1");
    auto seen = store_b.GetEventsSince("sess1", 0);
    ASSERT_EQ(seen.size(), size_t{1});
    EXPECT_EQ(seen[0].first, id_a1);
    EXPECT_EQ(seen[0].second, "from-a1");

    auto id_b1 = store_b.Append("sess1", "from-b1");
    auto id_a2 = store_a.Append("sess1", "from-a2");
    EXPECT_NE(id_a2, id_b1);

    auto resumed = store_b.GetEventsSince("sess1", id_a1);
    ASSERT_EQ(resumed.size(), size_t{2});
    EXPECT_EQ(resumed[0].second, "from-b1");
    EXPECT_EQ(resumed[1].second, "from-a2");
    EXPECT_LT(resumed[0].first, resumed[1].first);

    store_a.Clear("sess1");
    EXPECT_TRUE(store_b.GetEventsSince("sess1", 0).empty());
}

TEST_F(FileEventStoreTest, ConcurrentAppendKeepsOrder) {
    FileEventStore store(store_dir);
    constexpr int kThreads = 4;
    constexpr int kPerThread = 25;

    std::vector<std::vector<uint64_t>> ids(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                auto data = "t" + std::to_string(t) + "-" + std::to_string(i);
                ids[t].push_back(store.Append("sess1", data));
            }
        });
    }
    for (auto& thread : threads) thread.join();

    auto events = store.GetEventsSince("sess1", 0);
    ASSERT_EQ(events.size(), size_t{kThreads * kPerThread});

    std::set<uint64_t> unique_ids;
    std::set<std::string> unique_data;
    for (const auto& [id, data] : events) {
        unique_ids.insert(id);
        unique_data.insert(data);
    }
    EXPECT_EQ(unique_ids.size(), size_t{kThreads * kPerThread});
    EXPECT_EQ(unique_data.size(), size_t{kThreads * kPerThread});

    uint64_t expected = 1;
    for (auto id : unique_ids) {
        EXPECT_EQ(id, expected);
        ++expected;
    }
}

TEST_F(FileEventStoreTest, ClearRemovesPersistedSession) {
    {
        FileEventStore store(store_dir);
        AppendN(store, "sess1", 2, 0);
        store.Clear("sess1");
    }
    FileEventStore fresh(store_dir);
    EXPECT_TRUE(fresh.GetEventsSince("sess1", 0).empty());
}

TEST_F(FileEventStoreTest, TornTailLineIsIgnored) {
    {
        FileEventStore store(store_dir);
        AppendN(store, "sess1", 2, 0);
    }
    auto file_path = store_dir / "sess-sess1.jsonl";
    {
        std::ofstream file(file_path, std::ios::binary | std::ios::app);
        ASSERT_TRUE(file.is_open());
        file << "{\"id\":3,\"dat";
    }

    FileEventStore store(store_dir);
    auto events = store.GetEventsSince("sess1", 0);
    ASSERT_EQ(events.size(), size_t{2});

    auto id = store.Append("sess1", "data-2");
    EXPECT_EQ(id, 3);
    auto after = store.GetEventsSince("sess1", 0);
    ASSERT_EQ(after.size(), size_t{3});
    EXPECT_EQ(after[2].second, "data-2");
}

TEST_F(FileEventStoreTest, SpecialCharactersInSessionId) {
    FileEventStore store(store_dir);
    std::string session = "a/b:c*d?e\"f<g>i|j";
    auto id = store.Append(session, "payload");

    auto events = store.GetEventsSince(session, 0);
    ASSERT_EQ(events.size(), size_t{1});
    EXPECT_EQ(events[0].first, id);
    EXPECT_EQ(events[0].second, "payload");

    store.Clear(session);
    EXPECT_TRUE(store.GetEventsSince(session, 0).empty());
}

TEST_F(FileEventStoreTest, InvalidStorageDirThrows) {
    std::filesystem::create_directories(store_dir);
    auto file_dir = store_dir / "not-a-dir";
    {
        std::ofstream file(file_dir);
        ASSERT_TRUE(file.is_open());
        file << "x";
    }
    EXPECT_THROW(FileEventStore store(file_dir / "child"), std::runtime_error);
}

TEST_F(FileEventStoreTest, CursorAheadOfLogRebuildsNextId) {
    {
        FileEventStore store(store_dir);
        AppendN(store, "sess1", 2, 0);
    }
    {
        std::ofstream file(store_dir / "sess-sess1.meta.json",
                           std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        file << "{\"next_id\":100,\"count\":50}";
    }
    FileEventStore store(store_dir);
    EXPECT_EQ(store.Append("sess1", "after"), uint64_t{3});
    auto events = store.GetEventsSince("sess1", 0);
    ASSERT_EQ(events.size(), size_t{3});
    EXPECT_EQ(events[2].first, uint64_t{3});
    EXPECT_EQ(events[2].second, "after");
}

TEST_F(FileEventStoreTest, CursorBehindLogRebuildsNextId) {
    {
        FileEventStore store(store_dir);
        AppendN(store, "sess1", 2, 0);
    }
    {
        std::ofstream file(store_dir / "sess-sess1.meta.json",
                           std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        file << "{\"next_id\":1,\"count\":0}";
    }
    FileEventStore store(store_dir);
    EXPECT_EQ(store.Append("sess1", "after"), uint64_t{3});
    EXPECT_EQ(store.GetEventsSince("sess1", 0).size(), size_t{3});
}

TEST_F(FileEventStoreTest, CursorMissingAfterAppendRebuildsFromLog) {
    {
        FileEventStore store(store_dir);
        AppendN(store, "sess1", 2, 0);
    }
    std::error_code ec;
    std::filesystem::remove(store_dir / "sess-sess1.meta.json", ec);
    ASSERT_FALSE(ec);

    FileEventStore store(store_dir);
    EXPECT_EQ(store.Append("sess1", "after"), uint64_t{3});
    {
        FileEventStore reopened(store_dir);
        EXPECT_EQ(reopened.Append("sess1", "after-2"), uint64_t{4});
    }
    auto events = store.GetEventsSince("sess1", 0);
    ASSERT_EQ(events.size(), size_t{4});
    EXPECT_EQ(events.back().first, uint64_t{4});
}

TEST_F(FileEventStoreTest, TrimThenRestartKeepsNextId) {
    constexpr uint64_t kPrefill = static_cast<uint64_t>(EventStore::kMaxEventsPerSession) + 10;
    ASSERT_TRUE(std::filesystem::create_directories(store_dir));
    {
        std::ofstream file(store_dir / "sess-sess1.jsonl", std::ios::binary);
        ASSERT_TRUE(file.is_open());
        for (uint64_t i = 1; i <= kPrefill; ++i) {
            file << "{\"id\":" << i << ",\"data\":\"bulk-" << i << "\"}\n";
        }
    }
    uint64_t last = 0;
    {
        FileEventStore store(store_dir);
        for (int i = 0; i < 10; ++i) last = store.Append("sess1", "data-" + std::to_string(i));
    }
    EXPECT_EQ(last, kPrefill + 10);

    FileEventStore reopened(store_dir);
    EXPECT_EQ(reopened.Append("sess1", "after-restart"), last + 1);
    auto events = reopened.GetEventsSince("sess1", 0);
    ASSERT_EQ(events.size(), EventStore::kMaxEventsPerSession);
    EXPECT_EQ(events.back().first, last + 1);
}

TEST_F(FileEventStoreTest, ClearThenRestartResetsNextId) {
    {
        FileEventStore store(store_dir);
        AppendN(store, "sess1", 2, 0);
        store.Clear("sess1");
    }
    FileEventStore reopened(store_dir);
    EXPECT_EQ(reopened.Append("sess1", "fresh"), uint64_t{1});
    auto events = reopened.GetEventsSince("sess1", 0);
    ASSERT_EQ(events.size(), size_t{1});
    EXPECT_EQ(events[0].second, "fresh");
}

TEST_F(FileEventStoreTest, NextIdOverflowRejected) {
    auto max_id = std::to_string((std::numeric_limits<uint64_t>::max)());
    ASSERT_TRUE(std::filesystem::create_directories(store_dir));
    {
        std::ofstream file(store_dir / "sess-sess1.jsonl", std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << "{\"id\":" << max_id << ",\"data\":\"last\"}\n";
    }
    FileEventStore store(store_dir);
    EXPECT_THROW(store.Append("sess1", "overflow"), std::runtime_error);
    std::string content;
    {
        std::ifstream file(store_dir / "sess-sess1.jsonl", std::ios::binary);
        content.assign(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(content.find("overflow"), std::string::npos);
}

static std::string EventStoreTestReadEnv(const char* name) {
#ifdef _WIN32
    DWORD size = ::GetEnvironmentVariableA(name, nullptr, 0);
    if (size == 0) return {};
    std::string value(size, '\0');
    ::GetEnvironmentVariableA(name, value.data(), size);
    value.resize(size - 1);
    return value;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
#endif
}

static void EventStoreTestSetEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    std::wstring wide_name(name, name + std::strlen(name));
    std::wstring wide_value(value.begin(), value.end());
    ::SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

static void EventStoreTestClearEnv(const char* name) {
#ifdef _WIN32
    std::wstring wide_name(name, name + std::strlen(name));
    ::SetEnvironmentVariableW(wide_name.c_str(), nullptr);
#else
    ::unsetenv(name);
#endif
}

static std::string EventStoreTestSelfPath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(4096);
    DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    std::wstring wide(buffer.data(), length);
    int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0,
                                       nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), needed,
                          nullptr, nullptr);
    return result;
#else
    char buffer[4096];
    ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) return {};
    buffer[length] = '\0';
    return std::string(buffer);
#endif
}

TEST_F(FileEventStoreTest, ChildProcessAppender) {
    auto dir = EventStoreTestReadEnv("MCP_EVENT_TEST_CHILD_DIR");
    auto session = EventStoreTestReadEnv("MCP_EVENT_TEST_CHILD_SESSION");
    auto count_text = EventStoreTestReadEnv("MCP_EVENT_TEST_CHILD_COUNT");
    if (dir.empty() || session.empty() || count_text.empty()) return;
    FileEventStore store{std::filesystem::path(dir)};
    int count = std::stoi(count_text);
    for (int i = 0; i < count; ++i) {
        store.Append(session, "child-" + std::to_string(i));
    }
}

TEST_F(FileEventStoreTest, MultiProcessAppendKeepsUniqueIds) {
    auto exe = EventStoreTestSelfPath();
    if (exe.empty()) GTEST_SKIP() << "self executable path unavailable";

    EventStoreTestSetEnv("MCP_EVENT_TEST_CHILD_DIR", store_dir.string());
    EventStoreTestSetEnv("MCP_EVENT_TEST_CHILD_SESSION", "sess1");
    EventStoreTestSetEnv("MCP_EVENT_TEST_CHILD_COUNT", "20");

    int child_exit = -1;
#ifdef _WIN32
    std::wstring wide_exe(exe.begin(), exe.end());
    std::wstring command = L"\"" + wide_exe +
        L"\" --gtest_filter=FileEventStoreTest.ChildProcessAppender";
    std::vector<wchar_t> command_buffer(command.begin(), command.end());
    command_buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    PROCESS_INFORMATION process{};
    ASSERT_TRUE(::CreateProcessW(nullptr, command_buffer.data(), nullptr, nullptr,
        FALSE, 0, nullptr, nullptr, &startup, &process));
    ASSERT_EQ(::WaitForSingleObject(process.hProcess, 60000), DWORD{WAIT_OBJECT_0});
    DWORD code = 0;
    ASSERT_TRUE(::GetExitCodeProcess(process.hProcess, &code));
    child_exit = static_cast<int>(code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
#else
    pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        ::execl(exe.c_str(), exe.c_str(),
                "--gtest_filter=FileEventStoreTest.ChildProcessAppender",
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    child_exit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    EventStoreTestClearEnv("MCP_EVENT_TEST_CHILD_DIR");
    EventStoreTestClearEnv("MCP_EVENT_TEST_CHILD_SESSION");
    EventStoreTestClearEnv("MCP_EVENT_TEST_CHILD_COUNT");
    ASSERT_EQ(child_exit, 0);

    FileEventStore store(store_dir);
    for (int i = 0; i < 20; ++i) store.Append("sess1", "parent-" + std::to_string(i));
    auto events = store.GetEventsSince("sess1", 0);
    ASSERT_EQ(events.size(), size_t{40});
    std::set<uint64_t> ids;
    for (const auto& [id, data] : events) ids.insert(id);
    EXPECT_EQ(ids.size(), size_t{40});
    uint64_t expected = 1;
    for (auto id : ids) {
        EXPECT_EQ(id, expected);
        ++expected;
    }
}
