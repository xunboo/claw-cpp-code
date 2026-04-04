#pragma once
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <tl/expected.hpp>

namespace claw::runtime {

// ─── Team ────────────────────────────────────────────────────────────────────

enum class TeamStatus {
    Created,
    Running,
    Completed,
    Deleted,
};

[[nodiscard]] std::string_view team_status_name(TeamStatus s) noexcept;

struct Team {
    std::string team_id;
    std::string name;
    std::vector<std::string> task_ids;
    TeamStatus status{TeamStatus::Created};
    uint64_t created_at{0};
    uint64_t updated_at{0};
};

class TeamRegistry {
public:
    TeamRegistry() = default;

    [[nodiscard]] Team create(std::string_view name, std::vector<std::string> task_ids);
    [[nodiscard]] std::optional<Team> get(std::string_view team_id) const;
    [[nodiscard]] std::vector<Team> list() const;
    [[nodiscard]] tl::expected<Team, std::string> delete_team(std::string_view team_id); // soft-delete
    [[nodiscard]] std::optional<Team> remove(std::string_view team_id);                   // hard remove
    [[nodiscard]] std::size_t len() const;
    [[nodiscard]] bool is_empty() const;

private:
    struct Inner {
        std::unordered_map<std::string, Team> teams;
        uint64_t counter{0};
    };
    mutable std::mutex mutex_;
    Inner inner_;
};

// ─── Cron ────────────────────────────────────────────────────────────────────

struct CronEntry {
    std::string cron_id;
    std::string schedule;
    std::string prompt;
    std::optional<std::string> description;
    bool enabled{true};
    uint64_t created_at{0};
    uint64_t updated_at{0};
    std::optional<uint64_t> last_run_at;
    uint64_t run_count{0};
};

class CronRegistry {
public:
    CronRegistry() = default;

    [[nodiscard]] CronEntry create(std::string_view schedule, std::string_view prompt, std::optional<std::string_view> description = std::nullopt);
    [[nodiscard]] std::optional<CronEntry> get(std::string_view cron_id) const;
    [[nodiscard]] std::vector<CronEntry> list(bool enabled_only) const;
    [[nodiscard]] tl::expected<CronEntry, std::string> delete_cron(std::string_view cron_id);
    [[nodiscard]] tl::expected<void, std::string> disable(std::string_view cron_id);
    [[nodiscard]] tl::expected<void, std::string> record_run(std::string_view cron_id);
    [[nodiscard]] std::size_t len() const;
    [[nodiscard]] bool is_empty() const;

private:
    struct Inner {
        std::unordered_map<std::string, CronEntry> entries;
        uint64_t counter{0};
    };
    mutable std::mutex mutex_;
    Inner inner_;
};

} // namespace claw::runtime
