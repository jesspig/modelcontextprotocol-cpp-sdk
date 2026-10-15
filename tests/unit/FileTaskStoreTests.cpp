#include <mcp/storage/FileTaskStore.hpp>

#include <gtest/gtest.h>
#include <mcp/test/McpTestInfo.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace mcp;

struct FileTaskStoreTest : ::testing::Test {
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

TEST_F(FileTaskStoreTest, CreateDuplicateTaskThrows) {
    FileTaskStore store(store_path);
    store.CreateTask("task-1");
    EXPECT_THROW(store.CreateTask("task-1"), std::runtime_error);
}

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

TEST_F(FileTaskStoreTest, AdvanceTaskWithInput) {    FileTaskStore store(store_path);
    store.CreateTask("task-1");
    ASSERT_TRUE(store.SetTaskStatus("task-1", TaskStatus::InputRequired));

    JsonValue responses(JsonValue::object_tag);
    responses["name"] = JsonValue("Alice");
    EXPECT_TRUE(store.AdvanceTaskWithInput("task-1", responses));

    auto task = store.GetTask("task-1");
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->status, TaskStatus::Working);
    ASSERT_TRUE(task->input_responses.has_value());
    auto* name_entry = task->input_responses->Find("name");
    ASSERT_NE(name_entry, nullptr);
    EXPECT_EQ(name_entry->GetString(), "Alice");
    ASSERT_TRUE(task->last_updated_at.has_value());

    EXPECT_FALSE(store.AdvanceTaskWithInput("task-1", responses));
    EXPECT_FALSE(store.AdvanceTaskWithInput("missing", responses));
}

TEST_F(FileTaskStoreTest, TaskStoreOldFileCompat) {
    {
        std::ofstream ofs(store_path);
        ASSERT_TRUE(ofs.is_open());
        ofs << R"({"tasks":{"task-old":{"task_id":"task-old","status":1,)"
               R"("result":{"answer":7},"progress":0.0,"created_at":"1700000000000"}}})";
    }

    {
        FileTaskStore store(store_path);
        auto task = store.GetTask("task-old");
        ASSERT_TRUE(task.has_value());
        EXPECT_EQ(task->task_id, "task-old");
        EXPECT_EQ(task->status, TaskStatus::Working);
        ASSERT_TRUE(task->result.has_value());
        EXPECT_EQ(task->result->At("answer").GetInt(), 7);
        EXPECT_FALSE(task->status_message.has_value());
        EXPECT_FALSE(task->last_updated_at.has_value());
        EXPECT_FALSE(task->ttl_ms.has_value());
        EXPECT_FALSE(task->poll_interval_ms.has_value());
        EXPECT_FALSE(task->input_requests.has_value());
        EXPECT_FALSE(task->input_responses.has_value());
    }

    {
        std::ofstream ofs(store_path, std::ios::trunc);
        ASSERT_TRUE(ofs.is_open());
        ofs << R"({"tasks":{"task-new":{"task_id":"task-new","status":5,)"
               R"("progress":0.0,"created_at":"1700000000000",)"
               R"("statusMessage":"waiting","lastUpdatedAt":"1700000001000",)"
               R"("ttlMs":3000,"pollIntervalMs":250,)"
               R"("inputRequests":{"name":{"method":"elicitation/create",)"
               R"("params":{"message":"hi"}}},)"
               R"("inputResponses":{"name":"Alice"}}}})";
    }

    {
        FileTaskStore store(store_path);
        auto task = store.GetTask("task-new");
        ASSERT_TRUE(task.has_value());
        EXPECT_EQ(task->status, TaskStatus::InputRequired);
        ASSERT_TRUE(task->status_message.has_value());
        EXPECT_EQ(*task->status_message, "waiting");
        ASSERT_TRUE(task->last_updated_at.has_value());
        EXPECT_EQ(*task->last_updated_at, "1700000001000");
        ASSERT_TRUE(task->ttl_ms.has_value());
        EXPECT_EQ(*task->ttl_ms, 3000);
        ASSERT_TRUE(task->poll_interval_ms.has_value());
        EXPECT_EQ(*task->poll_interval_ms, 250);
        ASSERT_TRUE(task->input_requests.has_value());
        auto it = task->input_requests->find("name");
        ASSERT_TRUE(it != task->input_requests->end());
        EXPECT_EQ(it->second.method, "elicitation/create");
        ASSERT_TRUE(task->input_responses.has_value());
        EXPECT_EQ(task->input_responses->Find("name")->GetString(), "Alice");

        store.CreateTask("task-other");
    }

    {
        FileTaskStore restored(store_path);
        auto task = restored.GetTask("task-new");
        ASSERT_TRUE(task.has_value());
        EXPECT_EQ(task->status, TaskStatus::InputRequired);
        ASSERT_TRUE(task->status_message.has_value());
        EXPECT_EQ(*task->status_message, "waiting");
        ASSERT_TRUE(task->ttl_ms.has_value());
        EXPECT_EQ(*task->ttl_ms, 3000);
        ASSERT_TRUE(task->poll_interval_ms.has_value());
        EXPECT_EQ(*task->poll_interval_ms, 250);
        ASSERT_TRUE(task->input_requests.has_value());
        EXPECT_TRUE(task->input_requests->count("name") > 0u);
        ASSERT_TRUE(task->input_responses.has_value());
        EXPECT_EQ(task->input_responses->Find("name")->GetString(), "Alice");
        EXPECT_TRUE(restored.GetTask("task-other").has_value());
    }
}

TEST_F(FileTaskStoreTest, ParkTaskForInput) {
    FileTaskStore store(store_path);
    store.CreateTask("task-1");

    InputRequests requests;
    ElicitRequestParams params;
    params.message = "Need it";
    requests["first"] = MakeInputRequestForElicitation(params);
    requests["second"] = MakeInputRequestForElicitation(params);
    EXPECT_TRUE(store.ParkTaskForInput("task-1", requests));

    auto task = store.GetTask("task-1");
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->status, TaskStatus::InputRequired);
    ASSERT_TRUE(task->input_requests.has_value());
    EXPECT_EQ(task->input_requests->size(), size_t{2});
    ASSERT_TRUE(task->last_updated_at.has_value());

    EXPECT_FALSE(store.ParkTaskForInput("missing", requests));

    JsonValue result(JsonValue::object_tag);
    ASSERT_TRUE(store.UpdateTask("task-1", result));
    EXPECT_FALSE(store.ParkTaskForInput("task-1", requests));
}

TEST_F(FileTaskStoreTest, AdvanceTaskWithInputMergesResponses) {
    FileTaskStore store(store_path);
    store.CreateTask("task-1");

    InputRequests requests;
    ElicitRequestParams params;
    params.message = "Need it";
    requests["first"] = MakeInputRequestForElicitation(params);
    requests["second"] = MakeInputRequestForElicitation(params);
    ASSERT_TRUE(store.ParkTaskForInput("task-1", requests));

    JsonValue first(JsonValue::object_tag);
    first["first"] = JsonValue("one");
    EXPECT_TRUE(store.AdvanceTaskWithInput("task-1", first));

    InputRequests pruned;
    pruned["second"] = MakeInputRequestForElicitation(params);
    ASSERT_TRUE(store.ParkTaskForInput("task-1", pruned));

    JsonValue second(JsonValue::object_tag);
    second["second"] = JsonValue("two");
    second["unknown-key"] = JsonValue(true);
    EXPECT_TRUE(store.AdvanceTaskWithInput("task-1", second));

    auto task = store.GetTask("task-1");
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->status, TaskStatus::Working);
    ASSERT_TRUE(task->input_responses.has_value());
    auto* first_entry = task->input_responses->Find("first");
    ASSERT_NE(first_entry, nullptr);
    EXPECT_EQ(first_entry->GetString(), "one");
    auto* second_entry = task->input_responses->Find("second");
    ASSERT_NE(second_entry, nullptr);
    EXPECT_EQ(second_entry->GetString(), "two");
    EXPECT_NE(task->input_responses->Find("unknown-key"), nullptr);
}

TEST_F(FileTaskStoreTest, BindTaskToolPersistsAcrossInstances) {
    {
        FileTaskStore store(store_path);
        store.CreateTask("task-1");
        JsonValue args(JsonValue::object_tag);
        args["filename"] = JsonValue("a.txt");
        EXPECT_TRUE(store.BindTaskTool("task-1", "confirm_delete", args));
        EXPECT_FALSE(store.BindTaskTool("missing", "confirm_delete", std::nullopt));
    }

    FileTaskStore restored(store_path);
    auto task = restored.GetTask("task-1");
    ASSERT_TRUE(task.has_value());
    ASSERT_TRUE(task->tool_name.has_value());
    EXPECT_EQ(*task->tool_name, "confirm_delete");
    ASSERT_TRUE(task->tool_arguments.has_value());
    auto* filename = task->tool_arguments->Find("filename");
    ASSERT_NE(filename, nullptr);
    EXPECT_EQ(filename->GetString(), "a.txt");
}

TEST_F(FileTaskStoreTest, TaskStoreOldFileWithoutToolBinding) {
    {
        std::ofstream ofs(store_path);
        ASSERT_TRUE(ofs.is_open());
        ofs << R"({"tasks":{"task-old":{"task_id":"task-old","status":1,)"
               R"("result":{"answer":7},"progress":0.0,"created_at":"1700000000000"}}})";
    }

    FileTaskStore store(store_path);
    auto task = store.GetTask("task-old");
    ASSERT_TRUE(task.has_value());
    EXPECT_FALSE(task->tool_name.has_value());
    EXPECT_FALSE(task->tool_arguments.has_value());
}
