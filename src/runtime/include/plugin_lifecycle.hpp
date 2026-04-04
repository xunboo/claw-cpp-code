#pragma once
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <tl/expected.hpp>
#include <memory>

namespace claw::runtime {

enum class ServerStatusKind { Healthy, Degraded, Failed };

struct ServerHealth {
    std::string server_name;
    ServerStatusKind status{ServerStatusKind::Healthy};
    std::vector<std::string> capabilities;
    std::optional<std::string> last_error;
};

struct DegradedMode {
    std::vector<std::string> available_tools;
    std::vector<std::string> unavailable_tools;
    std::string reason;
};

struct DiscoveryResult {
    std::vector<std::string> tools;
    std::vector<std::string> resources;
    bool partial{false};
};

// PluginState variants
struct PluginStateUninitialized {};
struct PluginStateConfigValidated {};
struct PluginStateStarting {};
struct PluginStateHealthy { std::vector<ServerHealth> servers; };
struct PluginStateDegraded {
    std::vector<std::string> healthy_servers;
    std::vector<std::string> failed_servers;
    DegradedMode mode;
};
struct PluginStateFailed { std::string reason; };
struct PluginStateShutdown {};

using PluginState = std::variant<
    PluginStateUninitialized,
    PluginStateConfigValidated,
    PluginStateStarting,
    PluginStateHealthy,
    PluginStateDegraded,
    PluginStateFailed,
    PluginStateShutdown
>;

[[nodiscard]] PluginState plugin_state_from_servers(const std::vector<ServerHealth>& servers);
[[nodiscard]] std::optional<DegradedMode> degraded_mode(const PluginState& state);

enum class PluginLifecycleEventKind {
    ConfigValidated,
    StartupHealthy,
    StartupDegraded,
    StartupFailed,
    Shutdown,
};

struct PluginLifecycleEvent {
    PluginLifecycleEventKind kind;
    std::optional<std::string> detail;
};

// Abstract base class for plugin lifecycle
class PluginLifecycle {
public:
    virtual ~PluginLifecycle() = default;

    [[nodiscard]] virtual tl::expected<void, std::string> validate_config() = 0;
    [[nodiscard]] virtual tl::expected<ServerHealth, std::string> healthcheck(std::string_view server_name) = 0;
    [[nodiscard]] virtual tl::expected<DiscoveryResult, std::string> discover() = 0;
    virtual void shutdown() = 0;
};

class PluginHealthcheck {
public:
    explicit PluginHealthcheck(std::vector<ServerHealth> initial_health)
        : health_(std::move(initial_health)) {}

    [[nodiscard]] static PluginHealthcheck with_health(std::vector<ServerHealth> h) {
        return PluginHealthcheck(std::move(h));
    }

    [[nodiscard]] const std::vector<ServerHealth>& health() const noexcept { return health_; }
    [[nodiscard]] PluginState state() const { return plugin_state_from_servers(health_); }

private:
    std::vector<ServerHealth> health_;
};

} // namespace claw::runtime
