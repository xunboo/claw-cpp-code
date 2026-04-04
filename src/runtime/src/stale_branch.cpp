#include "stale_branch.hpp"
#include "bash.hpp"
#include <format>
#include <charconv>

namespace claw::runtime {

tl::expected<BranchFreshnessResult, std::string>
check_freshness_in(const std::string& repo_dir, const StaleBranchPolicy& policy) {
    std::string upstream = policy.upstream_branch.value_or("main");

    // git rev-list --count HEAD..upstream
    BashCommandInput input;
    input.cwd = repo_dir;
    input.command = std::format("git rev-list --count HEAD..origin/{} 2>/dev/null || echo 0", upstream);

    auto result = execute_bash(input);
    if (!result) return tl::unexpected(result.error());

    std::string out = result->stdout_output;
    // Trim whitespace
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();

    std::size_t commits_behind = 0;
    auto [ptr, ec] = std::from_chars(out.data(), out.data() + out.size(), commits_behind);
    if (ec != std::errc{}) {
        return BranchFreshnessResult{BranchFreshness::Unknown, std::chrono::seconds{0}, std::nullopt};
    }

    // Estimate age: assume ~1 commit per hour as heuristic
    std::chrono::seconds estimated_age = std::chrono::seconds(commits_behind * 3600);

    BranchFreshness freshness;
    if (commits_behind == 0) {
        freshness = BranchFreshness::Fresh;
    } else if (estimated_age >= policy.stale_threshold) {
        freshness = BranchFreshness::Stale;
    } else {
        freshness = BranchFreshness::Fresh;
    }

    return BranchFreshnessResult{freshness, estimated_age, commits_behind};
}

StaleBranchAction apply_policy(const BranchFreshnessResult& result, const StaleBranchPolicy& policy) {
    if (result.freshness != BranchFreshness::Stale) {
        return StaleBranchAction::Warn; // no action needed for fresh branches
    }
    return policy.action;
}

} // namespace claw::runtime
