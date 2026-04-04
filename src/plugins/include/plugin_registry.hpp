#pragma once

#include <tl/expected.hpp>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "plugin.hpp"
#include "plugin_error.hpp"
#include "plugin_tool.hpp"
#include "plugin_types.hpp"

namespace claw::plugins {

// ─── PluginRegistry ───────────────────────────────────────────────────────────
// Corresponds to Rust PluginRegistry
class PluginRegistry {
public:
    explicit PluginRegistry(std::vector<RegisteredPlugin> plugins);
    PluginRegistry() = default;

    [[nodiscard]] std::span<const RegisteredPlugin> plugins() const noexcept {
        return std::span{plugins_};
    }

    [[nodiscard]] const RegisteredPlugin* get(std::string_view plugin_id) const noexcept;
    [[nodiscard]] bool contains(std::string_view plugin_id) const noexcept;

    [[nodiscard]] std::vector<PluginSummary> summaries() const;

    [[nodiscard]] tl::expected<PluginHooks, PluginError>      aggregated_hooks() const;
    [[nodiscard]] tl::expected<std::vector<PluginTool>, PluginError> aggregated_tools() const;

    [[nodiscard]] tl::expected<void, PluginError> initialize() const;
    [[nodiscard]] tl::expected<void, PluginError> shutdown()   const;

    // Expose mutable vector for internal construction (PluginManager)
    std::vector<RegisteredPlugin>& mutable_plugins() noexcept { return plugins_; }

private:
    std::vector<RegisteredPlugin> plugins_;
};

// ─── PluginRegistryReport ─────────────────────────────────────────────────────
// Corresponds to Rust PluginRegistryReport
class PluginRegistryReport {
public:
    PluginRegistryReport(PluginRegistry registry, std::vector<PluginLoadFailure> failures);

    [[nodiscard]] const PluginRegistry&             registry()     const noexcept { return registry_;  }
    [[nodiscard]] std::span<const PluginLoadFailure> failures()     const noexcept;
    [[nodiscard]] bool                              has_failures() const noexcept { return !failures_.empty(); }
    [[nodiscard]] std::vector<PluginSummary>        summaries()    const;

    // Consumes this report: returns registry on success or PluginError if there were failures.
    [[nodiscard]] tl::expected<PluginRegistry, PluginError> into_registry() &&;

private:
    PluginRegistry registry_;
    std::vector<PluginLoadFailure> failures_;
};

// ─── Internal PluginDiscovery helper ─────────────────────────────────────────
struct PluginDiscovery {
    std::vector<PluginDefinition>   plugins;
    std::vector<PluginLoadFailure>  failures;

    void push_plugin(PluginDefinition plugin);
    void push_failure(PluginLoadFailure failure);
    void extend(PluginDiscovery other);
};

}  // namespace claw::plugins
