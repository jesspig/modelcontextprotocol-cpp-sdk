#include <mcp/storage/FileTaskStore.hpp>
#include <mcp/Log.hpp>
#include <mcp/McpError.hpp>
#include <mcp/detail/AtomicJsonFile.hpp>
#include <detail/JsonFields.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

namespace {

constexpr size_t kMinCompactThreshold = 64;

std::string MakeMillisNow() {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(now_ms);
}

std::optional<int64_t> ParseMillis(const std::string& value) {
    if (value.empty()) return std::nullopt;
    try {
        size_t consumed = 0;
        auto parsed = std::stoll(value, &consumed);
        if (consumed != value.size()) return std::nullopt;
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool IsTerminalStatus(TaskStatus status) {
    return status == TaskStatus::Completed
        || status == TaskStatus::Failed
        || status == TaskStatus::Cancelled;
}

JsonValue SerializeTaskState(const TaskState& state) {
    JsonValue::Object obj;
    obj["task_id"] = JsonValue(state.task_id);
    obj["owner"] = JsonValue(state.owner);
    obj["status"] = JsonValue(static_cast<int64_t>(state.status));
    if (state.result) obj[detail::kResult] = *state.result;
    if (state.error_message) obj["error_message"] = JsonValue(*state.error_message);
    if (state.input_required) obj["input_required"] = *state.input_required;
    obj["progress"] = JsonValue(state.progress);
    if (state.progress_total) obj["progress_total"] = JsonValue(*state.progress_total);
    obj["created_at"] = JsonValue(state.created_at);
    if (state.status_message) obj[detail::kStatusMessage] = JsonValue(*state.status_message);
    if (state.last_updated_at) obj[detail::kLastUpdatedAt] = JsonValue(*state.last_updated_at);
    if (state.ttl_ms) obj[detail::kTTLMs] = JsonValue(*state.ttl_ms);
    if (state.poll_interval_ms) obj[detail::kPollIntervalMs] = JsonValue(*state.poll_interval_ms);
    if (state.input_requests) obj[detail::kInputRequests] = SerializeInputRequests(*state.input_requests);
    if (state.input_responses) obj[detail::kInputResponses] = *state.input_responses;
    if (state.tool_name) obj["toolName"] = JsonValue(*state.tool_name);
    if (state.tool_arguments) obj["toolArguments"] = *state.tool_arguments;
    return JsonValue(std::move(obj));
}

TaskState DeserializeTaskState(const JsonValue& j) {
    TaskState state;
    if (auto* v = j.Find("task_id")) state.task_id = v->GetString();
    if (auto* v = j.Find("owner"); v && v->IsString()) state.owner = v->GetString();
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
    if (auto* v = j.Find(detail::kStatusMessage); v && v->IsString())
        state.status_message = v->GetString();
    if (auto* v = j.Find(detail::kLastUpdatedAt); v && v->IsString())
        state.last_updated_at = v->GetString();
    if (auto* v = j.Find(detail::kTTLMs); v && v->IsInt())
        state.ttl_ms = v->GetInt();
    if (auto* v = j.Find(detail::kPollIntervalMs); v && v->IsInt())
        state.poll_interval_ms = v->GetInt();
    if (auto* v = j.Find(detail::kInputRequests); v && v->IsObject())
        state.input_requests = DeserializeInputRequests(*v);
    if (auto* v = j.Find(detail::kInputResponses); v && !v->IsNull())
        state.input_responses = *v;
    if (auto* v = j.Find("toolName"); v && v->IsString())
        state.tool_name = v->GetString();
    if (auto* v = j.Find("toolArguments"); v && !v->IsNull())
        state.tool_arguments = *v;
    return state;
}

std::string SerializeRecord(const std::string& task_id, const TaskState& state) {
    JsonValue::Object obj;
    obj["task_id"] = JsonValue(task_id);
    obj["state"] = SerializeTaskState(state);
    return JsonValue(std::move(obj)).Dump();
}

std::string SerializeRemoval(const std::string& task_id) {
    JsonValue::Object obj;
    obj["task_id"] = JsonValue(task_id);
    obj["removed"] = JsonValue(true);
    return JsonValue(std::move(obj)).Dump();
}

} // anonymous namespace

FileTaskStore::FileTaskStore(std::filesystem::path storage_path)
    : storage_path_(std::move(storage_path))
{
    LoadTasks();
}

FileTaskStore::~FileTaskStore() {
    Flush();
}

void FileTaskStore::BackupCorruptFile() {
    MCP_LOG(Error, "task store: failed to load " + storage_path_.string());
    auto backup_path = storage_path_;
    backup_path += ".corrupt";
    std::error_code ec;
    std::filesystem::copy_file(storage_path_, backup_path,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        MCP_LOG(Error, "task store: failed to back up corrupt file to " + backup_path.string());
    }
}

void FileTaskStore::LoadTasks() {
    std::error_code ec;
    if (!std::filesystem::exists(storage_path_, ec)) return;
    std::ifstream file(storage_path_, std::ios::binary);
    if (!file.is_open()) {
        BackupCorruptFile();
        return;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    if (content.empty()) return;

    try {
        JsonValue json = JsonValue::Parse(content);
        if (json.IsObject()) {
            if (auto* tasks = json.Find("tasks"); tasks && tasks->IsObject()) {
                for (auto& [key, val] : *tasks) {
                    tasks_[key] = DeserializeTaskState(val);
                }
                return;
            }
        }
    } catch (const std::exception&) {
    }

    LoadRecords(content);
}

void FileTaskStore::LoadRecords(const std::string& content) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= content.size()) {
        auto pos = content.find('\n', start);
        if (pos == std::string::npos) {
            lines.push_back(content.substr(start));
            break;
        }
        lines.push_back(content.substr(start, pos - start));
        start = pos + 1;
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty()) continue;
        JsonValue json;
        try {
            json = JsonValue::Parse(line);
        } catch (const std::exception&) {
            if (i + 1 == lines.size()) {
                MCP_LOG(Error, "task store: ignoring truncated trailing record in " +
                    storage_path_.string());
                break;
            }
            BackupCorruptFile();
            tasks_.clear();
            return;
        }
        if (!json.IsObject()) {
            BackupCorruptFile();
            tasks_.clear();
            return;
        }
        if (auto* legacy = json.Find("tasks"); legacy && legacy->IsObject()) {
            for (auto& [key, val] : *legacy) {
                tasks_[key] = DeserializeTaskState(val);
            }
            continue;
        }
        auto* id = json.Find("task_id");
        if (!id || !id->IsString() || id->GetString().empty()) {
            BackupCorruptFile();
            tasks_.clear();
            return;
        }
        if (auto* removed = json.Find("removed"); removed && removed->GetBool()) {
            tasks_.erase(id->GetString());
            continue;
        }
        auto* state = json.Find("state");
        if (!state || !state->IsObject()) {
            BackupCorruptFile();
            tasks_.clear();
            return;
        }
        tasks_[id->GetString()] = DeserializeTaskState(*state);
        ++pending_records_;
    }
}

TaskState FileTaskStore::CreateTask(const std::string& task_id) {
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState state;
    state.task_id = task_id;
    state.status = TaskStatus::Pending;
    state.progress = 0;
    state.created_at = MakeMillisNow();
    state.last_updated_at = state.created_at;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        if (tasks_.find(task_id) != tasks_.end()) {
            throw McpError(McpErrorCode::InvalidParams,
                "task already exists: " + task_id);
        }
        tasks_[task_id] = state;
    }
    if (!AppendRecord(task_id, state)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        tasks_.erase(task_id);
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist task " + task_id);
    }
    return state;
}

TaskState FileTaskStore::CreateTask(const std::string& task_id,
                                     const std::string& owner,
                                     std::optional<int64_t> ttl_ms)
{
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState state;
    state.task_id = task_id;
    state.owner = owner;
    state.status = TaskStatus::Pending;
    state.progress = 0;
    state.created_at = MakeMillisNow();
    state.last_updated_at = state.created_at;
    state.ttl_ms = ttl_ms;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        if (tasks_.find(task_id) != tasks_.end()) {
            throw McpError(McpErrorCode::InvalidParams,
                "task already exists: " + task_id);
        }
        tasks_[task_id] = state;
    }
    if (!AppendRecord(task_id, state)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        tasks_.erase(task_id);
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist task " + task_id);
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
    TaskState updated;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        if (IsTerminalStatus(it->second.status)) return false;
        original = it->second;
        updated = original;
        updated.result = result;
        updated.status = result ? TaskStatus::Completed : TaskStatus::Working;
        updated.last_updated_at = MakeMillisNow();
        it->second = updated;
    }
    if (!AppendRecord(task_id, updated)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist task " + task_id);
    }
    return true;
}

bool FileTaskStore::CancelTask(
    const std::string& task_id,
    const std::optional<std::string>& reason)
{
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState original;
    TaskState updated;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        if (IsTerminalStatus(it->second.status)) return false;
        original = it->second;
        updated = original;
        updated.status = TaskStatus::Cancelled;
        updated.error_message = reason;
        updated.last_updated_at = MakeMillisNow();
        it->second = updated;
    }
    if (!AppendRecord(task_id, updated)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist task " + task_id);
    }
    return true;
}

bool FileTaskStore::SetTaskStatus(const std::string& task_id, TaskStatus status) {
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState original;
    TaskState updated;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        if (IsTerminalStatus(it->second.status) && it->second.status != status) {
            return false;
        }
        original = it->second;
        updated = original;
        updated.status = status;
        updated.last_updated_at = MakeMillisNow();
        it->second = updated;
    }
    if (!AppendRecord(task_id, updated)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist task " + task_id);
    }
    return true;
}

bool FileTaskStore::FailTask(
    const std::string& task_id,
    const JsonValue& error_payload)
{
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState original;
    TaskState updated;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        if (IsTerminalStatus(it->second.status)) return false;
        original = it->second;
        updated = original;
        updated.result = error_payload;
        updated.status = TaskStatus::Failed;
        if (auto* msg = error_payload.Find(detail::kMessage);
                msg && msg->IsString()) {
            updated.error_message = msg->GetString();
        }
        updated.last_updated_at = MakeMillisNow();
        it->second = updated;
    }
    if (!AppendRecord(task_id, updated)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist task " + task_id);
    }
    return true;
}

bool FileTaskStore::SetTaskOwner(const std::string& task_id,
                                  const std::string& owner)
{
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState original;
    TaskState updated;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        original = it->second;
        updated = original;
        updated.owner = owner;
        it->second = updated;
    }
    if (!AppendRecord(task_id, updated)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist task " + task_id);
    }
    return true;
}

bool FileTaskStore::SetTaskTtl(const std::string& task_id, int64_t ttl_ms) {
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState original;
    TaskState updated;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        original = it->second;
        updated = original;
        updated.ttl_ms = ttl_ms;
        it->second = updated;
    }
    if (!AppendRecord(task_id, updated)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist task " + task_id);
    }
    return true;
}

bool FileTaskStore::RemoveTask(const std::string& task_id) {
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    {
        std::shared_lock<std::shared_mutex> data_lock(data_mutex_);
        if (tasks_.find(task_id) == tasks_.end()) return false;
    }
    if (!AppendRemoval(task_id)) {
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist removal of task " + task_id);
    }
    std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
    tasks_.erase(task_id);
    return true;
}

std::vector<std::string> FileTaskStore::GetExpiredTaskIds(int64_t now_ms) {
    std::shared_lock<std::shared_mutex> lock(data_mutex_);
    std::vector<std::string> expired;
    for (const auto& [task_id, state] : tasks_) {
        if (!state.ttl_ms || *state.ttl_ms <= 0) continue;
        auto created_at = ParseMillis(state.created_at);
        if (!created_at) continue;
        if (now_ms - *created_at >= *state.ttl_ms) expired.push_back(task_id);
    }
    return expired;
}

bool FileTaskStore::AdvanceTaskWithInput(
    const std::string& task_id,
    const JsonValue& input_responses)
{
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState original;
    TaskState updated;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        if (it->second.status != TaskStatus::InputRequired) return false;
        original = it->second;
        updated = original;
        if (updated.input_responses && updated.input_responses->IsObject()
            && input_responses.IsObject()) {
            for (const auto& [key, value] : input_responses.GetObject()) {
                (*updated.input_responses)[key] = value;
            }
        } else {
            updated.input_responses = input_responses;
        }
        updated.status = TaskStatus::Working;
        updated.last_updated_at = MakeMillisNow();
        it->second = updated;
    }
    if (!AppendRecord(task_id, updated)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist task " + task_id);
    }
    return true;
}

bool FileTaskStore::ParkTaskForInput(
    const std::string& task_id,
    const InputRequests& input_requests)
{
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState original;
    TaskState updated;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        if (IsTerminalStatus(it->second.status)) return false;
        original = it->second;
        updated = original;
        updated.input_requests = input_requests;
        updated.status = TaskStatus::InputRequired;
        updated.last_updated_at = MakeMillisNow();
        it->second = updated;
    }
    if (!AppendRecord(task_id, updated)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist task " + task_id);
    }
    return true;
}

bool FileTaskStore::BindTaskTool(
    const std::string& task_id,
    const std::string& tool_name,
    const std::optional<JsonValue>& tool_arguments)
{
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    TaskState original;
    TaskState updated;
    {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;
        original = it->second;
        updated = original;
        updated.tool_name = tool_name;
        updated.tool_arguments = tool_arguments;
        it->second = updated;
    }
    if (!AppendRecord(task_id, updated)) {
        std::unique_lock<std::shared_mutex> data_lock(data_mutex_);
        auto it = tasks_.find(task_id);
        if (it != tasks_.end()) it->second = std::move(original);
        throw McpError(McpErrorCode::InternalError,
            "task store: failed to persist task " + task_id);
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
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    return CompactLocked();
}

bool FileTaskStore::AppendRecord(const std::string& task_id, const TaskState& state) {
    std::ofstream file(storage_path_, std::ios::app | std::ios::binary);
    if (!file.is_open()) {
        MCP_LOG(Error, "task store: failed to open " + storage_path_.string() +
            " for append");
        return false;
    }
    file << SerializeRecord(task_id, state) << '\n';
    file.flush();
    if (!file.good()) {
        MCP_LOG(Error, "task store: failed to append task record to " +
            storage_path_.string());
        return false;
    }
    ++pending_records_;
    size_t live_count = 0;
    {
        std::shared_lock<std::shared_mutex> data_lock(data_mutex_);
        live_count = tasks_.size();
    }
    if (pending_records_ > kMinCompactThreshold + live_count * 2) {
        return CompactLocked();
    }
    return true;
}

bool FileTaskStore::AppendRemoval(const std::string& task_id) {
    std::ofstream file(storage_path_, std::ios::app | std::ios::binary);
    if (!file.is_open()) {
        MCP_LOG(Error, "task store: failed to open " + storage_path_.string() +
            " for append");
        return false;
    }
    file << SerializeRemoval(task_id) << '\n';
    file.flush();
    if (!file.good()) {
        MCP_LOG(Error, "task store: failed to append task removal to " +
            storage_path_.string());
        return false;
    }
    ++pending_records_;
    return true;
}

bool FileTaskStore::CompactLocked() {
    std::string content;
    {
        std::shared_lock<std::shared_mutex> data_lock(data_mutex_);
        for (const auto& [task_id, state] : tasks_) {
            content += SerializeRecord(task_id, state);
            content += '\n';
        }
    }
    if (!detail::WriteAtomic(storage_path_, content)) {
        MCP_LOG(Error, "task store: failed to persist " + storage_path_.string());
        return false;
    }
    pending_records_ = 0;
    return true;
}

} // namespace mcp
