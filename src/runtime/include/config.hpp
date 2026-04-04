#pragma once
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <map>
#include <filesystem>
#include <tl/expected.hpp>
#include <nlohmann/json.hpp>

namespace claw::runtime {

enum class ConfigSource {
    DefaultBuiltin,
    ProjectFile,     // .claw/config.json in repo
    UserFile,        // ~/.claw/config.json
    SystemFile,      // /etc/claw/config.json
    EnvVar,
    CliArg,
};

// MCP server config variants
struct McpStdioServerConfig {
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::optional<uint32_t> tool_call_timeout_ms;
};

struct McpSseServerConfig {
    std::string url;
    std::map<std::string, std::string> headers;
};

struct McpHttpServerConfig {
    std::string url;
    std::map<std::string, std::string> headers;
    std::optional<std::string> oauth_client_id;
};

using McpServerConfig = std::variant<McpStdioServerConfig, McpSseServerConfig, McpHttpServerConfig>;

struct McpServerEntry {
    std::string name;
    McpServerConfig config;
};

struct RuntimeFeatureConfig {
    bool enable_caching{true};
    bool enable_compaction{true};
    bool enable_hooks{true};
    bool enable_sandbox{false};
    bool enable_lsp{false};
    bool enable_web_search{false};
    std::optional<std::size_t> max_output_tokens;
    std::optional<std::string> model_override;
};

struct RuntimeConfig {
    std::string model{"claude-sonnet-4-5-20251022"};
    std::optional<std::string> api_key;
    std::optional<std::string> base_url;
    std::vector<McpServerEntry> mcp_servers;
    RuntimeFeatureConfig features;
    std::vector<std::string> allowed_tools;
    std::vector<std::string> denied_tools;
    std::optional<std::string> system_prompt_extra;
    ConfigSource primary_source{ConfigSource::DefaultBuiltin};
};

class ConfigLoader {
public:
    ConfigLoader() = default;

    // Discover and merge all config files (project, user, system, env)
    [[nodiscard]] tl::expected<RuntimeConfig, std::string> load(const std::filesystem::path& cwd);

    // Load a single JSON config file
    [[nodiscard]] static tl::expected<RuntimeConfig, std::string> load_file(const std::filesystem::path& path);

private:
    // Merge src into dst (src values take precedence for scalar fields)
    static void merge(RuntimeConfig& dst, const RuntimeConfig& src);
};

} // namespace claw::runtime
