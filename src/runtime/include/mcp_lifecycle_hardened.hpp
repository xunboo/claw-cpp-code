#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <variant>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <set>

namespace claw::runtime {

enum class McpLifecyclePhase : uint8_t {
    ConfigLoad = 0,
    ServerRegistration,
    SpawnConnect,
    InitializeHandshake,
    ToolDiscovery,
    ResourceDiscovery,
    Invocation,
    ErrorSurfacing,
    Ready,
    Shutdown,
};

[[nodiscard]] std::string_view mcp_lifecycle_phase_name(McpLifecyclePhase phase) noexcept;
[[nodiscard]] bool is_valid_phase_transition(McpLifecyclePhase from, McpLifecyclePhase to) noexcept;
[[nodiscard]] std::vector<McpLifecyclePhase> all_phases() noexcept;

struct McpErrorSurface {
    McpLifecyclePhase phase;
    std::optional<std::string> server_name;
    std::string message;
    std::map<std::string, std::string> context;
    bool recoverable{false};
    uint64_t timestamp_epoch{0};

    // Convenience constructor matching Rust's McpErrorSurface::new
    static McpErrorSurface make(McpLifecyclePhase phase,
                                std::optional<std::string> server_name,
                                std::string message,
                                std::map<std::string, std::string> context = {},
                                bool recoverable = false) {
        return McpErrorSurface{
            phase,
            std::move(server_name),
            std::move(message),
            std::move(context),
            recoverable,
            0
        };
    }
};

struct McpPhaseSuccess {
    McpLifecyclePhase phase;
    std::chrono::milliseconds duration;
};
struct McpPhaseFailure {
    McpLifecyclePhase phase;
    McpErrorSurface error;
};
struct McpPhaseTimeout {
    McpLifecyclePhase phase;
    std::chrono::milliseconds waited;
    McpErrorSurface error;
};

using McpPhaseResult = std::variant<McpPhaseSuccess, McpPhaseFailure, McpPhaseTimeout>;

struct McpFailedServer {
    std::string server_name;
    McpLifecyclePhase phase;
    McpErrorSurface error;
};

struct McpDegradedReport {
    std::vector<std::string> working_servers;
    std::vector<McpFailedServer> failed_servers;
    std::vector<std::string> available_tools;
    std::vector<std::string> missing_tools;

    // Legacy single-server builder (kept for backward compat)
    static McpDegradedReport build(std::string server_name,
                                   std::vector<std::string> failed_phases,
                                   std::vector<std::string> missing_tools);

    // Full constructor matching Rust's McpDegradedReport::new
    static McpDegradedReport create(
        std::vector<std::string> working_servers,
        std::vector<McpFailedServer> failed_servers,
        std::vector<std::string> available_tools,
        std::vector<std::string> expected_tools);
};

void to_json(nlohmann::json& j, const McpErrorSurface& e);
void to_json(nlohmann::json& j, const McpFailedServer& fs);
void to_json(nlohmann::json& j, const McpDegradedReport& r);

/// Internal lifecycle state tracking.
struct McpLifecycleState {
    std::optional<McpLifecyclePhase> current_phase_;
    std::vector<McpLifecyclePhase> phase_history;
    std::vector<McpErrorSurface> errors;
    std::vector<McpPhaseResult> phase_results;

    [[nodiscard]] std::optional<McpLifecyclePhase> current_phase() const { return current_phase_; }
    void record_phase(McpLifecyclePhase phase);
    void record_error(McpErrorSurface error);
    void record_result(McpPhaseResult result);
    [[nodiscard]] bool can_resume_after_error() const;
};

class McpLifecycleValidator {
public:
    McpLifecycleValidator() = default;

    /// Run a lifecycle phase transition. Returns Success or Failure.
    [[nodiscard]] McpPhaseResult run_phase(McpLifecyclePhase next_phase);

    /// Record a failure. Returns a Failure result.
    [[nodiscard]] McpPhaseResult record_failure(McpErrorSurface error);

    /// Record a timeout. Returns a Timeout result.
    [[nodiscard]] McpPhaseResult record_timeout(McpLifecyclePhase phase,
                                                 std::chrono::milliseconds waited,
                                                 std::map<std::string, std::string> context = {});

    [[nodiscard]] const McpLifecycleState& state() const noexcept { return state_; }
    [[nodiscard]] bool has_failures() const noexcept;

private:
    McpLifecycleState state_;
};

} // namespace claw::runtime
