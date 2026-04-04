#pragma once
#include "mcp_stdio.hpp"
#include "oauth.hpp"
#include "config.hpp"
#include <string>
#include <optional>
#include <variant>
#include <memory>

namespace claw::runtime {

enum class McpTransportKind { Stdio, Sse, Http };

struct McpClientAuth {
    std::optional<std::string> oauth_client_id;
    std::optional<OAuthTokenSet> token_set;

    [[nodiscard]] static McpClientAuth from_oauth(std::string client_id, OAuthTokenSet tokens);
};

struct McpClientBootstrap {
    std::string server_name;
    McpTransportKind transport;
    std::optional<McpServerManager::ServerConfig> stdio_config;
    std::optional<std::string> url; // for SSE/HTTP
    std::optional<McpClientAuth> auth;
    uint32_t tool_call_timeout_ms{60000};

    // Build a bootstrap from scoped server config
    [[nodiscard]] static std::optional<McpClientBootstrap>
        from_scoped_config(const McpServerEntry& entry);

    // Resolved timeout (env override: MCP_TOOL_CALL_TIMEOUT_MS)
    [[nodiscard]] uint32_t resolved_tool_call_timeout_ms() const;
};

} // namespace claw::runtime
