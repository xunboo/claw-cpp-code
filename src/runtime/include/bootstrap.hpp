#pragma once
#include <string>
#include <vector>
#include <variant>
#include <optional>

namespace claw::runtime {

enum class BootstrapPhase {
    CheckDependencies,
    ValidateConfig,
    InitSession,
    LoadSystemPrompt,
    ConnectMcpServers,
    DiscoverTools,
    RunStartupHooks,
    WarmupCache,
    CheckGreenStatus,
    CheckStaleBranches,
    ApplyPermissions,
    Ready,
};

[[nodiscard]] std::string_view bootstrap_phase_name(BootstrapPhase phase) noexcept;

struct BootstrapStep {
    BootstrapPhase phase;
    std::string description;
    bool required{true};
};

struct BootstrapPlan {
    std::vector<BootstrapStep> steps;

    // Deduplicate: each phase appears at most once; later duplicate is dropped
    static BootstrapPlan build(std::vector<BootstrapStep> steps);
};

} // namespace claw::runtime
