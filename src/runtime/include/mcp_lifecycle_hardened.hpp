#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <variant>
#include <chrono>
#include <cstdint>

namespace claw::runtime {

enum class McpLifecyclePhase : uint8_t {
    ConfigLoad = 0,
    TransportInit,
    ProcessSpawn,
    Handshake,
    ToolDiscovery,
    ResourceDiscovery,
    AuthNegotiation,
    HealthCheck,
    ToolRegistration,
    OperationalReady,
    Cleanup,
};

[[nodiscard]] std::string_view mcp_lifecycle_phase_name(McpLifecyclePhase phase) noexcept;
[[nodiscard]] bool is_valid_phase_transition(McpLifecyclePhase from, McpLifecyclePhase to) noexcept;

struct McpErrorSurface {
    McpLifecyclePhase phase;
    std::string server_name;
    std::string message;
    std::map<std::string, std::string> context;
    bool recoverable{false};
    uint64_t timestamp_epoch{0};
};

struct McpPhaseSuccess {
    McpLifecyclePhase phase;
    std::chrono::milliseconds duration;
};
struct McpPhaseFailure {
    McpLifecyclePhase phase;
    McpErrorSurface error;
    bool recoverable{false};
};
struct McpPhaseTimeout {
    McpLifecyclePhase phase;
    std::chrono::milliseconds waited;
};

using McpPhaseResult = std::variant<McpPhaseSuccess, McpPhaseFailure, McpPhaseTimeout>;

struct McpDegradedReport {
    std::string server_name;
    std::vector<std::string> failed_phases;  // sorted, deduplicated
    std::vector<std::string> missing_tools;  // sorted, deduplicated

    static McpDegradedReport build(std::string server_name,
                                   std::vector<std::string> failed_phases,
                                   std::vector<std::string> missing_tools);
};

class McpLifecycleValidator {
public:
    explicit McpLifecycleValidator(std::string server_name)
        : server_name_(std::move(server_name)), current_phase_(McpLifecyclePhase::ConfigLoad) {}

    // Returns false if transition is invalid
    [[nodiscard]] bool run_phase(McpLifecyclePhase next_phase);
    void record_failure(McpErrorSurface error);
    void record_timeout(McpLifecyclePhase phase, std::chrono::milliseconds waited);

    [[nodiscard]] McpLifecyclePhase current_phase() const noexcept { return current_phase_; }
    [[nodiscard]] const std::vector<McpPhaseResult>& results() const noexcept { return results_; }
    [[nodiscard]] bool has_failures() const noexcept;
    [[nodiscard]] McpDegradedReport degraded_report() const;

private:
    std::string server_name_;
    McpLifecyclePhase current_phase_;
    std::vector<McpPhaseResult> results_;
};

} // namespace claw::runtime
