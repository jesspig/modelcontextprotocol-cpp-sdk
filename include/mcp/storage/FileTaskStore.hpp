#pragma once

#include <mcp/JsonValue.hpp>
#include <mcp/server/McpTaskStore.hpp>

#include <filesystem>
#include <mutex>
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

private:
    void Flush();
    TaskState& GetOrCreate(const std::string& task_id);

    std::filesystem::path storage_path_;
    std::unordered_map<std::string, TaskState> tasks_;
    std::mutex mutex_;
};

} // namespace mcp
