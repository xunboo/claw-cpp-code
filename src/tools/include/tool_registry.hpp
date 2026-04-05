#pragma once
#include <tl/expected.hpp>

#include "tool_types.hpp"
#include "permission_enforcer.hpp"

#include <set>
#include <string>
#include <vector>

#include "plugin_tool.hpp"

namespace claw::tools {

// ── ToolRegistry (manifest registry) ─────────────────────────────────────────

class ToolRegistry {
public:
    explicit ToolRegistry(std::vector<ToolManifestEntry> entries);

    [[nodiscard]] const std::vector<ToolManifestEntry>& entries() const noexcept { return entries_; }

private:
    std::vector<ToolManifestEntry> entries_;
};

// ── GlobalToolRegistry ────────────────────────────────────────────────────────

class GlobalToolRegistry {
public:
    /// Create registry with only built-in tools.
    [[nodiscard]] static GlobalToolRegistry builtin();

    /// Create registry seeded with plugin tools (validates no name conflicts).
    [[nodiscard]] static tl::expected<GlobalToolRegistry, std::string>
        with_plugin_tools(std::vector<claw::plugins::PluginTool> plugin_tools);

    /// Add runtime (MCP / dynamic) tool definitions (validates no name conflicts).
    [[nodiscard]] tl::expected<GlobalToolRegistry, std::string>
        with_runtime_tools(std::vector<RuntimeToolDefinition> runtime_tools) &&;

    /// Attach a permission enforcer to the registry.
    [[nodiscard]] GlobalToolRegistry with_enforcer(runtime::PermissionEnforcer enforcer) &&;

    void set_enforcer(runtime::PermissionEnforcer enforcer);

    /// Normalise and validate an --allowedTools list; returns nullopt if values is empty.
    [[nodiscard]] tl::expected<std::optional<std::set<std::string>>, std::string>
        normalize_allowed_tools(const std::vector<std::string>& values) const;

    /// Return ToolDefinition list (for API request), filtered by allowed_tools.
    [[nodiscard]] std::vector<json>
        definitions(const std::set<std::string>* allowed_tools) const;

    /// Return (name, PermissionMode) pairs for permission policy setup.
    [[nodiscard]] tl::expected<std::vector<std::pair<std::string, PermissionMode>>, std::string>
        permission_specs(const std::set<std::string>* allowed_tools) const;

    [[nodiscard]] bool has_runtime_tool(std::string_view name) const;

    /// Search for deferred tool specs matching query.
    [[nodiscard]] ToolSearchOutput search(
        const std::string& query,
        std::size_t max_results,
        std::optional<std::vector<std::string>> pending_mcp_servers,
        std::optional<nlohmann::json> mcp_degraded = std::nullopt) const;

    /// Execute a tool by name; delegates to built-ins or plugin tools.
    [[nodiscard]] tl::expected<std::string, std::string>
        execute(const std::string& name, const json& input) const;

private:
    GlobalToolRegistry() = default;

    std::vector<claw::plugins::PluginTool>         plugin_tools_;
    std::vector<RuntimeToolDefinition>              runtime_tools_;
    std::optional<runtime::PermissionEnforcer> enforcer_;
};

}  // namespace claw::tools
