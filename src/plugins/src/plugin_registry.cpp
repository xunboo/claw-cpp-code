#include "plugin_registry.hpp"

#include <algorithm>
#include <format>
#include <map>

namespace claw::plugins {

// ─── PluginRegistry ───────────────────────────────────────────────────────────

PluginRegistry::PluginRegistry(std::vector<RegisteredPlugin> plugins)
    : plugins_(std::move(plugins)) {
    // Sort by plugin id, matching Rust's BTreeMap-ordered iteration
    std::ranges::sort(plugins_, [](const RegisteredPlugin& a, const RegisteredPlugin& b) {
        return a.metadata().id < b.metadata().id;
    });
}

const RegisteredPlugin* PluginRegistry::get(std::string_view plugin_id) const noexcept {
    for (auto& p : plugins_)
        if (p.metadata().id == plugin_id) return &p;
    return nullptr;
}

bool PluginRegistry::contains(std::string_view plugin_id) const noexcept {
    return get(plugin_id) != nullptr;
}

std::vector<PluginSummary> PluginRegistry::summaries() const {
    std::vector<PluginSummary> result;
    result.reserve(plugins_.size());
    for (auto& p : plugins_)
        result.push_back(p.summary());
    return result;
}

// Mirrors Rust PluginRegistry::aggregated_hooks():
// fold over enabled plugins, validate each, merge hooks.
tl::expected<PluginHooks, PluginError> PluginRegistry::aggregated_hooks() const {
    PluginHooks acc;
    for (auto& plugin : plugins_) {
        if (!plugin.is_enabled()) continue;
        if (auto r = plugin.validate(); !r) return tl::unexpected(std::move(r.error()));
        acc = acc.merged_with(plugin.hooks());
    }
    return acc;
}

// Mirrors Rust PluginRegistry::aggregated_tools():
// collect tools from enabled+valid plugins; error on duplicate tool name.
tl::expected<std::vector<PluginTool>, PluginError>
PluginRegistry::aggregated_tools() const {
    std::vector<PluginTool> tools;
    std::map<std::string, std::string> seen_names;  // tool_name → plugin_id

    for (auto& plugin : plugins_) {
        if (!plugin.is_enabled()) continue;
        if (auto r = plugin.validate(); !r) return tl::unexpected(std::move(r.error()));

        for (auto& tool : plugin.tools()) {
            auto [it, inserted] = seen_names.emplace(
                tool.definition().name, tool.plugin_id());
            if (!inserted) {
                return tl::unexpected(PluginError::invalid_manifest(std::format(
                    "plugin tool `{}` is defined by both `{}` and `{}`",
                    tool.definition().name, it->second, tool.plugin_id())));
            }
            tools.push_back(tool);
        }
    }
    return tools;
}

// Mirrors Rust PluginRegistry::initialize():
// validate then initialize each enabled plugin in forward order.
tl::expected<void, PluginError> PluginRegistry::initialize() const {
    for (auto& plugin : plugins_) {
        if (!plugin.is_enabled()) continue;
        if (auto r = plugin.validate(); !r) return r;
        if (auto r = plugin.initialize(); !r) return r;
    }
    return {};
}

// Mirrors Rust PluginRegistry::shutdown():
// shutdown enabled plugins in reverse order (no validate on shutdown).
tl::expected<void, PluginError> PluginRegistry::shutdown() const {
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        if (!it->is_enabled()) continue;
        if (auto r = it->shutdown(); !r) return r;
    }
    return {};
}

// ─── PluginRegistryReport ─────────────────────────────────────────────────────

PluginRegistryReport::PluginRegistryReport(PluginRegistry registry,
                                           std::vector<PluginLoadFailure> failures)
    : registry_(std::move(registry))
    , failures_(std::move(failures))
{}

std::span<const PluginLoadFailure> PluginRegistryReport::failures() const noexcept {
    return std::span{failures_};
}

std::vector<PluginSummary> PluginRegistryReport::summaries() const {
    return registry_.summaries();
}

// Mirrors Rust PluginRegistryReport::into_registry():
// return registry if no failures, otherwise wrap failures into a PluginError.
tl::expected<PluginRegistry, PluginError>
PluginRegistryReport::into_registry() && {
    if (failures_.empty())
        return std::move(registry_);
    return tl::unexpected(PluginError::load_failures(std::move(failures_)));
}

// ─── PluginDiscovery ──────────────────────────────────────────────────────────

void PluginDiscovery::push_plugin(PluginDefinition plugin) {
    plugins.push_back(std::move(plugin));
}

void PluginDiscovery::push_failure(PluginLoadFailure failure) {
    failures.push_back(std::move(failure));
}

void PluginDiscovery::extend(PluginDiscovery other) {
    for (auto& p : other.plugins)  plugins.push_back(std::move(p));
    for (auto& f : other.failures) failures.push_back(std::move(f));
}

}  // namespace claw::plugins
