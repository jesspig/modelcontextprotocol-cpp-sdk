#pragma once

#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp {

enum class TaskStatus {
    Pending,
    Working,
    Completed,
    Failed,
    Cancelled,
    InputRequired,
};

struct TaskState {
    std::string task_id;
    std::string owner;
    TaskStatus status{TaskStatus::Pending};
    std::optional<JsonValue> result;
    std::optional<std::string> error_message;
    std::optional<JsonValue> input_required;
    double progress{0};
    std::optional<double> progress_total;
    std::string created_at;
    std::optional<std::string> status_message;
    std::optional<std::string> last_updated_at;
    std::optional<int64_t> ttl_ms;
    std::optional<int64_t> poll_interval_ms;
    std::optional<InputRequests> input_requests;
    std::optional<JsonValue> input_responses;
    std::optional<std::string> tool_name;
    std::optional<JsonValue> tool_arguments;
};

class IMcpTaskStore {
public:
    virtual ~IMcpTaskStore() = default;

    virtual TaskState CreateTask(const std::string& task_id) = 0;
    virtual TaskState CreateTask(const std::string& task_id,
                                  const std::string& owner,
                                  std::optional<int64_t> ttl_ms)
    {
        TaskState state = CreateTask(task_id);
        if (!owner.empty()) SetTaskOwner(task_id, owner);
        if (ttl_ms) SetTaskTtl(task_id, *ttl_ms);
        return state;
    }
    virtual std::optional<TaskState> GetTask(const std::string& task_id) = 0;
    virtual bool UpdateTask(const std::string& task_id,
                            const std::optional<JsonValue>& result) = 0;
    virtual bool CancelTask(const std::string& task_id,
                            const std::optional<std::string>& reason) = 0;
    virtual bool SetTaskStatus(const std::string& task_id, TaskStatus status) = 0;
    virtual bool AdvanceTaskWithInput(const std::string&, const JsonValue&) { return false; }
    virtual bool ParkTaskForInput(const std::string&, const InputRequests&) { return false; }
    virtual bool BindTaskTool(const std::string&, const std::string&,
                              const std::optional<JsonValue>&) { return false; }
    virtual std::vector<TaskState> GetAllTasks() = 0;
    virtual bool FailTask(const std::string& task_id, const JsonValue& error_payload)
    {
        (void)task_id;
        (void)error_payload;
        return false;
    }
    virtual bool RemoveTask(const std::string& task_id)
    {
        (void)task_id;
        return false;
    }
    virtual std::vector<std::string> GetExpiredTaskIds(int64_t now_ms)
    {
        (void)now_ms;
        return {};
    }
    virtual bool SetTaskOwner(const std::string& task_id, const std::string& owner)
    {
        (void)task_id;
        (void)owner;
        return false;
    }
    virtual bool SetTaskTtl(const std::string& task_id, int64_t ttl_ms)
    {
        (void)task_id;
        (void)ttl_ms;
        return false;
    }
};

} // namespace mcp
