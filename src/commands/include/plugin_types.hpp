#pragma once
// Minimal forward declarations / stub types mirroring the Rust `plugins` crate.
// A real integration would include headers from the converted plugins library.

#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace plugins {

// ---- PluginMetadata ----------------------------------------------------------

struct PluginMetadata {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    bool        default_enabled{false};
};

// ---- PluginSummary -----------------------------------------------------------

struct PluginSummary {
    PluginMetadata metadata;
    bool           enabled{false};
};

// ---- InstallOutcome / UpdateOutcome -----------------------------------------

struct InstallOutcome {
    std::string              plugin_id;
    std::string              version;
    std::filesystem::path    install_path;
};

struct UpdateOutcome {
    std::string              plugin_id;
    std::string              old_version;
    std::string              new_version;
    std::filesystem::path    install_path;
};

// ---- PluginError -------------------------------------------------------------

class PluginError : public std::exception {
public:
    enum class Kind { Io, Json, ManifestValidation, LoadFailures, InvalidManifest, NotFound, CommandFailed };

    explicit PluginError(Kind k, std::string msg)
        : kind_(k), message_(std::move(msg)) {}

    [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }
    [[nodiscard]] Kind         kind() const noexcept          { return kind_; }

    static PluginError not_found(std::string msg)         { return PluginError{Kind::NotFound,        std::move(msg)}; }
    static PluginError invalid_manifest(std::string msg)  { return PluginError{Kind::InvalidManifest, std::move(msg)}; }
    static PluginError io(std::string msg)                { return PluginError{Kind::Io,              std::move(msg)}; }

private:
    Kind        kind_;
    std::string message_;
};

// ---- PluginManagerConfig & PluginManager ------------------------------------

struct PluginManagerConfig {
    std::filesystem::path              config_home;
    std::map<std::string, bool>        enabled_plugins;
    std::vector<std::filesystem::path> external_dirs;
    std::optional<std::filesystem::path> install_root;
    std::optional<std::filesystem::path> registry_path;
    std::optional<std::filesystem::path> bundled_root;

    explicit PluginManagerConfig(std::filesystem::path home)
        : config_home(std::move(home)) {}
};

// Forward-declared; implementation belongs to the plugins library.
class PluginManager {
public:
    explicit PluginManager(PluginManagerConfig cfg) : config_(std::move(cfg)) {}

    [[nodiscard]] std::vector<PluginSummary> list_installed_plugins() const;
    [[nodiscard]] InstallOutcome             install(std::string_view target);
    void                                     enable(std::string_view plugin_id);
    void                                     disable(std::string_view plugin_id);
    void                                     uninstall(std::string_view plugin_id);
    [[nodiscard]] UpdateOutcome              update(std::string_view plugin_id);

private:
    PluginManagerConfig config_;
};

} // namespace plugins
