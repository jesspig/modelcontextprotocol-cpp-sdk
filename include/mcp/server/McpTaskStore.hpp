#pragma once

#include <mcp/McpTypes.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mcp {

// ── Task status values ──
enum class TaskStatus {
    Pending,
    Working,
    Completed,
    Failed,
    Cancelled,
    InputRequired,
};

// ── TaskState — full task state (persisted by store) ──
struct TaskState {
    std::string task_id;
    TaskStatus status{TaskStatus::Pending};
    std::optional<nlohmann::json> result;
    std::optional<std::string> error_message;
    std::optional<nlohmann::json> input_required;
    double progress{0};
    std::optional<double> progress_total;
    std::string created_at;
};

// ── IMcpTaskStore — interface for managing task lifecycle ──
// Corresponds conceptually to C# IMcpTaskStore.
class IMcpTaskStore {
public:
    virtual ~IMcpTaskStore() = default;

    virtual TaskState CreateTask(const std::string& task_id) = 0;
    virtual std::optional<TaskState> GetTask(const std::string& task_id) = 0;
    virtual bool UpdateTask(const std::string& task_id,
                            const std::optional<nlohmann::json>& result) = 0;
    virtual bool CancelTask(const std::string& task_id,
                            const std::optional<std::string>& reason) = 0;
    virtual bool SetTaskStatus(const std::string& task_id, TaskStatus status) = 0;
};

// ── InMemoryTaskStore — default implementation ──
class InMemoryTaskStore : public IMcpTaskStore {
public:
    TaskState CreateTask(const std::string& task_id) override;
    std::optional<TaskState> GetTask(const std::string& task_id) override;
    bool UpdateTask(const std::string& task_id,
                    const std::optional<nlohmann::json>& result) override;
    bool CancelTask(const std::string& task_id,
                    const std::optional<std::string>& reason) override;
    bool SetTaskStatus(const std::string& task_id, TaskStatus status) override;

private:
    std::unordered_map<std::string, TaskState> tasks_;
    std::mutex mutex_;
};

} // namespace mcp
