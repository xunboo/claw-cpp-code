#include "mcp_client.hpp"
#include <charconv>
#include <cstring>

namespace claw::runtime {

McpClientAuth McpClientAuth::from_oauth(std::string client_id, OAuthTokenSet tokens) {
    return McpClientAuth{
        .oauth_client_id = std::move(client_id),
        .token_set = std::move(tokens),
    };
}

std::optional<McpClientBootstrap> McpClientBootstrap::from_scoped_config(const McpServerEntry& entry) {
    McpClientBootstrap bootstrap;
    bootstrap.server_name = entry.name;

    if (const auto* stdio = std::get_if<McpStdioServerConfig>(&entry.config)) {
        bootstrap.transport = McpTransportKind::Stdio;
        McpServerManager::ServerConfig cfg;
        cfg.name = entry.name;
        cfg.command = stdio->command;
        cfg.args = stdio->args;
        for (const auto& [k, v] : stdio->env) cfg.env[k] = v;
        if (stdio->tool_call_timeout_ms) {
            cfg.tool_call_timeout_ms = *stdio->tool_call_timeout_ms;
            bootstrap.tool_call_timeout_ms = *stdio->tool_call_timeout_ms;
        }
        bootstrap.stdio_config = std::move(cfg);
        return bootstrap;
    }

    if (const auto* sse = std::get_if<McpSseServerConfig>(&entry.config)) {
        bootstrap.transport = McpTransportKind::Sse;
        bootstrap.url = sse->url;
        // SSE servers are not directly supported (go to unsupported list)
        return std::nullopt;
    }

    if (const auto* http = std::get_if<McpHttpServerConfig>(&entry.config)) {
        bootstrap.transport = McpTransportKind::Http;
        bootstrap.url = http->url;
        if (http->oauth_client_id.has_value()) {
            bootstrap.auth = McpClientAuth{http->oauth_client_id, std::nullopt};
        }
        // HTTP servers are not directly supported (go to unsupported list)
        return std::nullopt;
    }

    return std::nullopt;
}

uint32_t McpClientBootstrap::resolved_tool_call_timeout_ms() const {
    const char* env = std::getenv("MCP_TOOL_CALL_TIMEOUT_MS");
    if (env && *env) {
        uint32_t val{};
        auto [ptr, ec] = std::from_chars(env, env + std::strlen(env), val);
        if (ec == std::errc{} && val > 0) return val;
    }
    return tool_call_timeout_ms;
}

} // namespace claw::runtime
