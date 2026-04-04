#pragma once
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <cstdint>
#include <tl/expected.hpp>

namespace claw::runtime {

enum class TaskStatus {
    Created,
    Running,
    Completed,
    Failed,
    Stopped,
};

[[nodiscard]] std::string_view task_status_name(TaskStatus s) noexcept;

struct TaskMessage {
    std::string role;
    std::string content;
    uint64_t timestamp{0};
};

struct Task {
    std::string task_id;
    std::string prompt;
    std::optional<std::string> description;
    TaskStatus status{TaskStatus::Created};
    uint64_t created_at{0};
    uint64_t updated_at{0};
    std::vector<TaskMessage> messages;
    std::string output;
    std::optional<std::string> team_id;
};

class TaskRegistry {
public:
    TaskRegistry() = default;

    [[nodiscard]] Task create(std::string_view prompt, std::optional<std::string_view> description = std::nullopt);
    [[nodiscard]] std::optional<Task> get(std::string_view task_id) const;
    [[nodiscard]] std::vector<Task> list(std::optional<TaskStatus> status_filter = std::nullopt) const;
    [[nodiscard]] tl::expected<Task, std::string> stop(std::string_view task_id);
    [[nodiscard]] tl::expected<Task, std::string> update(std::string_view task_id, std::string_view message);
    [[nodiscard]] tl::expected<std::string, std::string> output(std::string_view task_id) const;
    [[nodiscard]] tl::expected<void, std::string> append_output(std::string_view task_id, std::string_view data);
    [[nodiscard]] tl::expected<void, std::string> set_status(std::string_view task_id, TaskStatus status);
    [[nodiscard]] tl::expected<void, std::string> assign_team(std::string_view task_id, std::string_view team_id);
    [[nodiscard]] std::optional<Task> remove(std::string_view task_id);
    [[nodiscard]] std::size_t len() const;
    [[nodiscard]] bool is_empty() const;

private:
    struct Inner {
        std::unordered_map<std::string, Task> tasks;
        uint64_t counter{0};
    };
    mutable std::mutex mutex_;
    Inner inner_;
};

} // namespace claw::runtime
