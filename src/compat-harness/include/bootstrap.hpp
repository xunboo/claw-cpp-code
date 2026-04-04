#pragma once

#include <algorithm>
#include <span>
#include <vector>

namespace claw {

// ---------------------------------------------------------------------------
// BootstrapPhase
// ---------------------------------------------------------------------------

enum class BootstrapPhase {
    CliEntry,
    FastPathVersion,
    StartupProfiler,
    SystemPromptFastPath,
    ChromeMcpFastPath,
    DaemonWorkerFastPath,
    BridgeFastPath,
    DaemonFastPath,
    BackgroundSessionFastPath,
    TemplateFastPath,
    EnvironmentRunnerFastPath,
    MainRuntime,
};

// ---------------------------------------------------------------------------
// BootstrapPlan
// ---------------------------------------------------------------------------

class BootstrapPlan {
public:
    // Construct from a list of phases, deduplicating while preserving order.
    [[nodiscard]] static BootstrapPlan from_phases(std::vector<BootstrapPhase> phases) {
        std::vector<BootstrapPhase> deduped;
        for (auto phase : phases) {
            if (std::find(deduped.begin(), deduped.end(), phase) == deduped.end()) {
                deduped.push_back(phase);
            }
        }
        return BootstrapPlan{std::move(deduped)};
    }

    // The default plan that matches the standard Claude Code bootstrap sequence.
    [[nodiscard]] static BootstrapPlan claude_code_default() {
        return from_phases({
            BootstrapPhase::CliEntry,
            BootstrapPhase::FastPathVersion,
            BootstrapPhase::StartupProfiler,
            BootstrapPhase::SystemPromptFastPath,
            BootstrapPhase::ChromeMcpFastPath,
            BootstrapPhase::DaemonWorkerFastPath,
            BootstrapPhase::BridgeFastPath,
            BootstrapPhase::DaemonFastPath,
            BootstrapPhase::BackgroundSessionFastPath,
            BootstrapPhase::TemplateFastPath,
            BootstrapPhase::EnvironmentRunnerFastPath,
            BootstrapPhase::MainRuntime,
        });
    }

    [[nodiscard]] std::span<const BootstrapPhase> phases() const noexcept {
        return phases_;
    }

    [[nodiscard]] bool operator==(const BootstrapPlan& other) const noexcept {
        return phases_ == other.phases_;
    }

private:
    explicit BootstrapPlan(std::vector<BootstrapPhase> phases)
        : phases_(std::move(phases)) {}

    std::vector<BootstrapPhase> phases_;
};

} // namespace claw