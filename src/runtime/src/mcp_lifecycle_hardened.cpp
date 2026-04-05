#include "mcp_lifecycle_hardened.hpp"
#include <algorithm>
#include <chrono>
#include <format>

namespace claw::runtime {

std::string_view mcp_lifecycle_phase_name(McpLifecyclePhase phase) noexcept {
    switch (phase) {
        case McpLifecyclePhase::ConfigLoad:           return "config_load";
        case McpLifecyclePhase::ServerRegistration:   return "server_registration";
        case McpLifecyclePhase::SpawnConnect:         return "spawn_connect";
        case McpLifecyclePhase::InitializeHandshake:  return "initialize_handshake";
        case McpLifecyclePhase::ToolDiscovery:        return "tool_discovery";
        case McpLifecyclePhase::ResourceDiscovery:    return "resource_discovery";
        case McpLifecyclePhase::Invocation:           return "invocation";
        case McpLifecyclePhase::ErrorSurfacing:       return "error_surfacing";
        case McpLifecyclePhase::Ready:                return "ready";
        case McpLifecyclePhase::Shutdown:             return "shutdown";
        default: return "unknown";
    }
}

std::vector<McpLifecyclePhase> all_phases() noexcept {
    return {
        McpLifecyclePhase::ConfigLoad,
        McpLifecyclePhase::ServerRegistration,
        McpLifecyclePhase::SpawnConnect,
        McpLifecyclePhase::InitializeHandshake,
        McpLifecyclePhase::ToolDiscovery,
        McpLifecyclePhase::ResourceDiscovery,
        McpLifecyclePhase::Invocation,
        McpLifecyclePhase::ErrorSurfacing,
        McpLifecyclePhase::Ready,
        McpLifecyclePhase::Shutdown,
    };
}

bool is_valid_phase_transition(McpLifecyclePhase from, McpLifecyclePhase to) noexcept {
    using P = McpLifecyclePhase;
    // Shutdown is always reachable
    if (to == P::Shutdown) return true;
    // ErrorSurfacing is always reachable
    if (to == P::ErrorSurfacing) return true;
    // Ready can be reached from ToolDiscovery, ResourceDiscovery, Invocation, ErrorSurfacing, Ready
    if (to == P::Ready) {
        return from == P::ToolDiscovery
            || from == P::ResourceDiscovery
            || from == P::Invocation
            || from == P::ErrorSurfacing
            || from == P::Ready;
    }
    // Invocation from Ready
    if (to == P::Invocation) return from == P::Ready;
    // Forward-only for init sequence
    auto f = static_cast<uint8_t>(from);
    auto t = static_cast<uint8_t>(to);
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
    // Legacy builder: construct with single server as failed
    McpFailedServer fs;
    fs.server_name = server_name;
    fs.phase = McpLifecyclePhase::ToolDiscovery;
    fs.error.phase = McpLifecyclePhase::ToolDiscovery;
    fs.error.server_name = server_name;
    fs.error.message = "legacy degraded report";
    return McpDegradedReport{{}, {std::move(fs)}, {}, std::move(missing_tools)};
}

McpDegradedReport McpDegradedReport::create(
    std::vector<std::string> working_servers,
    std::vector<McpFailedServer> failed_servers,
    std::vector<std::string> available_tools,
    std::vector<std::string> expected_tools) {
    // Deduplicate and sort working_servers
    std::sort(working_servers.begin(), working_servers.end());
    working_servers.erase(std::unique(working_servers.begin(), working_servers.end()), working_servers.end());
    // Deduplicate and sort available_tools
    std::sort(available_tools.begin(), available_tools.end());
    available_tools.erase(std::unique(available_tools.begin(), available_tools.end()), available_tools.end());
    // Compute missing_tools = expected - available
    std::set<std::string> available_set(available_tools.begin(), available_tools.end());
    std::sort(expected_tools.begin(), expected_tools.end());
    expected_tools.erase(std::unique(expected_tools.begin(), expected_tools.end()), expected_tools.end());
    std::vector<std::string> missing;
    for (auto& t : expected_tools) {
        if (available_set.find(t) == available_set.end())
            missing.push_back(t);
    }

    return McpDegradedReport{
        std::move(working_servers),
        std::move(failed_servers),
        std::move(available_tools),
        std::move(missing)
    };
}

// ── JSON serialization ───────────────────────────────────────────────────────

void to_json(nlohmann::json& j, const McpErrorSurface& e) {
    j = nlohmann::json{
        {"phase", mcp_lifecycle_phase_name(e.phase)},
        {"message", e.message},
        {"recoverable", e.recoverable}
    };
    if (e.server_name.has_value()) {
        j["server_name"] = *e.server_name;
    }
    if (!e.context.empty()) {
        nlohmann::json ctx = nlohmann::json::object();
        for (auto& [k, v] : e.context) ctx[k] = v;
        j["context"] = ctx;
    }
}

void to_json(nlohmann::json& j, const McpFailedServer& fs) {
    j = nlohmann::json{
        {"server_name", fs.server_name},
        {"phase", mcp_lifecycle_phase_name(fs.phase)}
    };
    nlohmann::json err_j;
    to_json(err_j, fs.error);
    j["error"] = err_j;
}

void to_json(nlohmann::json& j, const McpDegradedReport& r) {
    j = nlohmann::json{
        {"working_servers", r.working_servers},
        {"available_tools", r.available_tools},
        {"missing_tools", r.missing_tools}
    };
    auto fs_arr = nlohmann::json::array();
    for (auto& fs : r.failed_servers) {
        nlohmann::json fs_j;
        to_json(fs_j, fs);
        fs_arr.push_back(fs_j);
    }
    j["failed_servers"] = fs_arr;
}

// ---------------------------------------------------------------------------
// McpLifecycleState
// ---------------------------------------------------------------------------

void McpLifecycleState::record_phase(McpLifecyclePhase phase) {
    current_phase_ = phase;
    phase_history.push_back(phase);
}

void McpLifecycleState::record_error(McpErrorSurface error) {
    errors.push_back(std::move(error));
}

void McpLifecycleState::record_result(McpPhaseResult result) {
    phase_results.push_back(std::move(result));
}

bool McpLifecycleState::can_resume_after_error() const {
    if (phase_results.empty()) return false;
    const auto& last = phase_results.back();
    if (auto* failure = std::get_if<McpPhaseFailure>(&last)) {
        return failure->error.recoverable;
    }
    if (auto* timeout = std::get_if<McpPhaseTimeout>(&last)) {
        return timeout->error.recoverable;
    }
    return false;
}

// ---------------------------------------------------------------------------
// McpLifecycleValidator
// ---------------------------------------------------------------------------

McpPhaseResult McpLifecycleValidator::run_phase(McpLifecyclePhase phase) {
    auto started = std::chrono::steady_clock::now();

    if (auto current = state_.current_phase()) {
        // Check non-recoverable error when trying to return to Ready
        if (*current == McpLifecyclePhase::ErrorSurfacing
            && phase == McpLifecyclePhase::Ready
            && !state_.can_resume_after_error())
        {
            return record_failure(McpErrorSurface::make(
                phase,
                std::nullopt,
                "cannot return to ready after a non-recoverable MCP lifecycle failure",
                {{"from", std::string(mcp_lifecycle_phase_name(*current))},
                 {"to", std::string(mcp_lifecycle_phase_name(phase))}},
                false));
        }

        if (!is_valid_phase_transition(*current, phase)) {
            return record_failure(McpErrorSurface::make(
                phase,
                std::nullopt,
                std::format("invalid MCP lifecycle transition from {} to {}",
                    mcp_lifecycle_phase_name(*current), mcp_lifecycle_phase_name(phase)),
                {{"from", std::string(mcp_lifecycle_phase_name(*current))},
                 {"to", std::string(mcp_lifecycle_phase_name(phase))}},
                false));
        }
    } else if (phase != McpLifecyclePhase::ConfigLoad) {
        return record_failure(McpErrorSurface::make(
            phase,
            std::nullopt,
            std::format("invalid initial MCP lifecycle phase {}", mcp_lifecycle_phase_name(phase)),
            {{"phase", std::string(mcp_lifecycle_phase_name(phase))}},
            false));
    }

    state_.record_phase(phase);
    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    McpPhaseResult result = McpPhaseSuccess{phase, dur};
    state_.record_result(result);
    return result;
}

McpPhaseResult McpLifecycleValidator::record_failure(McpErrorSurface error) {
    auto phase = error.phase;
    state_.record_error(error);
    state_.record_phase(McpLifecyclePhase::ErrorSurfacing);
    McpPhaseResult result = McpPhaseFailure{phase, std::move(error)};
    state_.record_result(result);
    return result;
}

McpPhaseResult McpLifecycleValidator::record_timeout(McpLifecyclePhase phase,
                                                      std::chrono::milliseconds waited,
                                                      std::map<std::string, std::string> context) {
    auto error = McpErrorSurface::make(
        phase,
        std::nullopt,
        std::format("{} timed out after {}ms", mcp_lifecycle_phase_name(phase), waited.count()),
        std::move(context),
        true);
    state_.record_error(error);
    state_.record_phase(McpLifecyclePhase::ErrorSurfacing);
    McpPhaseResult result = McpPhaseTimeout{phase, waited, std::move(error)};
    state_.record_result(result);
    return result;
}

bool McpLifecycleValidator::has_failures() const noexcept {
    for (const auto& r : state_.phase_results) {
        if (std::holds_alternative<McpPhaseFailure>(r)) return true;
        if (std::holds_alternative<McpPhaseTimeout>(r)) return true;
    }
    return false;
}

} // namespace claw::runtime
