#include <mcp/storage/FileTaskStore.hpp>

#include <filesystem>
#include <fstream>

namespace mcp {

namespace {

nlohmann::json SerializeTaskState(const TaskState& state) {
    nlohmann::json j;
    j["task_id"] = state.task_id;
    j["status"] = static_cast<int>(state.status);
    if (state.result) j["result"] = *state.result;
    if (state.error_message) j["error_message"] = *state.error_message;
    if (state.input_required) j["input_required"] = *state.input_required;
    j["progress"] = state.progress;
    if (state.progress_total) j["progress_total"] = *state.progress_total;
    j["created_at"] = state.created_at;
    return j;
}

TaskState DeserializeTaskState(const nlohmann::json& j) {
    TaskState state;
    state.task_id = j.value("task_id", "");
    state.status = static_cast<TaskStatus>(j.value("status", 0));
    if (j.contains("result") && !j["result"].is_null())
        state.result = j["result"];
    if (j.contains("error_message") && !j["error_message"].is_null())
        state.error_message = j["error_message"].get<std::string>();
    if (j.contains("input_required") && !j["input_required"].is_null())
        state.input_required = j["input_required"];
    state.progress = j.value("progress", 0.0);
    if (j.contains("progress_total") && !j["progress_total"].is_null())
        state.progress_total = j["progress_total"].get<double>();
    state.created_at = j.value("created_at", "");
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
                auto json = nlohmann::json::parse(file, nullptr, false);
                if (!json.is_discarded() && json.contains("tasks")) {
                    for (auto& [key, val] : json["tasks"].items()) {
                        tasks_[key] = DeserializeTaskState(val);
                    }
                }
            } catch (...) {
                // Corrupted file; start fresh
            }
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
    const std::optional<nlohmann::json>& result)
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
    nlohmann::json j;
    for (auto& [id, state] : tasks_) {
        j["tasks"][id] = SerializeTaskState(state);
    }
    std::ofstream file(storage_path_);
    if (file.is_open()) {
        file << j.dump(2);
    }
}

} // namespace mcp
