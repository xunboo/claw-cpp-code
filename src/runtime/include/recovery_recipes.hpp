#pragma once
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <unordered_map>
#include <cstddef>
#include <tl/expected.hpp>

namespace claw::runtime {

enum class FailureScenario {
    TrustPromptUnresolved,
    PromptMisdelivery,
    StaleBranch,
    CompileRedCrossCrate,
    McpHandshakeFailure,
    PartialPluginStartup,
};

[[nodiscard]] std::string_view failure_scenario_name(FailureScenario s) noexcept;

// Recovery steps
struct StepRetryWithDelay       { uint64_t delay_ms{1000}; };
struct StepResetToLastGoodState {};
struct StepEscalateToHuman      {};
struct StepRestartComponent     { std::string component; };
struct StepRetryMcpHandshake    { uint64_t timeout_ms{30000}; };
struct StepRestartPlugin        { std::string name; };
struct StepSkipAndContinue      {};

using RecoveryStep = std::variant<
    StepRetryWithDelay,
    StepResetToLastGoodState,
    StepEscalateToHuman,
    StepRestartComponent,
    StepRetryMcpHandshake,
    StepRestartPlugin,
    StepSkipAndContinue
>;

enum class EscalationPolicy {
    AlertHuman,
    LogAndContinue,
    Abort,
};

struct RecoveryRecipe {
    FailureScenario scenario;
    std::vector<RecoveryStep> steps;
    uint32_t max_attempts{1};
    EscalationPolicy escalation_policy{EscalationPolicy::LogAndContinue};
};

struct RecoveryRecovered   { std::vector<RecoveryStep> steps_taken; };
struct RecoveryPartial     { std::vector<RecoveryStep> recovered; std::vector<RecoveryStep> remaining; };
struct RecoveryEscalation  { std::string reason; };

using RecoveryResult = std::variant<RecoveryRecovered, RecoveryPartial, RecoveryEscalation>;

struct RecoveryEvent {
    std::string kind;
    std::string detail;
};

struct RecoveryContext {
    std::unordered_map<std::string, uint32_t> attempts; // scenario name → count
    std::vector<RecoveryEvent> events;
    std::optional<std::size_t> fail_at_step; // For testing: force failure at step N
};

// Returns the hard-coded recipe for a scenario
[[nodiscard]] RecoveryRecipe recipe_for(FailureScenario scenario);

// Attempt recovery: enforces one-attempt policy
[[nodiscard]] RecoveryResult attempt_recovery(FailureScenario scenario,
                                              RecoveryContext& ctx);

} // namespace claw::runtime
