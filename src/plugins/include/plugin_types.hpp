#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace claw::plugins {

// ─── Constants ────────────────────────────────────────────────────────────────
inline constexpr const char* EXTERNAL_MARKETPLACE = "external";
inline constexpr const char* BUILTIN_MARKETPLACE  = "builtin";
inline constexpr const char* BUNDLED_MARKETPLACE  = "bundled";
inline constexpr const char* SETTINGS_FILE_NAME   = "settings.json";
inline constexpr const char* REGISTRY_FILE_NAME   = "installed.json";
inline constexpr const char* MANIFEST_FILE_NAME   = "plugin.json";
inline constexpr const char* MANIFEST_RELATIVE_PATH = ".claude-plugin/plugin.json";

// ─── PluginKind ───────────────────────────────────────────────────────────────
enum class PluginKind { Builtin, Bundled, External };

[[nodiscard]] inline constexpr const char* plugin_kind_str(PluginKind k) noexcept {
    switch (k) {
        case PluginKind::Builtin:  return "builtin";
        case PluginKind::Bundled:  return "bundled";
        case PluginKind::External: return "external";
    }
    return "external";
}

[[nodiscard]] inline constexpr const char* plugin_kind_marketplace(PluginKind k) noexcept {
    switch (k) {
        case PluginKind::Builtin:  return BUILTIN_MARKETPLACE;
        case PluginKind::Bundled:  return BUNDLED_MARKETPLACE;
        case PluginKind::External: return EXTERNAL_MARKETPLACE;
    }
    return EXTERNAL_MARKETPLACE;
}

// ─── PluginPermission ─────────────────────────────────────────────────────────
enum class PluginPermission { Read, Write, Execute };

[[nodiscard]] inline constexpr const char* plugin_permission_str(PluginPermission p) noexcept {
    switch (p) {
        case PluginPermission::Read:    return "read";
        case PluginPermission::Write:   return "write";
        case PluginPermission::Execute: return "execute";
    }
    return "read";
}

[[nodiscard]] std::optional<PluginPermission> parse_plugin_permission(std::string_view value) noexcept;

// ─── PluginToolPermission ─────────────────────────────────────────────────────
enum class PluginToolPermission { ReadOnly, WorkspaceWrite, DangerFullAccess };

[[nodiscard]] inline constexpr const char* plugin_tool_permission_str(PluginToolPermission p) noexcept {
    switch (p) {
        case PluginToolPermission::ReadOnly:         return "read-only";
        case PluginToolPermission::WorkspaceWrite:   return "workspace-write";
        case PluginToolPermission::DangerFullAccess: return "danger-full-access";
    }
    return "danger-full-access";
}

[[nodiscard]] std::optional<PluginToolPermission> parse_plugin_tool_permission(std::string_view value) noexcept;

// ─── PluginMetadata ───────────────────────────────────────────────────────────
struct PluginMetadata {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    PluginKind kind{PluginKind::External};
    std::string source;
    bool default_enabled{false};
    std::optional<std::filesystem::path> root;
};

// ─── PluginHooks ──────────────────────────────────────────────────────────────
struct PluginHooks {
    std::vector<std::string> pre_tool_use;
    std::vector<std::string> post_tool_use;
    std::vector<std::string> post_tool_use_failure;

    [[nodiscard]] bool is_empty() const noexcept {
        return pre_tool_use.empty() && post_tool_use.empty() && post_tool_use_failure.empty();
    }

    [[nodiscard]] PluginHooks merged_with(const PluginHooks& other) const;
};

// ─── PluginLifecycle ──────────────────────────────────────────────────────────
struct PluginLifecycle {
    std::vector<std::string> init;
    std::vector<std::string> shutdown;

    [[nodiscard]] bool is_empty() const noexcept {
        return init.empty() && shutdown.empty();
    }
};

// ─── PluginToolDefinition ─────────────────────────────────────────────────────
struct PluginToolDefinition {
    std::string name;
    std::optional<std::string> description;
    nlohmann::json input_schema;
};

// ─── PluginCommandManifest ────────────────────────────────────────────────────
struct PluginCommandManifest {
    std::string name;
    std::string description;
    std::string command;
};

// ─── PluginToolManifest ───────────────────────────────────────────────────────
struct PluginToolManifest {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    std::string command;
    std::vector<std::string> args;
    PluginToolPermission required_permission{PluginToolPermission::DangerFullAccess};
};

// ─── PluginManifest ───────────────────────────────────────────────────────────
struct PluginManifest {
    std::string name;
    std::string version;
    std::string description;
    std::vector<PluginPermission> permissions;
    bool default_enabled{false};
    PluginHooks hooks;
    PluginLifecycle lifecycle;
    std::vector<PluginToolManifest> tools;
    std::vector<PluginCommandManifest> commands;
};

// ─── Raw deserialization types ────────────────────────────────────────────────
struct RawPluginToolManifest {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    std::string command;
    std::vector<std::string> args;
    std::string required_permission{"danger-full-access"};
};

struct RawPluginManifest {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> permissions;
    bool default_enabled{false};
    PluginHooks hooks;
    PluginLifecycle lifecycle;
    std::vector<RawPluginToolManifest> tools;
    std::vector<PluginCommandManifest> commands;
};

// ─── PluginInstallSource ──────────────────────────────────────────────────────
struct LocalPathSource  { std::filesystem::path path; };
struct GitUrlSource     { std::string url; };
using PluginInstallSource = std::variant<LocalPathSource, GitUrlSource>;

[[nodiscard]] std::string describe_install_source(const PluginInstallSource& source);

// ─── InstalledPluginRecord ────────────────────────────────────────────────────
struct InstalledPluginRecord {
    PluginKind kind{PluginKind::External};
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::filesystem::path install_path;
    PluginInstallSource source;
    uint64_t installed_at_unix_ms{0};
    uint64_t updated_at_unix_ms{0};
};

// ─── InstalledPluginRegistry ──────────────────────────────────────────────────
struct InstalledPluginRegistry {
    std::map<std::string, InstalledPluginRecord> plugins;
};

// ─── JSON (de)serialisation helpers ──────────────────────────────────────────
void to_json(nlohmann::json& j, const PluginKind& k);
void from_json(const nlohmann::json& j, PluginKind& k);

void to_json(nlohmann::json& j, const PluginHooks& h);
void from_json(const nlohmann::json& j, PluginHooks& h);

void to_json(nlohmann::json& j, const PluginLifecycle& l);
void from_json(const nlohmann::json& j, PluginLifecycle& l);

void to_json(nlohmann::json& j, const PluginInstallSource& s);
void from_json(const nlohmann::json& j, PluginInstallSource& s);

void to_json(nlohmann::json& j, const InstalledPluginRecord& r);
void from_json(const nlohmann::json& j, InstalledPluginRecord& r);

void to_json(nlohmann::json& j, const InstalledPluginRegistry& r);
void from_json(const nlohmann::json& j, InstalledPluginRegistry& r);

void from_json(const nlohmann::json& j, RawPluginManifest& r);
void from_json(const nlohmann::json& j, RawPluginToolManifest& r);
void from_json(const nlohmann::json& j, PluginCommandManifest& r);

}  // namespace claw::plugins
