#include "policy_engine.hpp"
#include <algorithm>

namespace claw::runtime {

std::vector<PolicyAction> flatten_actions(const std::vector<PolicyAction>& actions) {
    // In this implementation, ActionChain is not in the variant (removed for simplicity).
    // All actions are already flat.
    return actions;
}

PolicyEngine::PolicyEngine(std::vector<PolicyRule> rules) : rules_(std::move(rules)) {
    std::stable_sort(rules_.begin(), rules_.end(), [](const PolicyRule& a, const PolicyRule& b) {
        return a.priority > b.priority;
    });
}

bool PolicyEngine::condition_matches(const PolicyCondition& cond, const LaneContext& ctx) const {
    return std::visit([&](const auto& c) -> bool {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, ConditionGreenAt>) {
            return ctx.green_level >= c.level;
        } else if constexpr (std::is_same_v<T, ConditionStaleBranch>) {
            return ctx.branch_freshness >= STALE_BRANCH_THRESHOLD;
        } else if constexpr (std::is_same_v<T, ConditionStartupBlocked>) {
            return ctx.blocker;
        } else if constexpr (std::is_same_v<T, ConditionLaneCompleted>) {
            return ctx.completed;
        } else if constexpr (std::is_same_v<T, ConditionLaneReconciled>) {
            return ctx.reconciled;
        } else if constexpr (std::is_same_v<T, ConditionReviewPassed>) {
            return ctx.review_status;
        } else if constexpr (std::is_same_v<T, ConditionScopedDiff>) {
            return ctx.diff_scope == c.scope;
        } else if constexpr (std::is_same_v<T, ConditionTimedOut>) {
            return ctx.elapsed >= c.duration;
        }
        return false;
    }, cond);
}

std::vector<PolicyAction> PolicyEngine::evaluate(const LaneContext& ctx) const {
    std::vector<PolicyAction> all_actions;
    for (const auto& rule : rules_) {
        if (condition_matches(rule.condition, ctx)) {
            auto flat = flatten_actions(rule.actions);
            for (auto& a : flat) all_actions.push_back(std::move(a));
        }
    }
    return all_actions;
}

} // namespace claw::runtime
