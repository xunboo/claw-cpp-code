#include "bootstrap.hpp"
#include <unordered_set>

namespace claw::runtime {

std::string_view bootstrap_phase_name(BootstrapPhase phase) noexcept {
    switch (phase) {
        case BootstrapPhase::CheckDependencies:  return "check_dependencies";
        case BootstrapPhase::ValidateConfig:     return "validate_config";
        case BootstrapPhase::InitSession:        return "init_session";
        case BootstrapPhase::LoadSystemPrompt:   return "load_system_prompt";
        case BootstrapPhase::ConnectMcpServers:  return "connect_mcp_servers";
        case BootstrapPhase::DiscoverTools:      return "discover_tools";
        case BootstrapPhase::RunStartupHooks:    return "run_startup_hooks";
        case BootstrapPhase::WarmupCache:        return "warmup_cache";
        case BootstrapPhase::CheckGreenStatus:   return "check_green_status";
        case BootstrapPhase::CheckStaleBranches: return "check_stale_branches";
        case BootstrapPhase::ApplyPermissions:   return "apply_permissions";
        case BootstrapPhase::Ready:              return "ready";
    }
    return "unknown";
}

BootstrapPlan BootstrapPlan::build(std::vector<BootstrapStep> steps) {
    BootstrapPlan plan;
    std::unordered_set<int> seen;

    for (auto& step : steps) {
        int key = static_cast<int>(step.phase);
        if (seen.insert(key).second) {
            plan.steps.push_back(std::move(step));
        }
        // Duplicates are dropped (first occurrence wins)
    }
    return plan;
}

} // namespace claw::runtime
