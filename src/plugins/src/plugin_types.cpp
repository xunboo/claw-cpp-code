#include "plugin_types.hpp"

#include <stdexcept>

namespace claw::plugins {

std::optional<PluginPermission> parse_plugin_permission(std::string_view value) noexcept {
    if (value == "read")    return PluginPermission::Read;
    if (value == "write")   return PluginPermission::Write;
    if (value == "execute") return PluginPermission::Execute;
    return std::nullopt;
}

std::optional<PluginToolPermission> parse_plugin_tool_permission(std::string_view value) noexcept {
    if (value == "read-only")          return PluginToolPermission::ReadOnly;
    if (value == "workspace-write")    return PluginToolPermission::WorkspaceWrite;
    if (value == "danger-full-access") return PluginToolPermission::DangerFullAccess;
    return std::nullopt;
}

PluginHooks PluginHooks::merged_with(const PluginHooks& other) const {
    PluginHooks merged = *this;
    merged.pre_tool_use.insert(merged.pre_tool_use.end(),
                               other.pre_tool_use.begin(), other.pre_tool_use.end());
    merged.post_tool_use.insert(merged.post_tool_use.end(),
                                other.post_tool_use.begin(), other.post_tool_use.end());
    merged.post_tool_use_failure.insert(merged.post_tool_use_failure.end(),
                                        other.post_tool_use_failure.begin(),
                                        other.post_tool_use_failure.end());
    return merged;
}

std::string describe_install_source(const PluginInstallSource& source) {
    return std::visit([](auto&& s) -> std::string {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, LocalPathSource>)
            return s.path.string();
        else
            return s.url;
    }, source);
}

// ─── JSON helpers ─────────────────────────────────────────────────────────────

void to_json(nlohmann::json& j, const PluginKind& k) {
    j = plugin_kind_str(k);
}

void from_json(const nlohmann::json& j, PluginKind& k) {
    auto s = j.get<std::string>();
    if (s == "builtin")  { k = PluginKind::Builtin;  return; }
    if (s == "bundled")  { k = PluginKind::Bundled;  return; }
    k = PluginKind::External;
}

void to_json(nlohmann::json& j, const PluginHooks& h) {
    j = nlohmann::json::object();
    if (!h.pre_tool_use.empty())         j["PreToolUse"]         = h.pre_tool_use;
    if (!h.post_tool_use.empty())        j["PostToolUse"]        = h.post_tool_use;
    if (!h.post_tool_use_failure.empty())j["PostToolUseFailure"] = h.post_tool_use_failure;
}

void from_json(const nlohmann::json& j, PluginHooks& h) {
    if (j.contains("PreToolUse"))         j.at("PreToolUse").get_to(h.pre_tool_use);
    if (j.contains("PostToolUse"))        j.at("PostToolUse").get_to(h.post_tool_use);
    if (j.contains("PostToolUseFailure")) j.at("PostToolUseFailure").get_to(h.post_tool_use_failure);
}

void to_json(nlohmann::json& j, const PluginLifecycle& l) {
    j = nlohmann::json::object();
    if (!l.init.empty())     j["Init"]     = l.init;
    if (!l.shutdown.empty()) j["Shutdown"] = l.shutdown;
}

void from_json(const nlohmann::json& j, PluginLifecycle& l) {
    if (j.contains("Init"))     j.at("Init").get_to(l.init);
    if (j.contains("Shutdown")) j.at("Shutdown").get_to(l.shutdown);
}

void to_json(nlohmann::json& j, const PluginInstallSource& s) {
    std::visit([&j](auto&& src) {
        using T = std::decay_t<decltype(src)>;
        if constexpr (std::is_same_v<T, LocalPathSource>)
            j = {{"type", "local_path"}, {"path", src.path.string()}};
        else
            j = {{"type", "git_url"}, {"url", src.url}};
    }, s);
}

void from_json(const nlohmann::json& j, PluginInstallSource& s) {
    auto type = j.at("type").get<std::string>();
    if (type == "local_path")
        s = LocalPathSource{std::filesystem::path{j.at("path").get<std::string>()}};
    else
        s = GitUrlSource{j.at("url").get<std::string>()};
}

void to_json(nlohmann::json& j, const InstalledPluginRecord& r) {
    j = {
        {"kind",                plugin_kind_str(r.kind)},
        {"id",                  r.id},
        {"name",                r.name},
        {"version",             r.version},
        {"description",         r.description},
        {"install_path",        r.install_path.string()},
        {"source",              nlohmann::json(r.source)},
        {"installed_at_unix_ms", r.installed_at_unix_ms},
        {"updated_at_unix_ms",   r.updated_at_unix_ms},
    };
}

void from_json(const nlohmann::json& j, InstalledPluginRecord& r) {
    j.at("id").get_to(r.id);
    j.at("name").get_to(r.name);
    j.at("version").get_to(r.version);
    j.at("description").get_to(r.description);
    r.install_path = std::filesystem::path{j.at("install_path").get<std::string>()};
    j.at("source").get_to(r.source);
    j.at("installed_at_unix_ms").get_to(r.installed_at_unix_ms);
    j.at("updated_at_unix_ms").get_to(r.updated_at_unix_ms);
    if (j.contains("kind"))
        j.at("kind").get_to(r.kind);
    else
        r.kind = PluginKind::External;
}

void to_json(nlohmann::json& j, const InstalledPluginRegistry& r) {
    j = nlohmann::json::object();
    nlohmann::json plugins_obj = nlohmann::json::object();
    for (auto& [id, rec] : r.plugins)
        plugins_obj[id] = rec;
    j["plugins"] = std::move(plugins_obj);
}

void from_json(const nlohmann::json& j, InstalledPluginRegistry& r) {
    if (j.contains("plugins"))
        for (auto& [id, rec] : j.at("plugins").items())
            r.plugins[id] = rec.get<InstalledPluginRecord>();
}

void from_json(const nlohmann::json& j, RawPluginToolManifest& r) {
    j.at("name").get_to(r.name);
    j.at("description").get_to(r.description);
    j.at("inputSchema").get_to(r.input_schema);
    j.at("command").get_to(r.command);
    if (j.contains("args")) j.at("args").get_to(r.args);
    if (j.contains("requiredPermission"))
        j.at("requiredPermission").get_to(r.required_permission);
    else
        r.required_permission = "danger-full-access";
}

void from_json(const nlohmann::json& j, PluginCommandManifest& r) {
    j.at("name").get_to(r.name);
    j.at("description").get_to(r.description);
    j.at("command").get_to(r.command);
}

void from_json(const nlohmann::json& j, RawPluginManifest& r) {
    j.at("name").get_to(r.name);
    j.at("version").get_to(r.version);
    j.at("description").get_to(r.description);
    if (j.contains("permissions"))    j.at("permissions").get_to(r.permissions);
    if (j.contains("defaultEnabled")) j.at("defaultEnabled").get_to(r.default_enabled);
    if (j.contains("hooks"))          j.at("hooks").get_to(r.hooks);
    if (j.contains("lifecycle"))      j.at("lifecycle").get_to(r.lifecycle);
    if (j.contains("tools"))          j.at("tools").get_to(r.tools);
    if (j.contains("commands"))       j.at("commands").get_to(r.commands);
}

}  // namespace claw::plugins
