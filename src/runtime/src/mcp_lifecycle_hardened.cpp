#include "mcp_lifecycle_hardened.hpp"
#include <algorithm>
#include <chrono>
#include <format>

namespace claw::runtime {

std::string_view mcp_lifecycle_phase_name(McpLifecyclePhase phase) noexcept {
    switch (phase) {
        case McpLifecyclePhase::ConfigLoad:         return "config_load";
        case McpLifecyclePhase::TransportInit:      return "transport_init";
        case McpLifecyclePhase::ProcessSpawn:       return "process_spawn";
        case McpLifecyclePhase::Handshake:          return "handshake";
        case McpLifecyclePhase::ToolDiscovery:      return "tool_discovery";
        case McpLifecyclePhase::ResourceDiscovery:  return "resource_discovery";
        case McpLifecyclePhase::AuthNegotiation:    return "auth_negotiation";
        case McpLifecyclePhase::HealthCheck:        return "health_check";
        case McpLifecyclePhase::ToolRegistration:   return "tool_registration";
        case McpLifecyclePhase::OperationalReady:   return "operational_ready";
        case McpLifecyclePhase::Cleanup:            return "cleanup";
    }
    return "unknown";
}

bool is_valid_phase_transition(McpLifecyclePhase from, McpLifecyclePhase to) noexcept {
    // Valid forward transitions (sequential or specific skips)
    using P = McpLifecyclePhase;
    auto f = static_cast<uint8_t>(from);
    auto t = static_cast<uint8_t>(to);
    // Allow advancing to next phase or jumping to Cleanup from any phase
    if (to == P::Cleanup) return true;
    return t == f + 1;
}

McpDegradedReport McpDegradedReport::build(std::string server_name,
                                            std::vector<std::string> failed_phases,
                                            std::vector<std::string> missing_tools) {
    // Deduplicate and sort
    std::sort(failed_phases.begin(), failed_phases.end());
    failed_phases.erase(std::unique(failed_phases.begin(), failed_phases.end()), failed_phases.end());
    std::sort(missing_tools.begin(), missing_tools.end());
    missing_tools.erase(std::unique(missing_tools.begin(), missing_tools.end()), missing_tools.end());
    return McpDegradedReport{std::move(server_name), std::move(failed_phases), std::move(missing_tools)};
}

bool McpLifecycleValidator::run_phase(McpLifecyclePhase next_phase) {
    if (!is_valid_phase_transition(current_phase_, next_phase)) {
        return false;
    }
    auto start = std::chrono::steady_clock::now();
    current_phase_ = next_phase;
    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    results_.push_back(McpPhaseSuccess{next_phase, dur});
    return true;
}

void McpLifecycleValidator::record_failure(McpErrorSurface error) {
    auto phase = error.phase;
    bool recoverable = error.recoverable;
    results_.push_back(McpPhaseFailure{phase, std::move(error), recoverable});
}

void McpLifecycleValidator::record_timeout(McpLifecyclePhase phase, std::chrono::milliseconds waited) {
    results_.push_back(McpPhaseTimeout{phase, waited});
}

bool McpLifecycleValidator::has_failures() const noexcept {
    for (const auto& r : results_) {
        if (std::holds_alternative<McpPhaseFailure>(r)) return true;
        if (std::holds_alternative<McpPhaseTimeout>(r)) return true;
    }
    return false;
}

McpDegradedReport McpLifecycleValidator::degraded_report() const {
    std::vector<std::string> failed;
    for (const auto& r : results_) {
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, McpPhaseFailure>) {
                failed.push_back(std::string(mcp_lifecycle_phase_name(v.phase)));
            } else if constexpr (std::is_same_v<T, McpPhaseTimeout>) {
                failed.push_back(std::string(mcp_lifecycle_phase_name(v.phase)));
            }
        }, r);
    }
    return McpDegradedReport::build(server_name_, std::move(failed), {});
}

} // namespace claw::runtime
