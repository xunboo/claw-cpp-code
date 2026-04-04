#pragma once
#include <string>
#include <optional>
#include <variant>
#include <chrono>
#include <tl/expected.hpp>

namespace claw::runtime {

enum class BranchFreshness {
    Fresh,
    Stale,
    Unknown,
};

enum class StaleBranchAction {
    Warn,
    Block,
    AutoRebase,
    AutoMerge,
};

struct StaleBranchPolicy {
    std::chrono::seconds stale_threshold{3600};
    StaleBranchAction action{StaleBranchAction::Warn};
    std::optional<std::string> upstream_branch; // e.g. "main", "develop"
};

struct BranchFreshnessResult {
    BranchFreshness freshness;
    std::chrono::seconds age_behind; // how far behind upstream
    std::optional<std::size_t> commits_behind;
};

// Check branch freshness by running git rev-list in the given repo directory
[[nodiscard]] tl::expected<BranchFreshnessResult, std::string>
    check_freshness_in(const std::string& repo_dir, const StaleBranchPolicy& policy);

// Apply the policy: returns action to take
[[nodiscard]] StaleBranchAction apply_policy(const BranchFreshnessResult& result, const StaleBranchPolicy& policy);

} // namespace claw::runtime
