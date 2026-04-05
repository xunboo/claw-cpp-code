#pragma once
#include <tl/expected.hpp>

#include "lane_events.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace claw::tools {

// ── Lane event types (re-exported from runtime) ──────────────────────────────
// These were previously defined locally; now they live in runtime::lane_events.
// The `using` declarations preserve source compatibility for existing code.

using LaneEventName    = claw::runtime::LaneEventName;
using LaneEventStatus  = claw::runtime::LaneEventStatus;
using LaneFailureClass = claw::runtime::LaneFailureClass;
using LaneEventBlocker = claw::runtime::LaneEventBlocker;
using LaneEvent        = claw::runtime::LaneEvent;

// Legacy alias -- old code used `LaneBlocker`; Rust renamed to `LaneEventBlocker`.
using LaneBlocker = LaneEventBlocker;

// ── Agent manifest ────────────────────────────────────────────────────────────

struct AgentOutput {
    std::string                  agent_id;
    std::string                  name;
    std::string                  description;
    std::optional<std::string>   subagent_type;
    std::optional<std::string>   model;
    std::string                  status;
    std::string                  output_file;
    std::string                  manifest_file;
    std::string                  created_at;
    std::optional<std::string>   started_at;
    std::optional<std::string>   completed_at;
    std::vector<LaneEvent>            lane_events;
    std::optional<LaneEventBlocker>   current_blocker;
    std::optional<std::string>        error;
};

nlohmann::json to_json(const AgentOutput& ao);

// ── AgentInput ────────────────────────────────────────────────────────────────

struct AgentInput {
    std::string                description;
    std::string                prompt;
    std::optional<std::string> subagent_type;
    std::optional<std::string> name;
    std::optional<std::string> model;
};

// ── Execution ─────────────────────────────────────────────────────────────────

[[nodiscard]] tl::expected<AgentOutput, std::string>
    execute_agent(AgentInput input);

// ── Helpers ───────────────────────────────────────────────────────────────────

[[nodiscard]] std::string iso8601_now();
[[nodiscard]] std::string make_agent_id();
[[nodiscard]] std::string slugify_agent_name(const std::string& description);
[[nodiscard]] std::string normalize_subagent_type(const std::optional<std::string>& subagent_type);
[[nodiscard]] std::set<std::string> allowed_tools_for_subagent(const std::string& subagent_type);

[[nodiscard]] LaneBlocker       classify_lane_blocker(const std::string& error);
[[nodiscard]] LaneFailureClass  classify_lane_failure(const std::string& error);

[[nodiscard]] std::string_view lane_failure_class_name(LaneFailureClass fc) noexcept;

}  // namespace claw::tools
