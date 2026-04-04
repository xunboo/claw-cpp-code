#pragma once
#include "green_contract.hpp"
#include "stale_branch.hpp"
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <chrono>

namespace claw::runtime {

using GreenLevelU8 = uint8_t;

inline constexpr std::chrono::seconds STALE_BRANCH_THRESHOLD{3600};

// Policy actions
struct ActionMergeToDev {};
struct ActionMergeForward {};
struct ActionRecoverOnce {};
struct ActionEscalate { std::string reason; };
struct ActionCloseoutLane {};
struct ActionCleanupSession {};
struct ActionNotify { std::string channel; };
struct ActionBlock { std::string reason; };
struct ActionChain; // forward declared

using PolicyAction = std::variant<
    ActionMergeToDev,
    ActionMergeForward,
    ActionRecoverOnce,
    ActionEscalate,
    ActionCloseoutLane,
    ActionCleanupSession,
    ActionNotify,
    ActionBlock
>;

// Flatten a chain of actions (recursive)
[[nodiscard]] std::vector<PolicyAction> flatten_actions(const std::vector<PolicyAction>& actions);

// Policy conditions
struct ConditionAnd;
struct ConditionOr;

struct ConditionGreenAt    { GreenLevelU8 level; };
struct ConditionStaleBranch {};
struct ConditionStartupBlocked {};
struct ConditionLaneCompleted {};
struct ConditionReviewPassed {};
struct ConditionScopedDiff { std::string scope; };
struct ConditionTimedOut   { std::chrono::seconds duration; };

using PolicyCondition = std::variant<
    ConditionGreenAt,
    ConditionStaleBranch,
    ConditionStartupBlocked,
    ConditionLaneCompleted,
    ConditionReviewPassed,
    ConditionScopedDiff,
    ConditionTimedOut
>;

struct PolicyRule {
    std::string name;
    PolicyCondition condition;
    std::vector<PolicyAction> actions;
    int priority{0};
};

struct LaneContext {
    std::string lane_id;
    GreenLevelU8 green_level{0};
    std::chrono::seconds branch_freshness{0};
    bool blocker{false};
    bool review_status{false};
    std::string diff_scope;
    bool completed{false};
    std::chrono::seconds elapsed{0};
};

class PolicyEngine {
public:
    explicit PolicyEngine(std::vector<PolicyRule> rules);

    // Evaluate all matching rules for given context; returns flattened actions
    [[nodiscard]] std::vector<PolicyAction> evaluate(const LaneContext& ctx) const;

private:
    std::vector<PolicyRule> rules_; // sorted by priority descending

    [[nodiscard]] bool condition_matches(const PolicyCondition& cond, const LaneContext& ctx) const;
};

} // namespace claw::runtime
