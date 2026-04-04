#pragma once
#include <tl/expected.hpp>

#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace claw::tools {

// ── Lane event types ──────────────────────────────────────────────────────────

enum class LaneEventName { Started, Blocked, Finished, Failed };
enum class LaneFailureClass {
    PromptDelivery, TrustGate, BranchDivergence,
    Compile, Test, PluginStartup, McpStartup, McpHandshake,
    GatewayRouting, ToolRuntime, Infra
};

struct LaneBlocker {
    LaneFailureClass failure_class;
    std::string      detail;
};

struct LaneEvent {
    LaneEventName              event;
    std::string                status;
    std::string                emitted_at;
    std::optional<LaneFailureClass> failure_class;
    std::optional<std::string> detail;
};

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
    std::vector<LaneEvent>       lane_events;
    std::optional<LaneBlocker>   current_blocker;
    std::optional<std::string>   error;
};

nlohmann::json to_json(const AgentOutput& ao);
nlohmann::json to_json(const LaneEvent& ev);
nlohmann::json to_json(const LaneBlocker& b);

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
