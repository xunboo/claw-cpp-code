#include "team_cron_registry.hpp"
#include <chrono>
#include <format>

namespace claw::runtime {

namespace {

uint64_t now_secs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

} // anonymous namespace

// ─── TeamStatus ─────────────────────────────────────────────────────────────

std::string_view team_status_name(TeamStatus s) noexcept {
    switch (s) {
        case TeamStatus::Created:   return "created";
        case TeamStatus::Running:   return "running";
        case TeamStatus::Completed: return "completed";
        case TeamStatus::Deleted:   return "deleted";
    }
    return "unknown";
}

// ─── TeamRegistry ───────────────────────────────────────────────────────────

Team TeamRegistry::create(std::string_view name, std::vector<std::string> task_ids) {
    std::lock_guard lock(mutex_);
    ++inner_.counter;
    uint64_t ts = now_secs();
    std::string team_id = std::format("team_{:08x}_{}", ts, inner_.counter);
    Team team{
        .team_id = team_id,
        .name = std::string(name),
        .task_ids = std::move(task_ids),
        .status = TeamStatus::Created,
        .created_at = ts,
        .updated_at = ts,
    };
    inner_.teams.emplace(team_id, team);
    return team;
}

std::optional<Team> TeamRegistry::get(std::string_view team_id) const {
    std::lock_guard lock(mutex_);
    auto it = inner_.teams.find(std::string(team_id));
    if (it == inner_.teams.end()) return std::nullopt;
    return it->second;
}

std::vector<Team> TeamRegistry::list() const {
    std::lock_guard lock(mutex_);
    std::vector<Team> result;
    result.reserve(inner_.teams.size());
    for (const auto& [id, team] : inner_.teams) {
        result.push_back(team);
    }
    return result;
}

tl::expected<Team, std::string> TeamRegistry::delete_team(std::string_view team_id) {
    std::lock_guard lock(mutex_);
    auto it = inner_.teams.find(std::string(team_id));
    if (it == inner_.teams.end()) {
        return tl::unexpected(std::format("team not found: {}", team_id));
    }
    it->second.status = TeamStatus::Deleted;
    it->second.updated_at = now_secs();
    return it->second;
}

std::optional<Team> TeamRegistry::remove(std::string_view team_id) {
    std::lock_guard lock(mutex_);
    auto it = inner_.teams.find(std::string(team_id));
    if (it == inner_.teams.end()) return std::nullopt;
    Team t = std::move(it->second);
    inner_.teams.erase(it);
    return t;
}

std::size_t TeamRegistry::len() const {
    std::lock_guard lock(mutex_);
    return inner_.teams.size();
}

bool TeamRegistry::is_empty() const {
    return len() == 0;
}

// ─── CronRegistry ───────────────────────────────────────────────────────────

CronEntry CronRegistry::create(std::string_view schedule, std::string_view prompt, std::optional<std::string_view> description) {
    std::lock_guard lock(mutex_);
    ++inner_.counter;
    uint64_t ts = now_secs();
    std::string cron_id = std::format("cron_{:08x}_{}", ts, inner_.counter);
    CronEntry entry{
        .cron_id = cron_id,
        .schedule = std::string(schedule),
        .prompt = std::string(prompt),
        .description = description.has_value() ? std::optional<std::string>(std::string(*description)) : std::nullopt,
        .enabled = true,
        .created_at = ts,
        .updated_at = ts,
        .last_run_at = std::nullopt,
        .run_count = 0,
    };
    inner_.entries.emplace(cron_id, entry);
    return entry;
}

std::optional<CronEntry> CronRegistry::get(std::string_view cron_id) const {
    std::lock_guard lock(mutex_);
    auto it = inner_.entries.find(std::string(cron_id));
    if (it == inner_.entries.end()) return std::nullopt;
    return it->second;
}

std::vector<CronEntry> CronRegistry::list(bool enabled_only) const {
    std::lock_guard lock(mutex_);
    std::vector<CronEntry> result;
    for (const auto& [id, entry] : inner_.entries) {
        if (!enabled_only || entry.enabled) {
            result.push_back(entry);
        }
    }
    return result;
}

tl::expected<CronEntry, std::string> CronRegistry::delete_cron(std::string_view cron_id) {
    std::lock_guard lock(mutex_);
    auto it = inner_.entries.find(std::string(cron_id));
    if (it == inner_.entries.end()) {
        return tl::unexpected(std::format("cron not found: {}", cron_id));
    }
    CronEntry e = std::move(it->second);
    inner_.entries.erase(it);
    return e;
}

tl::expected<void, std::string> CronRegistry::disable(std::string_view cron_id) {
    std::lock_guard lock(mutex_);
    auto it = inner_.entries.find(std::string(cron_id));
    if (it == inner_.entries.end()) {
        return tl::unexpected(std::format("cron not found: {}", cron_id));
    }
    it->second.enabled = false;
    it->second.updated_at = now_secs();
    return {};
}

tl::expected<void, std::string> CronRegistry::record_run(std::string_view cron_id) {
    std::lock_guard lock(mutex_);
    auto it = inner_.entries.find(std::string(cron_id));
    if (it == inner_.entries.end()) {
        return tl::unexpected(std::format("cron not found: {}", cron_id));
    }
    auto ts = now_secs();
    it->second.last_run_at = ts;
    ++it->second.run_count;
    it->second.updated_at = ts;
    return {};
}

std::size_t CronRegistry::len() const {
    std::lock_guard lock(mutex_);
    return inner_.entries.size();
}

bool CronRegistry::is_empty() const {
    return len() == 0;
}

} // namespace claw::runtime
