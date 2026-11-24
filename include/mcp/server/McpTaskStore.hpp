#pragma once

#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp {

inline constexpr size_t kMaxTasksPerPrincipal = 10000;
inline constexpr size_t kMaxTaskResultBytes = 8388608;
inline constexpr size_t kMaxTaskNotificationBytes = 1048576;
inline constexpr size_t kMaxTaskIdsPerSubscription = 1024;

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
        if (!owner.empty()) (void)SetTaskOwner(task_id, owner);
        if (ttl_ms) (void)SetTaskTtl(task_id, *ttl_ms);
        return state;
    }
    [[nodiscard]] virtual std::optional<TaskState> GetTask(const std::string& task_id) = 0;
    [[nodiscard]] virtual bool UpdateTask(const std::string& task_id,
                                          const std::optional<JsonValue>& result) = 0;
    [[nodiscard]] virtual bool CancelTask(const std::string& task_id,
                                          const std::optional<std::string>& reason) = 0;
    [[nodiscard]] virtual bool SetTaskStatus(const std::string& task_id, TaskStatus status) = 0;
    [[nodiscard]] virtual bool AdvanceTaskWithInput(const std::string&, const JsonValue&) { return false; }
    [[nodiscard]] virtual bool ParkTaskForInput(const std::string&, const InputRequests&) { return false; }
    [[nodiscard]] virtual bool BindTaskTool(const std::string&, const std::string&,
                                            const std::optional<JsonValue>&) { return false; }
    virtual std::vector<TaskState> GetAllTasks() = 0;
    [[nodiscard]] virtual bool FailTask(const std::string& task_id, const JsonValue& error_payload)
    {
        (void)task_id;
        (void)error_payload;
        return false;
    }
    [[nodiscard]] virtual bool RemoveTask(const std::string& task_id)
    {
        (void)task_id;
        return false;
    }
    virtual std::vector<std::string> GetExpiredTaskIds(int64_t now_ms)
    {
        (void)now_ms;
        return {};
    }
    [[nodiscard]] virtual bool SetTaskOwner(const std::string& task_id, const std::string& owner)
    {
        (void)task_id;
        (void)owner;
        return false;
    }
    [[nodiscard]] virtual bool SetTaskTtl(const std::string& task_id, int64_t ttl_ms)
    {
        (void)task_id;
        (void)ttl_ms;
        return false;
    }
};

class InMemoryTaskStore : public IMcpTaskStore {
public:
    TaskState CreateTask(const std::string& task_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tasks_.count(task_id) > 0) {
            throw McpError(McpErrorCode::InvalidParams,
                "task already exists: " + task_id);
        }
        TaskState state;
        state.task_id = task_id;
        state.status = TaskStatus::Pending;
        tasks_[task_id] = state;
        return state;
    }

    TaskState CreateTask(const std::string& task_id,
                         const std::string& owner,
                         std::optional<int64_t> ttl_ms) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tasks_.count(task_id) > 0) {
            throw McpError(McpErrorCode::InvalidParams,
                "task already exists: " + task_id);
        }
        TaskState state;
        state.task_id = task_id;
        state.owner = owner;
        state.ttl_ms = ttl_ms;
        state.status = TaskStatus::Pending;
        tasks_[task_id] = state;
        return state;
    }

    std::optional<TaskState> GetTask(const std::string& task_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return std::nullopt;
        return it->second;
    }

    bool UpdateTask(const std::string& task_id,
                    const std::optional<JsonValue>& result) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end() || IsTerminal(it->second.status)) return false;
        it->second.result = result;
        it->second.status = result ? TaskStatus::Completed : TaskStatus::Working;
        return true;
    }

    bool CancelTask(const std::string& task_id,
                    const std::optional<std::string>& reason) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end() || IsTerminal(it->second.status)) return false;
        it->second.status = TaskStatus::Cancelled;
        it->second.error_message = reason;
        return true;
    }

    bool SetTaskStatus(const std::string& task_id, TaskStatus status) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        if (IsTerminal(it->second.status) && it->second.status != status) return false;
        it->second.status = status;
        return true;
    }

    std::vector<TaskState> GetAllTasks() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TaskState> result;
        result.reserve(tasks_.size());
        for (const auto& [_, state] : tasks_) result.push_back(state);
        return result;
    }

    bool FailTask(const std::string& task_id, const JsonValue& error_payload) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end() || IsTerminal(it->second.status)) return false;
        it->second.result = error_payload;
        it->second.status = TaskStatus::Failed;
        return true;
    }

    bool RemoveTask(const std::string& task_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.erase(task_id) > 0;
    }

    bool SetTaskOwner(const std::string& task_id, const std::string& owner) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        it->second.owner = owner;
        return true;
    }

    bool SetTaskTtl(const std::string& task_id, int64_t ttl_ms) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        it->second.ttl_ms = ttl_ms;
        return true;
    }

private:
    static bool IsTerminal(TaskStatus status) {
        return status == TaskStatus::Completed || status == TaskStatus::Failed ||
            status == TaskStatus::Cancelled;
    }

    std::mutex mutex_;
    std::unordered_map<std::string, TaskState> tasks_;
};

} // namespace mcp
