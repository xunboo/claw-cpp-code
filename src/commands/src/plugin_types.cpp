// plugin_types.cpp — implementations for the plugin types declared in
// commands/include/plugin_types.hpp. These allow the commands module to
// compile and link standalone.

#include "plugin_types.hpp"

namespace plugins {

std::vector<PluginSummary> PluginManager::list_installed_plugins() const {
    // Scan install root for installed plugins
    std::vector<PluginSummary> result;
    if (config_.install_root && std::filesystem::is_directory(*config_.install_root)) {
        for (auto& entry : std::filesystem::directory_iterator(*config_.install_root)) {
            if (entry.is_directory()) {
                PluginMetadata meta;
                meta.id = entry.path().filename().string();
                meta.name = meta.id;
                auto enabled_it = config_.enabled_plugins.find(meta.id);
                bool enabled = (enabled_it != config_.enabled_plugins.end()) && enabled_it->second;
                result.push_back({meta, enabled});
            }
        }
    }
    return result;
}

InstallOutcome PluginManager::install(std::string_view target) {
    auto install_dir = config_.install_root.value_or(config_.config_home / "plugins");
    std::filesystem::create_directories(install_dir);
    auto plugin_dir = install_dir / std::string(target);
    std::filesystem::create_directories(plugin_dir);
    return InstallOutcome{
        std::string(target),
        "0.1.0",
        plugin_dir
    };
}

void PluginManager::enable(std::string_view plugin_id) {
    config_.enabled_plugins[std::string(plugin_id)] = true;
}

void PluginManager::disable(std::string_view plugin_id) {
    config_.enabled_plugins[std::string(plugin_id)] = false;
}

void PluginManager::uninstall(std::string_view plugin_id) {
    auto install_dir = config_.install_root.value_or(config_.config_home / "plugins");
    auto plugin_dir = install_dir / std::string(plugin_id);
    if (std::filesystem::exists(plugin_dir)) {
        std::filesystem::remove_all(plugin_dir);
    }
    config_.enabled_plugins.erase(std::string(plugin_id));
}

UpdateOutcome PluginManager::update(std::string_view plugin_id) {
    return UpdateOutcome{
        std::string(plugin_id),
        "0.1.0",
        "0.1.1",
        config_.install_root.value_or(config_.config_home / "plugins") / std::string(plugin_id)
    };
}

} // namespace plugins
