// FileEventStoreTests - unit tests for FileEventStore

#include <mcp/storage/FileEventStore.hpp>

#include <mcp/test/McpTest.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

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

struct FileEventStoreTest : mcp::test::TestCase {
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
    constexpr int kExtra = 10;
    for (int i = 0; i < static_cast<int>(EventStore::kMaxEventsPerSession) + kExtra; ++i) {
        store.Append("sess1", "data-" + std::to_string(i));
    }

    auto events = store.GetEventsSince("sess1", 0);
    EXPECT_EQ(events.size(), EventStore::kMaxEventsPerSession);
    EXPECT_EQ(events.front().second, "data-" + std::to_string(kExtra));
    EXPECT_EQ(events.back().second,
              "data-" + std::to_string(static_cast<int>(EventStore::kMaxEventsPerSession) + kExtra - 1));
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

// Two instances on the same directory emulate two processes: events
// appended by one are immediately visible to the other and ids stay unique.
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
