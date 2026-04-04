// plugin_lifecycle.cpp — translated from Rust plugin_lifecycle.rs
//
// Mapping notes:
//
//   Rust PluginState enum              C++ PluginState variant
//   ────────────────────────────────   ────────────────────────────────────────
//   Unconfigured                     → PluginStateUninitialized
//   Validated                        → PluginStateConfigValidated
//   Starting                         → PluginStateStarting
//   Healthy                          → PluginStateHealthy{servers}
//   Degraded{healthy_servers,         → PluginStateDegraded{healthy_servers,
//            failed_servers}                                  failed_servers, mode}
//   Failed{reason}                   → PluginStateFailed{reason}
//   ShuttingDown / Stopped           → PluginStateShutdown
//
//   Rust ServerStatus::Healthy/Degraded/Failed → ServerStatusKind::Healthy/Degraded/Failed
//
// plugin_state_from_servers()
//   Mirrors Rust PluginState::from_servers():
//   - empty servers   → PluginStateFailed{"no servers available"}
//   - all failed      → PluginStateFailed{"all N servers failed"}
//   - any failed or
//     degraded        → PluginStateDegraded{healthy_names, failed_health_vec, mode}
//   - all healthy     → PluginStateHealthy{servers}
//
//   "Healthy" for purposes of the healthy_servers list means status != Failed
//   (i.e. Degraded servers count as healthy, mirroring Rust).
//
// degraded_mode(state)
//   Returns the DegradedMode embedded in PluginStateDegraded, or nullopt.
//   In the Rust code this was a method on PluginHealthcheck that also took
//   a DiscoveryResult to fill available_tools; in C++ the mode is pre-built
//   inside plugin_state_from_servers() using the server capabilities, and the
//   degraded_mode() free function simply projects it out of the variant.
//
// PluginLifecycleEventKind display strings match Rust Display impl:
//   ConfigValidated → "config_validated"
//   StartupHealthy  → "startup_healthy"
//   StartupDegraded → "startup_degraded"
//   StartupFailed   → "startup_failed"
//   Shutdown        → "shutdown"

#include "plugin_lifecycle.hpp"
#include <algorithm>
#include <format>

namespace claw::runtime {

// ---------------------------------------------------------------------------
// plugin_state_from_servers
// ---------------------------------------------------------------------------

PluginState plugin_state_from_servers(const std::vector<ServerHealth>& servers) {
    // empty → mirrors Rust: Failed{"no servers available"}
    if (servers.empty()) {
        return PluginStateFailed{"no servers available"};
    }

    // Partition: healthy_servers (status != Failed) and failed_servers (status == Failed).
    // This mirrors Rust exactly: Degraded-status servers are included in healthy_servers.
    std::vector<std::string>   healthy_names;
    std::vector<ServerHealth>  failed_health;
    bool has_degraded = false;

    for (const auto& s : servers) {
        if (s.status == ServerStatusKind::Failed) {
            failed_health.push_back(s);
        } else {
            healthy_names.push_back(s.server_name);
            if (s.status == ServerStatusKind::Degraded) {
                has_degraded = true;
            }
        }
    }

    // All servers failed → Failed
    if (healthy_names.empty()) {
        return PluginStateFailed{
            std::format("all {} servers failed", failed_health.size())
        };
    }

    // No failures at all and no degraded servers → Healthy
    if (failed_health.empty() && !has_degraded) {
        return PluginStateHealthy{servers};
    }

    // Mixed state → Degraded.
    // Build tool availability from capabilities, matching Rust degraded_mode():
    //   available_tools   ← capabilities of non-Failed servers
    //   unavailable_tools ← capabilities of Failed servers
    std::vector<std::string> available_tools;
    std::vector<std::string> unavailable_tools;

    for (const auto& s : servers) {
        if (s.status != ServerStatusKind::Failed) {
            for (const auto& cap : s.capabilities) {
                available_tools.push_back(cap);
            }
        } else {
            for (const auto& cap : s.capabilities) {
                unavailable_tools.push_back(cap);
            }
        }
    }

    // Reason string mirrors Rust: "{N} servers healthy, {M} servers failed"
    std::string reason = std::format(
        "{} servers healthy, {} servers failed",
        healthy_names.size(),
        failed_health.size());

    return PluginStateDegraded{
        .healthy_servers = std::move(healthy_names),
        .failed_servers  = [&] {
            std::vector<std::string> names;
            names.reserve(failed_health.size());
            for (const auto& fh : failed_health) {
                names.push_back(fh.server_name);
            }
            return names;
        }(),
        .mode = DegradedMode{
            .available_tools   = std::move(available_tools),
            .unavailable_tools = std::move(unavailable_tools),
            .reason            = std::move(reason),
        },
    };
}

// ---------------------------------------------------------------------------
// degraded_mode — projects DegradedMode out of a PluginStateDegraded
// ---------------------------------------------------------------------------

std::optional<DegradedMode> degraded_mode(const PluginState& state) {
    if (const auto* d = std::get_if<PluginStateDegraded>(&state)) {
        return d->mode;
    }
    return std::nullopt;
}

} // namespace claw::runtime
