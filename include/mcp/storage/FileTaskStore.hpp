// FileTaskStore.hpp - Task store persisted to JSON file

#pragma once

#include <mcp/JsonValue.hpp>
#include <mcp/server/McpTaskStore.hpp>

#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace mcp {

class FileTaskStore : public IMcpTaskStore {
public:
    explicit FileTaskStore(std::filesystem::path storage_path);
    ~FileTaskStore() override;

    // IMcpTaskStore interface
    TaskState CreateTask(const std::string& task_id) override;
    std::optional<TaskState> GetTask(const std::string& task_id) override;
    bool UpdateTask(const std::string& task_id,
                    const std::optional<JsonValue>& result) override;
    bool CancelTask(const std::string& task_id,
                    const std::optional<std::string>& reason) override;
    bool SetTaskStatus(const std::string& task_id, TaskStatus status) override;
    std::vector<TaskState> GetAllTasks() override;

private:
    // Returns false if the on-disk persistence failed; callers must propagate.
    bool Flush();
    bool PersistTasks(const std::unordered_map<std::string, TaskState>& tasks);

    // Lock order: write_mutex_ (outer) -> data_mutex_ (inner). Writers hold
    // write_mutex_ for the whole operation including the persist, which runs
    // outside data_mutex_, so readers stay concurrent during disk I/O and a
    // failed persist can roll back without racing a concurrent writer.
    std::mutex write_mutex_;
    std::shared_mutex data_mutex_;
    std::filesystem::path storage_path_;
    std::unordered_map<std::string, TaskState> tasks_;
};

} // namespace mcp
