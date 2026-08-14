// FileTaskStoreTests - unit tests for FileTaskStore

#include <mcp/storage/FileTaskStore.hpp>

#include <mcp/test/McpTest.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace mcp;

struct FileTaskStoreTest : mcp::test::TestCase {
    std::filesystem::path store_path;

    void SetUp() override {
        auto temp_dir = std::filesystem::temp_directory_path();
        store_path = temp_dir / ("mcp_test_tasks_" +
            std::string(mcp::test::CurrentTestName()) +
            ".json");
        RemoveFiles();
    }

    void TearDown() override {
        RemoveFiles();
    }

private:
    void RemoveFiles() {
        std::error_code ec;
        std::filesystem::remove(store_path, ec);
        std::filesystem::remove(store_path.string() + ".tmp", ec);
    }
};

TEST_F(FileTaskStoreTest, CreateTask) {
    FileTaskStore store(store_path);
    auto task = store.CreateTask("task-1");

    EXPECT_EQ(task.task_id, "task-1");
    EXPECT_EQ(task.status, TaskStatus::Pending);

    auto fetched = store.GetTask("task-1");
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->task_id, "task-1");
    EXPECT_EQ(fetched->status, TaskStatus::Pending);
}

TEST_F(FileTaskStoreTest, GetNonExistentTask) {
    FileTaskStore store(store_path);
    auto task = store.GetTask("nonexistent");
    EXPECT_FALSE(task.has_value());
}

TEST_F(FileTaskStoreTest, UpdateTask) {
    FileTaskStore store(store_path);
    store.CreateTask("task-1");

    JsonValue result(JsonValue::object_tag);
    result["answer"] = JsonValue(42);
    bool updated = store.UpdateTask("task-1", result);
    EXPECT_TRUE(updated);

    auto task = store.GetTask("task-1");
    ASSERT_TRUE(task.has_value());
    ASSERT_TRUE(task->result.has_value());
    EXPECT_EQ(task->result->At("answer").GetInt(), 42);
    EXPECT_EQ(task->status, TaskStatus::Completed);
}

TEST_F(FileTaskStoreTest, CancelTask) {
    FileTaskStore store(store_path);
    store.CreateTask("task-1");

    bool cancelled = store.CancelTask("task-1", "user request");
    EXPECT_TRUE(cancelled);

    auto task = store.GetTask("task-1");
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->status, TaskStatus::Cancelled);
    ASSERT_TRUE(task->error_message.has_value());
    EXPECT_EQ(*task->error_message, "user request");
}

TEST_F(FileTaskStoreTest, GetAllTasks) {
    FileTaskStore store(store_path);
    store.CreateTask("task-1");
    store.CreateTask("task-2");
    store.CreateTask("task-3");

    auto tasks = store.GetAllTasks();
    EXPECT_EQ(tasks.size(), size_t{3});
}

TEST_F(FileTaskStoreTest, SetTaskStatus) {
    FileTaskStore store(store_path);
    store.CreateTask("task-1");

    bool set = store.SetTaskStatus("task-1", TaskStatus::Working);
    EXPECT_TRUE(set);

    auto task = store.GetTask("task-1");
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->status, TaskStatus::Working);
}

// ── Duplicate task ids are rejected (throws, not silent overwrite) ──
TEST_F(FileTaskStoreTest, CreateDuplicateTaskThrows) {
    FileTaskStore store(store_path);
    store.CreateTask("task-1");
    EXPECT_THROW(store.CreateTask("task-1"), std::runtime_error);
}

// ── State survives store destruction and reconstruction from the file ──
TEST_F(FileTaskStoreTest, PersistsAcrossInstances) {
    {
        FileTaskStore store(store_path);
        store.CreateTask("task-1");
        store.SetTaskStatus("task-1", TaskStatus::Working);
        JsonValue result(JsonValue::object_tag);
        result["answer"] = JsonValue(42);
        store.UpdateTask("task-1", result);
    }

    FileTaskStore restored(store_path);
    auto tasks = restored.GetAllTasks();
    ASSERT_EQ(tasks.size(), size_t{1});
    EXPECT_EQ(tasks[0].task_id, "task-1");
    EXPECT_EQ(tasks[0].status, TaskStatus::Completed);
    ASSERT_TRUE(tasks[0].result.has_value());
    EXPECT_EQ(tasks[0].result->At("answer").GetInt(), 42);
}

// ── A corrupt store file must not throw on construction; the store is empty ──
TEST_F(FileTaskStoreTest, CorruptFileDoesNotThrow) {
    {
        std::ofstream ofs(store_path);
        ASSERT_TRUE(ofs.is_open());
        ofs << "{ not valid json";
    }

    FileTaskStore store(store_path);
    EXPECT_TRUE(store.GetAllTasks().empty());
    EXPECT_FALSE(store.GetTask("task-1").has_value());
}
