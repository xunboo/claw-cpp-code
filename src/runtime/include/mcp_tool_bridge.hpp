#pragma once
#include "mcp_stdio.hpp"
#include "plugin_lifecycle.hpp"
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <optional>
#include <tl/expected.hpp>

namespace claw::runtime {

enum class McpConnectionStatus {
    Disconnected,
    Connecting,
    Connected,
    AuthRequired,
    Error,
};

struct McpToolInfo {
    std::string name;
    std::optional<std::string> description;
    std::optional<nlohmann::json> input_schema;
};

struct McpResourceInfo {
    std::string uri;
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
};

// Type aliases for plugin_lifecycle compatibility (defined after the structs they alias)
using ToolInfo     = McpToolInfo;
using ResourceInfo = McpResourceInfo;

struct McpServerState {
    std::string server_name;
    McpConnectionStatus status{McpConnectionStatus::Disconnected};
    std::vector<McpToolInfo> tools;
    std::vector<McpResourceInfo> resources;
    std::optional<std::string> last_error;
};

class McpToolRegistry {
public:
    McpToolRegistry() = default;

    void register_server(std::string name, McpServerState state);
    [[nodiscard]] std::optional<McpServerState> get_server(std::string_view name) const;
    [[nodiscard]] std::vector<McpServerState> list_servers() const;

    [[nodiscard]] tl::expected<std::vector<McpResourceInfo>, std::string>
        list_resources(std::string_view server_name) const;
    [[nodiscard]] tl::expected<McpReadResourceResult, std::string>
        read_resource(std::string_view server_name, std::string_view uri);

    [[nodiscard]] std::vector<McpToolInfo> list_tools(std::optional<std::string_view> server_name = std::nullopt) const;

    // Call a tool by its qualified name; spawns a dedicated thread for async-like behavior
    [[nodiscard]] tl::expected<McpToolCallResult, std::string>
        call_tool(std::string_view qualified_name, const nlohmann::json& arguments);

    void set_auth_status(std::string_view server_name, McpConnectionStatus status);
    void disconnect(std::string_view server_name);

    [[nodiscard]] std::size_t len() const;
    [[nodiscard]] bool is_empty() const;

    // Bind a server manager (set once)
    void set_manager(std::shared_ptr<McpServerManager> manager);

private:
    struct Inner {
        std::unordered_map<std::string, McpServerState> servers;
    };
    mutable std::mutex mutex_;
    Inner inner_;

    // Manager is set once after construction
    std::shared_ptr<McpServerManager> manager_;
    mutable std::mutex manager_mutex_;
};

} // namespace claw::runtime
