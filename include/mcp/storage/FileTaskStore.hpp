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

    TaskState CreateTask(const std::string& task_id) override;
    TaskState CreateTask(const std::string& task_id,
                          const std::string& owner,
                          std::optional<int64_t> ttl_ms) override;
    std::optional<TaskState> GetTask(const std::string& task_id) override;
    bool UpdateTask(const std::string& task_id,
                    const std::optional<JsonValue>& result) override;
    bool CancelTask(const std::string& task_id,
                    const std::optional<std::string>& reason) override;
    bool SetTaskStatus(const std::string& task_id, TaskStatus status) override;
    bool AdvanceTaskWithInput(const std::string& task_id,
                              const JsonValue& input_responses) override;
    bool ParkTaskForInput(const std::string& task_id,
                          const InputRequests& input_requests) override;
    bool BindTaskTool(const std::string& task_id,
                      const std::string& tool_name,
                      const std::optional<JsonValue>& tool_arguments) override;
    std::vector<TaskState> GetAllTasks() override;
    bool FailTask(const std::string& task_id, const JsonValue& error_payload) override;
    bool RemoveTask(const std::string& task_id) override;
    std::vector<std::string> GetExpiredTaskIds(int64_t now_ms) override;
    bool SetTaskOwner(const std::string& task_id, const std::string& owner) override;
    bool SetTaskTtl(const std::string& task_id, int64_t ttl_ms) override;

private:
    bool Flush();
    bool AppendRecord(const std::string& task_id, const TaskState& state);
    bool AppendRemoval(const std::string& task_id);
    bool CompactLocked();
    void LoadTasks();
    void LoadRecords(const std::string& content);
    void BackupCorruptFile();

    std::mutex write_mutex_;
    std::shared_mutex data_mutex_;
    std::filesystem::path storage_path_;
    std::unordered_map<std::string, TaskState> tasks_;
    size_t pending_records_{0};
};

} // namespace mcp
