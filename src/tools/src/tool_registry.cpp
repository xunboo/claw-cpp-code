#include "tool_registry.hpp"
#include "tool_executor.hpp"
#include "tool_specs.hpp"

#include <algorithm>
#include <tl/expected.hpp>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// PluginTool now included via tool_registry.hpp -> plugin_tool.hpp

namespace claw::tools {

// ── ToolRegistry ──────────────────────────────────────────────────────────────

ToolRegistry::ToolRegistry(std::vector<ToolManifestEntry> entries)
    : entries_(std::move(entries)) {}

// ── GlobalToolRegistry ────────────────────────────────────────────────────────

GlobalToolRegistry GlobalToolRegistry::builtin() {
    GlobalToolRegistry reg;
    return reg;
}

tl::expected<GlobalToolRegistry, std::string>
GlobalToolRegistry::with_plugin_tools(std::vector<plugins::PluginTool> plugin_tools) {
    // Build set of built-in names
    std::set<std::string> builtin_names;
    for (auto& spec : mvp_tool_specs())
        builtin_names.insert(spec.name);

    std::set<std::string> seen_plugin_names;
    for (auto& tool : plugin_tools) {
        const auto& name = tool.definition().name;
        if (builtin_names.count(name))
            return tl::unexpected(
                "plugin tool `" + name + "` conflicts with a built-in tool name");
        if (!seen_plugin_names.insert(name).second)
            return tl::unexpected("duplicate plugin tool name `" + name + "`");
    }

    GlobalToolRegistry reg;
    reg.plugin_tools_ = std::move(plugin_tools);
    return tl::expected<GlobalToolRegistry, std::string>{std::move(reg)};
}

tl::expected<GlobalToolRegistry, std::string>
GlobalToolRegistry::with_runtime_tools(std::vector<RuntimeToolDefinition> runtime_tools) && {
    // Seed seen names from builtins + existing plugin tools
    std::set<std::string> seen_names;
    for (auto& spec : mvp_tool_specs())
        seen_names.insert(spec.name);
    for (auto& tool : plugin_tools_)
        seen_names.insert(tool.definition().name);

    for (auto& tool : runtime_tools) {
        if (!seen_names.insert(tool.name).second)
            return tl::unexpected(
                "runtime tool `" + tool.name + "` conflicts with an existing tool name");
    }

    runtime_tools_ = std::move(runtime_tools);
    return tl::expected<GlobalToolRegistry, std::string>{std::move(*this)};
}

GlobalToolRegistry GlobalToolRegistry::with_enforcer(
    runtime::PermissionEnforcer enforcer) && {
    enforcer_ = std::move(enforcer);
    return std::move(*this);
}

void GlobalToolRegistry::set_enforcer(runtime::PermissionEnforcer enforcer) {
    enforcer_ = std::move(enforcer);
}

tl::expected<std::optional<std::set<std::string>>, std::string>
GlobalToolRegistry::normalize_allowed_tools(const std::vector<std::string>& values) const {
    if (values.empty()) return std::optional<std::set<std::string>>{std::nullopt};

    // Build canonical name list: builtins + plugins + runtime
    std::vector<std::string> canonical_names;
    for (auto& spec : mvp_tool_specs())
        canonical_names.push_back(spec.name);
    for (auto& tool : plugin_tools_)
        canonical_names.push_back(tool.definition().name);
    for (auto& tool : runtime_tools_)
        canonical_names.push_back(tool.name);

    // name_map: normalized -> canonical
    std::map<std::string, std::string> name_map;
    for (auto& name : canonical_names)
        name_map[normalize_tool_name(name)] = name;

    // Inject short aliases
    for (auto [alias, canonical] : {
            std::pair{"read",  "read_file"},
            std::pair{"write", "write_file"},
            std::pair{"edit",  "edit_file"},
            std::pair{"glob",  "glob_search"},
            std::pair{"grep",  "grep_search"},
        })
        name_map[alias] = canonical;

    std::set<std::string> allowed;
    for (auto& value : values) {
        // split on comma or whitespace
        std::istringstream ss(value);
        std::string token;
        // iterate char by char
        std::string cur;
        auto flush = [&]() -> tl::expected<void, std::string> {
            if (cur.empty()) return {};
            auto normalized = normalize_tool_name(cur);
            auto it = name_map.find(normalized);
            if (it == name_map.end()) {
                std::string names_list;
                for (auto& n : canonical_names) {
                    if (!names_list.empty()) names_list += ", ";
                    names_list += n;
                }
                return tl::unexpected(
                    "unsupported tool in --allowedTools: " + cur +
                    " (expected one of: " + names_list + ")");
            }
            allowed.insert(it->second);
            cur.clear();
            return {};
        };

        for (char ch : value) {
            if (ch == ',' || std::isspace(static_cast<unsigned char>(ch))) {
                if (auto r = flush(); !r) return tl::unexpected(r.error());
            } else {
                cur += ch;
            }
        }
        if (auto r = flush(); !r) return tl::unexpected(r.error());
    }

    return std::optional<std::set<std::string>>{std::move(allowed)};
}

std::vector<json>
GlobalToolRegistry::definitions(const std::set<std::string>* allowed_tools) const {
    std::vector<json> result;

    // Built-in tools
    for (auto& spec : mvp_tool_specs()) {
        if (allowed_tools && !allowed_tools->count(spec.name)) continue;
        result.push_back(json{
            {"name",         spec.name},
            {"description",  spec.description},
            {"input_schema", spec.input_schema},
        });
    }

    // Runtime tools
    for (auto& tool : runtime_tools_) {
        if (allowed_tools && !allowed_tools->count(tool.name)) continue;
        json def;
        def["name"] = tool.name;
        if (tool.description) def["description"] = *tool.description;
        else                  def["description"] = nullptr;
        def["input_schema"] = tool.input_schema;
        result.push_back(std::move(def));
    }

    // Plugin tools
    for (auto& tool : plugin_tools_) {
        const auto& d = tool.definition();
        if (allowed_tools && !allowed_tools->count(d.name)) continue;
        json def;
        def["name"] = d.name;
        if (d.description) def["description"] = *d.description;
        else               def["description"] = nullptr;
        def["input_schema"] = d.input_schema;
        result.push_back(std::move(def));
    }

    return result;
}

tl::expected<std::vector<std::pair<std::string, PermissionMode>>, std::string>
GlobalToolRegistry::permission_specs(const std::set<std::string>* allowed_tools) const {
    std::vector<std::pair<std::string, PermissionMode>> result;

    // Built-in tools
    for (auto& spec : mvp_tool_specs()) {
        if (allowed_tools && !allowed_tools->count(spec.name)) continue;
        result.emplace_back(std::string(spec.name), spec.required_permission);
    }

    // Runtime tools
    for (auto& tool : runtime_tools_) {
        if (allowed_tools && !allowed_tools->count(tool.name)) continue;
        result.emplace_back(tool.name, tool.required_permission);
    }

    // Plugin tools — permission string must be mapped
    for (auto& tool : plugin_tools_) {
        const auto& d = tool.definition();
        if (allowed_tools && !allowed_tools->count(d.name)) continue;
        auto pm = permission_mode_from_plugin(tool.required_permission());
        if (!pm) return tl::unexpected(pm.error());
        result.emplace_back(d.name, *pm);
    }

    return result;
}

bool GlobalToolRegistry::has_runtime_tool(std::string_view name) const {
    return std::any_of(runtime_tools_.begin(), runtime_tools_.end(),
        [&](const RuntimeToolDefinition& t) { return t.name == name; });
}

ToolSearchOutput GlobalToolRegistry::search(
    const std::string& query,
    std::size_t max_results,
    std::optional<std::vector<std::string>> pending_mcp_servers) const
{
    auto trimmed = query;
    auto s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) trimmed.clear();
    else {
        auto e = trimmed.find_last_not_of(" \t\r\n");
        trimmed = trimmed.substr(s, e - s + 1);
    }

    auto normalized_query = normalize_tool_search_query(trimmed);

    // Build searchable spec list: deferred builtins + runtime + plugin tools
    std::vector<SearchableToolSpec> searchable;
    for (auto& spec : deferred_tool_specs())
        searchable.push_back({std::string(spec.name), std::string(spec.description)});
    for (auto& tool : runtime_tools_)
        searchable.push_back({tool.name, tool.description.value_or("")});
    for (auto& tool : plugin_tools_) {
        const auto& d = tool.definition();
        searchable.push_back({d.name, d.description.value_or("")});
    }

    std::size_t total = searchable.size();
    auto matches = search_tool_specs(trimmed, std::max(max_results, std::size_t{1}), searchable);

    return ToolSearchOutput{
        std::move(matches),
        trimmed,
        std::move(normalized_query),
        total,
        std::move(pending_mcp_servers),
    };
}

tl::expected<std::string, std::string>
GlobalToolRegistry::execute(const std::string& name, const json& input) const {
    // Check if it's a built-in
    auto specs = mvp_tool_specs();
    bool is_builtin = std::any_of(specs.begin(), specs.end(),
        [&](const ToolSpec& spec) { return spec.name == name; });
    if (is_builtin) {
        const runtime::PermissionEnforcer* ep =
            enforcer_ ? &*enforcer_ : nullptr;
        return execute_tool_with_enforcer(ep, name, input);
    }

    // Look for a plugin tool
    auto it = std::find_if(plugin_tools_.begin(), plugin_tools_.end(),
        [&](const plugins::PluginTool& t) { return t.definition().name == name; });
    if (it == plugin_tools_.end())
        return tl::unexpected("unsupported tool: " + name);

    auto r = it->execute(input);
    if (!r) return tl::unexpected(r.error().what());
    return *r;
}

}  // namespace claw::tools
