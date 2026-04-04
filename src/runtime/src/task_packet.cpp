// task_packet.cpp — translated from Rust task_packet.rs
// Every public Rust fn is present. Conventions:
//   Result<T,E>  → tl::expected<T,E>
//   Option<T>    → std::optional<T>
//   Vec<T>       → std::vector<T>
//   BTreeMap     → std::map
//   PathBuf/Path → std::string (with std::filesystem helpers)
//   serde_json   → nlohmann::json
//   namespace    → runtime  (header says "runtime", kept consistent)

#include "task_packet.hpp"
#include <algorithm>
#include <filesystem>
#include <format>

namespace claw::runtime {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Returns true when the string is empty or contains only whitespace.
static bool is_blank(const std::string& s) {
    return s.empty() ||
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

/// Resolve a single path against dispatch_root.
/// Absolute paths are returned as-is; relative paths are joined.
static std::string resolve_path(const std::string& dispatch_root,
                                 const std::string& path) {
    namespace fs = std::filesystem;
    fs::path fp(path);
    if (fp.is_absolute()) return path;
    return (fs::path(dispatch_root) / fp).string();
}

// ---------------------------------------------------------------------------
// RepoConfig helpers
// ---------------------------------------------------------------------------

/// Returns the effective dispatch root: worktree_root when set and non-empty,
/// otherwise repo_root. Mirrors Rust RepoConfig::dispatch_root().
[[nodiscard]] static const std::string&
dispatch_root(const RepoConfig& rc) noexcept {
    if (!rc.worktree_root.empty()) return rc.worktree_root;
    return rc.repo_root;
}

// ---------------------------------------------------------------------------
// resolve_task_scope_paths
// ---------------------------------------------------------------------------

TaskScope resolve_task_scope_paths(const TaskScope& scope,
                                   const std::string& dr) {
    return std::visit([&](const auto& s) -> TaskScope {
        using T = std::decay_t<decltype(s)>;

        if constexpr (std::is_same_v<T, TaskScopeGlobal>) {
            // Workspace scope → the dispatch root itself (returned as-is)
            return TaskScopeGlobal{};

        } else if constexpr (std::is_same_v<T, TaskScopeFiles>) {
            // SingleFile / Custom: resolve every path
            std::vector<std::string> resolved;
            resolved.reserve(s.paths.size());
            for (const auto& p : s.paths) {
                resolved.push_back(resolve_path(dr, p));
            }
            return TaskScopeFiles{std::move(resolved)};

        } else if constexpr (std::is_same_v<T, TaskScopeDirectory>) {
            // Module scope → join dispatch_root/crates/<dir>
            return TaskScopeDirectory{resolve_path(dr, s.directory)};
        }
    }, scope);
}

// ---------------------------------------------------------------------------
// Validation helpers (mirror Rust private fns)
// ---------------------------------------------------------------------------

static void validate_scope(const TaskScope& scope,
                            std::vector<std::string>& errors) {
    std::visit([&](const auto& s) {
        using T = std::decay_t<decltype(s)>;

        if constexpr (std::is_same_v<T, TaskScopeFiles>) {
            // Covers both SingleFile (one path) and Custom (many paths)
            if (s.paths.empty()) {
                errors.push_back("custom scope paths must not be empty");
            } else {
                for (std::size_t i = 0; i < s.paths.size(); ++i) {
                    if (s.paths[i].empty()) {
                        errors.push_back(
                            std::format("custom scope contains empty path at index {}", i));
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, TaskScopeDirectory>) {
            if (is_blank(s.directory)) {
                errors.push_back("module scope crate_name must not be empty");
            }
        }
        // TaskScopeGlobal (Workspace) needs no validation
    }, scope);
}

static void validate_branch_policy(const BranchPolicy& bp,
                                    std::vector<std::string>& errors) {
    std::visit([&](const auto& b) {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, BranchPolicyCreateNew>) {
            if (is_blank(b.prefix)) {
                errors.push_back("create_new branch prefix must not be empty");
            }
        } else if constexpr (std::is_same_v<T, BranchPolicyUseExisting>) {
            if (is_blank(b.name)) {
                errors.push_back("use_existing branch name must not be empty");
            }
        }
        // BranchPolicyWorktreeIsolated needs no validation
    }, bp);
}

static void validate_acceptance_tests(const std::vector<AcceptanceTest>& tests,
                                       std::vector<std::string>& errors) {
    for (const auto& test : tests) {
        std::visit([&](const auto& t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, AccTestCargoTest>) {
                // filter must not be empty when present
                if (t.filter.has_value() && is_blank(*t.filter)) {
                    errors.push_back(
                        "cargo_test filter must not be empty when present");
                }
            } else if constexpr (std::is_same_v<T, AccTestCustomCommand>) {
                if (is_blank(t.cmd)) {
                    errors.push_back("custom_command cmd must not be empty");
                }
            }
            // AccTestGreenLevel needs no validation
        }, test);
    }
}

static void validate_escalation_policy(const EscalationPolicy& ep,
                                        std::vector<std::string>& errors) {
    if (const auto* r = std::get_if<EscalationPolicyRetryThenEscalate>(&ep)) {
        if (r->max_retries == 0) {
            errors.push_back(
                "retry_then_escalate max_retries must be greater than zero");
        }
    }
}

// ---------------------------------------------------------------------------
// validate_packet
// ---------------------------------------------------------------------------

tl::expected<ValidatedPacket, std::vector<std::string>>
validate_packet(const TaskPacket& packet) {
    std::vector<std::string> errors;

    // --- id & objective ---
    if (is_blank(packet.id)) {
        errors.push_back("packet id must not be empty");
    }
    if (is_blank(packet.objective)) {
        errors.push_back("packet objective must not be empty");
    }

    // --- repo_config ---
    if (packet.repo.repo_root.empty()) {
        errors.push_back("repo_config repo_root must not be empty");
    }
    // worktree_root must not be empty *when present* (non-empty string = present)
    if (!packet.repo.worktree_root.empty() &&
        is_blank(packet.repo.worktree_root)) {
        errors.push_back(
            "repo_config worktree_root must not be empty when present");
    }

    // --- sub-validators ---
    validate_scope(packet.scope, errors);
    validate_branch_policy(packet.branch_policy, errors);
    validate_acceptance_tests(packet.acceptance_tests, errors);
    validate_escalation_policy(packet.escalation, errors);

    if (!errors.empty()) {
        return tl::unexpected(std::move(errors));
    }
    return ValidatedPacket{packet};
}

} // namespace claw::runtime
