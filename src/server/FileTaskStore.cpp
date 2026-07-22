#include <mcp/storage/FileTaskStore.hpp>
#include <mcp/Log.hpp>

#include <filesystem>
#include <fstream>

namespace mcp {

namespace {

JsonValue SerializeTaskState(const TaskState& state) {
    JsonValue::Object obj;
    obj["task_id"] = JsonValue(state.task_id);
    obj["status"] = JsonValue(static_cast<int64_t>(state.status));
    if (state.result) obj["result"] = *state.result;
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

} // anonymous namespace

FileTaskStore::FileTaskStore(std::filesystem::path storage_path)
    : storage_path_(std::move(storage_path))
{
    if (std::filesystem::exists(storage_path_)) {
        std::ifstream file(storage_path_);
        if (file.is_open()) {
            try {
                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                auto json = JsonValue::Parse(content);
                if (auto* tasks = json.Find("tasks")) {
                    for (auto& [key, val] : *tasks) {
                        tasks_[key] = DeserializeTaskState(val);
                    }
                }
            } catch (...) { MCP_LOG(Warning, "task store parse failed"); }
        }
    }
}

FileTaskStore::~FileTaskStore() {
    Flush();
}

TaskState FileTaskStore::CreateTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    TaskState state;
    state.task_id = task_id;
    state.status = TaskStatus::Pending;
    state.progress = 0;
    state.created_at = std::to_string(now_ms);
    tasks_[task_id] = state;
    Flush();
    return state;
}

std::optional<TaskState> FileTaskStore::GetTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return std::nullopt;
    return it->second;
}

bool FileTaskStore::UpdateTask(
    const std::string& task_id,
    const std::optional<JsonValue>& result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return false;
    it->second.result = result;
    it->second.status = result ? TaskStatus::Completed : TaskStatus::Working;
    Flush();
    return true;
}

bool FileTaskStore::CancelTask(
    const std::string& task_id,
    const std::optional<std::string>& reason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return false;
    it->second.status = TaskStatus::Cancelled;
    it->second.error_message = reason;
    Flush();
    return true;
}

bool FileTaskStore::SetTaskStatus(const std::string& task_id, TaskStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return false;
    it->second.status = status;
    Flush();
    return true;
}

void FileTaskStore::Flush() {
    JsonValue::Object root_obj;
    JsonValue::Object tasks_obj;
    for (auto& [id, state] : tasks_) {
        tasks_obj[id] = SerializeTaskState(state);
    }
    root_obj["tasks"] = JsonValue(std::move(tasks_obj));
    auto tmp_path = storage_path_;
    tmp_path += ".tmp";
    {
        std::ofstream file(tmp_path);
        if (!file.is_open()) return;
        file << JsonValue(std::move(root_obj)).Dump(2);
    }
    std::filesystem::rename(tmp_path, storage_path_);
}

} // namespace mcp
