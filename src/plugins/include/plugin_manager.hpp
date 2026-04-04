#pragma once

#include <tl/expected.hpp>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "plugin.hpp"
#include "plugin_error.hpp"
#include "plugin_registry.hpp"
#include "plugin_types.hpp"

namespace claw::plugins {

// ─── PluginManagerConfig ─────────────────────────────────────────────────────
struct PluginManagerConfig {
    std::filesystem::path config_home;
    std::map<std::string, bool> enabled_plugins;
    std::vector<std::filesystem::path> external_dirs;
    std::optional<std::filesystem::path> install_root;
    std::optional<std::filesystem::path> registry_path;
    std::optional<std::filesystem::path> bundled_root;

    explicit PluginManagerConfig(std::filesystem::path config_home_)
        : config_home(std::move(config_home_)) {}
};

// ─── Outcome types ────────────────────────────────────────────────────────────
struct InstallOutcome {
    std::string plugin_id;
    std::string version;
    std::filesystem::path install_path;
};

struct UpdateOutcome {
    std::string plugin_id;
    std::string old_version;
    std::string new_version;
    std::filesystem::path install_path;
};

// ─── PluginManager ────────────────────────────────────────────────────────────
// Corresponds to Rust PluginManager
class PluginManager {
public:
    explicit PluginManager(PluginManagerConfig config);

    // Default bundled root (relative to the binary)
    [[nodiscard]] static std::filesystem::path bundled_root();

    [[nodiscard]] std::filesystem::path install_root()   const;
    [[nodiscard]] std::filesystem::path registry_path()  const;
    [[nodiscard]] std::filesystem::path settings_path()  const;

    [[nodiscard]] tl::expected<PluginRegistry,       PluginError> plugin_registry();
    [[nodiscard]] tl::expected<PluginRegistryReport, PluginError> plugin_registry_report();

    [[nodiscard]] tl::expected<std::vector<PluginSummary>,    PluginError> list_plugins();
    [[nodiscard]] tl::expected<std::vector<PluginSummary>,    PluginError> list_installed_plugins();
    [[nodiscard]] tl::expected<std::vector<PluginDefinition>, PluginError> discover_plugins();

    [[nodiscard]] tl::expected<PluginHooks,            PluginError> aggregated_hooks();
    [[nodiscard]] tl::expected<std::vector<PluginTool>,PluginError> aggregated_tools();

    [[nodiscard]] tl::expected<PluginManifest,  PluginError> validate_plugin_source(std::string_view source);
    [[nodiscard]] tl::expected<InstallOutcome,  PluginError> install(std::string_view source);
    [[nodiscard]] tl::expected<void,            PluginError> enable(std::string_view plugin_id);
    [[nodiscard]] tl::expected<void,            PluginError> disable(std::string_view plugin_id);
    [[nodiscard]] tl::expected<void,            PluginError> uninstall(std::string_view plugin_id);
    [[nodiscard]] tl::expected<UpdateOutcome,   PluginError> update(std::string_view plugin_id);

    [[nodiscard]] tl::expected<PluginRegistryReport, PluginError> installed_plugin_registry_report();

private:
    PluginManagerConfig config_;

    // ── Private helpers ────────────────────────────────────────────────────────
    [[nodiscard]] tl::expected<PluginDiscovery, PluginError>
        discover_installed_plugins_with_failures();

    [[nodiscard]] tl::expected<PluginDiscovery, PluginError>
        discover_external_directory_plugins_with_failures(
            const std::vector<PluginDefinition>& existing);

    [[nodiscard]] tl::expected<void, PluginError> sync_bundled_plugins();

    [[nodiscard]] bool is_enabled(const PluginMetadata& metadata) const noexcept;

    [[nodiscard]] tl::expected<void, PluginError> ensure_known_plugin(std::string_view plugin_id);

    [[nodiscard]] tl::expected<InstalledPluginRegistry, PluginError> load_registry()  const;
    [[nodiscard]] tl::expected<void,                    PluginError> store_registry(
        const InstalledPluginRegistry& registry) const;

    [[nodiscard]] tl::expected<void, PluginError>
        write_enabled_state(std::string_view plugin_id, std::optional<bool> enabled);

    [[nodiscard]] tl::expected<PluginRegistry, PluginError> installed_plugin_registry();

    [[nodiscard]] PluginRegistryReport build_registry_report(PluginDiscovery discovery) const;
};

}  // namespace claw::plugins
