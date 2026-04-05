#pragma once
#include <tl/expected.hpp>

#include "tool_types.hpp"

#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace claw::runtime {
class PermissionEnforcer;
}

namespace claw::tools {

/// Core dispatch: execute a built-in tool by name with the given JSON input.
/// Returns the result string or an error string.
[[nodiscard]] tl::expected<std::string, std::string>
    execute_tool(const std::string& name, const json& input);

/// Same but with an optional permission enforcer applied first.
[[nodiscard]] tl::expected<std::string, std::string>
    execute_tool_with_enforcer(
        const claw::runtime::PermissionEnforcer* enforcer,
        const std::string& name,
        const json& input);

/// Check permission via enforcer; returns Err(reason) if denied.
[[nodiscard]] tl::expected<void, std::string>
    enforce_permission_check(
        const claw::runtime::PermissionEnforcer& enforcer,
        const std::string& tool_name,
        const json& input);

/// Map a plugin tool permission string to PermissionMode.
[[nodiscard]] tl::expected<PermissionMode, std::string>
    permission_mode_from_plugin(std::string_view value);

}  // namespace claw::tools
