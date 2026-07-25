// FileTaskStoreTests - unit tests for FileTaskStore

#include <mcp/storage/FileTaskStore.hpp>

#include <gtest/gtest.h>

#include <filesystem>

using namespace mcp;

struct FileTaskStoreTest : ::testing::Test {
    std::filesystem::path store_path;

    void SetUp() override {
        store_path = std::filesystem::temp_directory_path() / "mcp_test_tasks.json";
    }

    void TearDown() override {
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
