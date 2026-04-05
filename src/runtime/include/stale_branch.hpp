#pragma once
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <chrono>
#include <tl/expected.hpp>

namespace claw::runtime {

/// Branch freshness relative to upstream.
/// Mirrors Rust BranchFreshness enum.
struct BranchFresh {};
struct BranchStale {
    std::size_t behind{0};
    std::vector<std::string> missing_fixes;
};
struct BranchDiverged {
    std::size_t ahead{0};
    std::size_t behind{0};
    std::vector<std::string> missing_fixes;
};

using BranchFreshness = std::variant<BranchFresh, BranchStale, BranchDiverged>;

enum class StaleBranchPolicyKind {
    WarnOnly,
    Block,
    AutoRebase,
    AutoMergeForward,
};

struct StaleBranchPolicy {
    StaleBranchPolicyKind kind{StaleBranchPolicyKind::WarnOnly};
    std::optional<std::string> upstream_branch; // e.g. "main", "develop"
};

/// Actions produced by applying a stale-branch policy.
struct StaleBranchActionWarn { std::string message; };
struct StaleBranchActionBlock { std::string message; };
struct StaleBranchActionRebase {};
struct StaleBranchActionMergeForward {};

using StaleBranchAction = std::variant<
    StaleBranchActionWarn,
    StaleBranchActionBlock,
    StaleBranchActionRebase,
    StaleBranchActionMergeForward
>;

/// Check branch freshness by running git rev-list in the given repo directory.
[[nodiscard]] tl::expected<BranchFreshness, std::string>
    check_freshness_in(const std::string& repo_dir, const StaleBranchPolicy& policy);

/// Apply the policy to a freshness result: returns action to take.
[[nodiscard]] StaleBranchAction apply_policy(const BranchFreshness& freshness,
                                              StaleBranchPolicyKind policy);

} // namespace claw::runtime
