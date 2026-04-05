#include "stale_branch.hpp"
#include "bash.hpp"
#include <format>
#include <charconv>
#include <sstream>

namespace claw::runtime {

namespace {

/// Format missing fixes for display in policy messages.
std::string format_missing_fixes(const std::vector<std::string>& missing_fixes) {
    if (missing_fixes.empty()) return "(none)";
    std::string result;
    for (std::size_t i = 0; i < missing_fixes.size(); ++i) {
        if (i > 0) result += "; ";
        result += missing_fixes[i];
    }
    return result;
}

std::size_t rev_list_count(const std::string& a, const std::string& b,
                            const std::string& repo_dir) {
    BashCommandInput input;
    input.cwd = repo_dir;
    input.command = std::format("git rev-list --count {}..{} 2>/dev/null || echo 0", b, a);
    auto result = execute_bash(input);
    if (!result) return 0;

    std::string out = result->stdout_output;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();

    std::size_t count = 0;
    auto [ptr, ec] = std::from_chars(out.data(), out.data() + out.size(), count);
    if (ec != std::errc{}) return 0;
    return count;
}

std::vector<std::string> missing_fix_subjects(const std::string& main_ref,
                                               const std::string& branch,
                                               const std::string& repo_dir) {
    BashCommandInput input;
    input.cwd = repo_dir;
    input.command = std::format(
        "git log --oneline {}..{} 2>/dev/null || true", branch, main_ref);
    auto result = execute_bash(input);
    if (!result) return {};

    std::vector<std::string> subjects;
    std::istringstream stream(result->stdout_output);
    std::string line;
    while (std::getline(stream, line)) {
        // Skip empty lines
        auto s = line.find_first_not_of(" \t\r");
        if (s == std::string::npos) continue;
        // Extract subject (after first space = after short hash)
        auto space = line.find(' ', s);
        if (space != std::string::npos) {
            subjects.push_back(line.substr(space + 1));
        } else {
            subjects.push_back(line.substr(s));
        }
    }
    return subjects;
}

} // anonymous namespace

tl::expected<BranchFreshness, std::string>
check_freshness_in(const std::string& repo_dir, const StaleBranchPolicy& policy) {
    std::string upstream = policy.upstream_branch.value_or("main");
    std::string main_ref = std::format("origin/{}", upstream);
    std::string branch = "HEAD";

    std::size_t behind = rev_list_count(main_ref, branch, repo_dir);
    std::size_t ahead = rev_list_count(branch, main_ref, repo_dir);

    if (behind == 0 && ahead == 0) {
        return BranchFresh{};
    }

    if (ahead > 0) {
        return BranchDiverged{
            ahead, behind,
            missing_fix_subjects(main_ref, branch, repo_dir)
        };
    }

    auto fixes = missing_fix_subjects(main_ref, branch, repo_dir);
    return BranchStale{behind, std::move(fixes)};
}

StaleBranchAction apply_policy(const BranchFreshness& freshness,
                                StaleBranchPolicyKind policy) {
    return std::visit([&](const auto& f) -> StaleBranchAction {
        using T = std::decay_t<decltype(f)>;

        if constexpr (std::is_same_v<T, BranchFresh>) {
            return StaleBranchActionWarn{"branch is up to date"};
        }
        else if constexpr (std::is_same_v<T, BranchStale>) {
            switch (policy) {
                case StaleBranchPolicyKind::WarnOnly:
                    return StaleBranchActionWarn{std::format(
                        "Branch is {} commit(s) behind main. Missing fixes: {}",
                        f.behind, format_missing_fixes(f.missing_fixes))};
                case StaleBranchPolicyKind::Block:
                    return StaleBranchActionBlock{std::format(
                        "Branch is {} commit(s) behind main and must be updated before proceeding. Missing fixes: {}",
                        f.behind, format_missing_fixes(f.missing_fixes))};
                case StaleBranchPolicyKind::AutoRebase:
                    return StaleBranchActionRebase{};
                case StaleBranchPolicyKind::AutoMergeForward:
                    return StaleBranchActionMergeForward{};
            }
            return StaleBranchActionWarn{"unknown policy"};
        }
        else if constexpr (std::is_same_v<T, BranchDiverged>) {
            switch (policy) {
                case StaleBranchPolicyKind::WarnOnly:
                    return StaleBranchActionWarn{std::format(
                        "Branch has diverged: {} commit(s) ahead, {} commit(s) behind main. Missing fixes: {}",
                        f.ahead, f.behind, format_missing_fixes(f.missing_fixes))};
                case StaleBranchPolicyKind::Block:
                    return StaleBranchActionBlock{std::format(
                        "Branch has diverged ({} ahead, {} behind) and must be reconciled before proceeding. Missing fixes: {}",
                        f.ahead, f.behind, format_missing_fixes(f.missing_fixes))};
                case StaleBranchPolicyKind::AutoRebase:
                    return StaleBranchActionRebase{};
                case StaleBranchPolicyKind::AutoMergeForward:
                    return StaleBranchActionMergeForward{};
            }
            return StaleBranchActionWarn{"unknown policy"};
        }
    }, freshness);
}

} // namespace claw::runtime
