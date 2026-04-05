#include "lane_completion.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace claw::tools {

using namespace claw::runtime;

// ── detect_lane_completion ───────────────────────────────────────────────────

std::optional<LaneContext>
detect_lane_completion(const AgentOutput& output,
                       bool test_green,
                       bool has_pushed) {
    // Must be finished without errors
    if (output.error.has_value()) {
        return std::nullopt;
    }

    // Must have finished status (case-insensitive check)
    auto status_lower = output.status;
    std::transform(status_lower.begin(), status_lower.end(), status_lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (status_lower != "completed" && status_lower != "finished") {
        return std::nullopt;
    }

    // Must have no current blocker
    if (output.current_blocker.has_value()) {
        return std::nullopt;
    }

    // Must have green tests
    if (!test_green) {
        return std::nullopt;
    }

    // Must have pushed code
    if (!has_pushed) {
        return std::nullopt;
    }

    // All conditions met -- create completed context
    LaneContext ctx;
    ctx.lane_id          = output.agent_id;
    ctx.green_level      = 3; // Workspace green
    ctx.branch_freshness = std::chrono::seconds{0};
    ctx.blocker          = false;
    ctx.review_status    = false; // Approved maps to false in the simplified C++ model
    ctx.diff_scope       = "scoped";
    ctx.completed        = true;
    ctx.elapsed          = std::chrono::seconds{0};

    return ctx;
}

// ── evaluate_completed_lane ──────────────────────────────────────────────────

std::vector<PolicyAction>
evaluate_completed_lane(const LaneContext& context) {
    PolicyEngine engine({
        PolicyRule{
            "closeout-completed-lane",
            ConditionLaneCompleted{},
            {ActionCloseoutLane{}},
            10,
        },
        PolicyRule{
            "cleanup-completed-session",
            ConditionLaneCompleted{},
            {ActionCleanupSession{}},
            5,
        },
    });

    return engine.evaluate(context);
}

} // namespace claw::tools
