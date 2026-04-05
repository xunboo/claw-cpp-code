#pragma once
// lane_completion.hpp -- C++20 port of tools/src/lane_completion.rs
// Lane completion detector: automatically marks lanes as completed when
// session finishes successfully with green tests and pushed code.

#include "agent_tools.hpp"
#include "policy_engine.hpp"

#include <optional>
#include <string>
#include <vector>

namespace claw::tools {

/// Detects if a lane should be automatically marked as completed.
///
/// Returns a LaneContext with completed=true if all conditions met,
/// std::nullopt if lane should remain active.
[[nodiscard]] std::optional<claw::runtime::LaneContext>
    detect_lane_completion(const AgentOutput& output,
                           bool test_green,
                           bool has_pushed);

/// Evaluates policy actions for a completed lane.
[[nodiscard]] std::vector<claw::runtime::PolicyAction>
    evaluate_completed_lane(const claw::runtime::LaneContext& context);

} // namespace claw::tools
