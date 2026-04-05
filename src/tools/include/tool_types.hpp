#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations from other crates
namespace claw::runtime {
enum class PermissionMode;
struct McpDegradedReport;
void to_json(nlohmann::json& j, const McpDegradedReport& r);
}

namespace claw::tools {

using nlohmann::json;
using PermissionMode = claw::runtime::PermissionMode;

// ── ToolSource / ToolManifestEntry ───────────────────────────────────────────

enum class ToolSource { Base, Conditional };

struct ToolManifestEntry {
    std::string name;
    ToolSource  source{ToolSource::Base};
};

// ── ToolSpec ─────────────────────────────────────────────────────────────────

struct ToolSpec {
    const char*  name;
    const char*  description;
    json         input_schema;
    PermissionMode required_permission;
};

// ── RuntimeToolDefinition ────────────────────────────────────────────────────

struct RuntimeToolDefinition {
    std::string              name;
    std::optional<std::string> description;
    json                     input_schema;
    PermissionMode           required_permission;
};

// ── ToolSearchOutput ─────────────────────────────────────────────────────────

struct ToolSearchOutput {
    std::vector<std::string>        matches;
    std::string                     query;
    std::string                     normalized_query;
    std::size_t                     total_deferred_tools{0};
    std::optional<std::vector<std::string>> pending_mcp_servers;
    std::optional<nlohmann::json>   mcp_degraded; // serialized McpDegradedReport
};

inline void to_json(json& j, const ToolSearchOutput& o) {
    j = json{
        {"matches", o.matches},
        {"query", o.query},
        {"normalized_query", o.normalized_query},
        {"total_deferred_tools", o.total_deferred_tools},
    };
    if (o.pending_mcp_servers)
        j["pending_mcp_servers"] = *o.pending_mcp_servers;
    else
        j["pending_mcp_servers"] = nullptr;
    if (o.mcp_degraded)
        j["mcp_degraded"] = *o.mcp_degraded;
}

}  // namespace claw::tools
