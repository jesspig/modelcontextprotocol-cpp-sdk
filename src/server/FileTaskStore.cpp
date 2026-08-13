// FileTaskStore.cpp - JSON file-backed task store implementation

#include <mcp/storage/FileTaskStore.hpp>
#include <mcp/Log.hpp>
#include <mcp/detail/AtomicJsonFile.hpp>
#include <detail/JsonFields.hpp>

#include <filesystem>
#include <stdexcept>

namespace mcp {

namespace {

JsonValue SerializeTaskState(const TaskState& state) {
    JsonValue::Object obj;
    obj["task_id"] = JsonValue(state.task_id);
    obj["status"] = JsonValue(static_cast<int64_t>(state.status));
    if (state.result) obj[detail::kResult] = *state.result;
    if (state.error_message) obj["error_message"] = JsonValue(*state.error_message);
    if (state.input_required) obj["input_required"] = *state.input_required;
    obj["progress"] = JsonValue(state.progress);
    if (state.progress_total) obj["progress_total"] = JsonValue(*state.progress_total);
    obj["created_at"] = JsonValue(state.created_at);
    return JsonValue(std::move(obj));
}

TaskState DeserializeTaskState(const JsonValue& j) {
    TaskState state;
    if (auto* v = j.Find("task_id")) state.task_id = v->GetString();
    if (auto* v = j.Find("status")) state.status = static_cast<TaskStatus>(v->GetInt());
    if (auto* v = j.Find("result"); v && !v->IsNull())
        state.result = *v;
    if (auto* v = j.Find("error_message"); v && !v->IsNull())
        state.error_message = v->GetString();
    if (auto* v = j.Find("input_required"); v && !v->IsNull())
        state.input_required = *v;
    if (auto* v = j.Find("progress")) state.progress = v->GetDouble();
    if (auto* v = j.Find("progress_total"); v && !v->IsNull())
        state.progress_total = v->GetDouble();
    if (auto* v = j.Find("created_at")) state.created_at = v->GetString();
    return state;
}

JsonValue SerializeTasks(const std::unordered_map<std::string, TaskState>& tasks) {
    JsonValue::Object root_obj;
    JsonValue::Object tasks_obj;
    for (auto& [id, state] : tasks) {
        tasks_obj[id] = SerializeTaskState(state);
    }
    root_obj["tasks"] = JsonValue(std::move(tasks_obj));
    return JsonValue(std::move(root_obj));
}

} // anonymous namespace

FileTaskStore::FileTaskStore(std::filesystem::path storage_path)
    : storage_path_(std::move(storage_path))
{
    try {
        auto json = detail::LoadJson(storage_path_);
        if (auto* tasks = json.Find("tasks")) {
            for (auto& [key, val] : *tasks) {
                tasks_[key] = DeserializeTaskState(val);
            }
        }
    } catch (const std::exception& e) {
        MCP_LOG(Error, "task store: failed to load " + storage_path_.string() + ": " + e.what());
        auto backup_path = storage_path_;
        backup_path += ".corrupt";
        std::error_code ec;
        std::filesystem::copy_file(storage_path_, backup_path,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            MCP_LOG(Error, "task store: failed to back up corrupt file to " + backup_path.string());
        }
    }
}

FileTaskStore::~FileTaskStore() {
    Flush();
}

TaskState FileTaskStore::CreateTask(const std::string& task_id) {
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    TaskState state;
    state.task_id = task_id;
    state.status = TaskStatus::Pending;
    state.progress = 0;
    state.created_at = std::to_string(now_ms);
    std::unordered_map<std::string, TaskState> snapshot;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto [_, inserted] = tasks_.try_emplace(task_id, state);
        if (!inserted) {
            throw std::runtime_error("task already exists: " + task_id);
        }
        snapshot = tasks_;
    }
    if (!PersistTasks(snapshot)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        tasks_.erase(task_id);
        throw std::runtime_error("task store: failed to persist task " + task_id);
    }
    return state;
}

std::optional<TaskState> FileTaskStore::GetTask(const std::string& task_id) {
    std::shared_lock<std::shared_mutex> lock(data_mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return std::nullopt;
    return it->second;
}

bool FileTaskStore::UpdateTask(
    const std::string& task_id,
    const std::optional<JsonValue>& result)
{
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState original;
    std::unordered_map<std::string, TaskState> snapshot;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        original = it->second;
        it->second.result = result;
        it->second.status = result ? TaskStatus::Completed : TaskStatus::Working;
        snapshot = tasks_;
    }
    if (!PersistTasks(snapshot)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw std::runtime_error("task store: failed to persist task " + task_id);
    }
    return true;
}

bool FileTaskStore::CancelTask(
    const std::string& task_id,
    const std::optional<std::string>& reason)
{
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState original;
    std::unordered_map<std::string, TaskState> snapshot;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        original = it->second;
        it->second.status = TaskStatus::Cancelled;
        it->second.error_message = reason;
        snapshot = tasks_;
    }
    if (!PersistTasks(snapshot)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw std::runtime_error("task store: failed to persist task " + task_id);
    }
    return true;
}

bool FileTaskStore::SetTaskStatus(const std::string& task_id, TaskStatus status) {
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState original;
    std::unordered_map<std::string, TaskState> snapshot;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        original = it->second;
        it->second.status = status;
        snapshot = tasks_;
    }
    if (!PersistTasks(snapshot)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw std::runtime_error("task store: failed to persist task " + task_id);
    }
    return true;
}

std::vector<TaskState> FileTaskStore::GetAllTasks() {
    std::shared_lock<std::shared_mutex> lock(data_mutex_);
    std::vector<TaskState> result;
    result.reserve(tasks_.size());
    for (const auto& [_, state] : tasks_) {
        result.push_back(state);
    }
    return result;
}

bool FileTaskStore::Flush() {
    std::unordered_map<std::string, TaskState> snapshot;
    {
        std::shared_lock<std::shared_mutex> data_lock(data_mutex_);
        snapshot = tasks_;
    }
    return PersistTasks(snapshot);
}

bool FileTaskStore::PersistTasks(
    const std::unordered_map<std::string, TaskState>& tasks)
{
    try {
        return detail::WriteAtomic(storage_path_, SerializeTasks(tasks));
    } catch (...) {
        return false;
    }
}

} // namespace mcp
