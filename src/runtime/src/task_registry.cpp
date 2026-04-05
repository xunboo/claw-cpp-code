#include "task_registry.hpp"
#include <chrono>
#include <format>

namespace claw::runtime {

namespace {

uint64_t now_secs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

} // anonymous namespace

std::string_view task_status_name(TaskStatus s) noexcept {
    switch (s) {
        case TaskStatus::Created:   return "created";
        case TaskStatus::Running:   return "running";
        case TaskStatus::Completed: return "completed";
        case TaskStatus::Failed:    return "failed";
        case TaskStatus::Stopped:   return "stopped";
    }
    return "unknown";
}

Task TaskRegistry::create(std::string_view prompt, std::optional<std::string_view> description) {
    std::lock_guard lock(mutex_);
    ++inner_.counter;
    uint64_t ts = now_secs();
    std::string task_id = std::format("task_{:08x}_{}", ts, inner_.counter);
    Task task{
        .task_id = task_id,
        .prompt = std::string(prompt),
        .description = description.has_value() ? std::optional<std::string>(std::string(*description)) : std::nullopt,
        .task_packet = std::nullopt,
        .status = TaskStatus::Created,
        .created_at = ts,
        .updated_at = ts,
    };
    inner_.tasks.emplace(task_id, task);
    return task;
}

tl::expected<Task, TaskPacketValidationError>
TaskRegistry::create_from_packet(TaskPacket packet) {
    auto validated = validate_packet(packet);
    if (!validated.has_value()) {
        return tl::unexpected(validated.error());
    }
    auto inner_packet = std::move(validated).value().into_inner();
    std::string prompt_str = inner_packet.objective;
    std::string scope_str = inner_packet.scope;
    std::lock_guard lock(mutex_);
    ++inner_.counter;
    uint64_t ts = now_secs();
    std::string task_id = std::format("task_{:08x}_{}", ts, inner_.counter);
    Task task{
        .task_id = task_id,
        .prompt = std::move(prompt_str),
        .description = std::move(scope_str),
        .task_packet = std::move(inner_packet),
        .status = TaskStatus::Created,
        .created_at = ts,
        .updated_at = ts,
    };
    inner_.tasks.emplace(task_id, task);
    return task;
}

std::optional<Task> TaskRegistry::get(std::string_view task_id) const {
    std::lock_guard lock(mutex_);
    auto it = inner_.tasks.find(std::string(task_id));
    if (it == inner_.tasks.end()) return std::nullopt;
    return it->second;
}

std::vector<Task> TaskRegistry::list(std::optional<TaskStatus> status_filter) const {
    std::lock_guard lock(mutex_);
    std::vector<Task> result;
    for (const auto& [id, task] : inner_.tasks) {
        if (!status_filter.has_value() || task.status == *status_filter) {
            result.push_back(task);
        }
    }
    return result;
}

tl::expected<Task, std::string> TaskRegistry::stop(std::string_view task_id) {
    std::lock_guard lock(mutex_);
    auto it = inner_.tasks.find(std::string(task_id));
    if (it == inner_.tasks.end()) {
        return tl::unexpected(std::format("task not found: {}", task_id));
    }
    auto& task = it->second;
    if (task.status == TaskStatus::Completed || task.status == TaskStatus::Failed || task.status == TaskStatus::Stopped) {
        return tl::unexpected(std::format("task {} is already in terminal state: {}", task_id, task_status_name(task.status)));
    }
    task.status = TaskStatus::Stopped;
    task.updated_at = now_secs();
    return task;
}

tl::expected<Task, std::string> TaskRegistry::update(std::string_view task_id, std::string_view message) {
    std::lock_guard lock(mutex_);
    auto it = inner_.tasks.find(std::string(task_id));
    if (it == inner_.tasks.end()) {
        return tl::unexpected(std::format("task not found: {}", task_id));
    }
    auto& task = it->second;
    task.messages.push_back(TaskMessage{
        .role = "user",
        .content = std::string(message),
        .timestamp = now_secs(),
    });
    task.updated_at = now_secs();
    return task;
}

tl::expected<std::string, std::string> TaskRegistry::output(std::string_view task_id) const {
    std::lock_guard lock(mutex_);
    auto it = inner_.tasks.find(std::string(task_id));
    if (it == inner_.tasks.end()) {
        return tl::unexpected(std::format("task not found: {}", task_id));
    }
    return it->second.output;
}

tl::expected<void, std::string> TaskRegistry::append_output(std::string_view task_id, std::string_view data) {
    std::lock_guard lock(mutex_);
    auto it = inner_.tasks.find(std::string(task_id));
    if (it == inner_.tasks.end()) {
        return tl::unexpected(std::format("task not found: {}", task_id));
    }
    it->second.output += data;
    it->second.updated_at = now_secs();
    return {};
}

tl::expected<void, std::string> TaskRegistry::set_status(std::string_view task_id, TaskStatus status) {
    std::lock_guard lock(mutex_);
    auto it = inner_.tasks.find(std::string(task_id));
    if (it == inner_.tasks.end()) {
        return tl::unexpected(std::format("task not found: {}", task_id));
    }
    it->second.status = status;
    it->second.updated_at = now_secs();
    return {};
}

tl::expected<void, std::string> TaskRegistry::assign_team(std::string_view task_id, std::string_view team_id) {
    std::lock_guard lock(mutex_);
    auto it = inner_.tasks.find(std::string(task_id));
    if (it == inner_.tasks.end()) {
        return tl::unexpected(std::format("task not found: {}", task_id));
    }
    it->second.team_id = std::string(team_id);
    it->second.updated_at = now_secs();
    return {};
}

std::optional<Task> TaskRegistry::remove(std::string_view task_id) {
    std::lock_guard lock(mutex_);
    auto it = inner_.tasks.find(std::string(task_id));
    if (it == inner_.tasks.end()) return std::nullopt;
    Task t = std::move(it->second);
    inner_.tasks.erase(it);
    return t;
}

std::size_t TaskRegistry::len() const {
    std::lock_guard lock(mutex_);
    return inner_.tasks.size();
}

bool TaskRegistry::is_empty() const {
    return len() == 0;
}

} // namespace claw::runtime
