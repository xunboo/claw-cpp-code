#pragma once

#include <tl/expected.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "plugin_error.hpp"
#include "plugin_types.hpp"

namespace claw::plugins {

// Corresponds to Rust PluginTool
class PluginTool {
public:
    PluginTool(std::string plugin_id,
               std::string plugin_name,
               PluginToolDefinition definition,
               std::string command,
               std::vector<std::string> args,
               PluginToolPermission required_permission,
               std::optional<std::filesystem::path> root);

    [[nodiscard]] const std::string& plugin_id() const noexcept { return plugin_id_; }
    [[nodiscard]] const PluginToolDefinition& definition() const noexcept { return definition_; }
    [[nodiscard]] const char* required_permission() const noexcept {
        return plugin_tool_permission_str(required_permission_);
    }
    [[nodiscard]] PluginToolPermission required_permission_enum() const noexcept {
        return required_permission_;
    }

    // Execute the tool command, piping input_json on stdin.
    // Returns stdout on success, PluginError on failure.
    [[nodiscard]] tl::expected<std::string, PluginError> execute(const nlohmann::json& input) const;

    // Public data needed by registry / serialisation
    std::string command;
    std::vector<std::string> args;
    std::optional<std::filesystem::path> root;

private:
    std::string plugin_id_;
    std::string plugin_name_;
    PluginToolDefinition definition_;
    PluginToolPermission required_permission_{PluginToolPermission::DangerFullAccess};
};

}  // namespace claw::plugins
