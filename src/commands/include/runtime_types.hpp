#pragma once
// Minimal forward declarations / stub types mirroring the Rust `runtime` crate.
// A real integration would include headers from the converted runtime library.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace runtime {

// ---- MessageRole & ContentBlock ---------------------------------------------

enum class MessageRole { System, User, Assistant, Tool };

struct TextBlock {
    std::string text;
};

struct ToolUseBlock {
    std::string id;
    std::string name;
    std::string input;
};

struct ToolResultBlock {
    std::string tool_use_id;
    std::string tool_name;
    std::string output;
    bool        is_error{false};
};

using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock>;

// ---- TokenUsage -------------------------------------------------------------

struct TokenUsage {
    std::uint64_t input_tokens{0};
    std::uint64_t output_tokens{0};
    std::uint64_t cache_read_input_tokens{0};
    std::uint64_t cache_creation_input_tokens{0};
};

// ---- ConversationMessage ----------------------------------------------------

struct ConversationMessage {
    MessageRole                  role;
    std::vector<ContentBlock>    blocks;
    std::optional<TokenUsage>    usage;

    static ConversationMessage user_text(std::string text) {
        return {MessageRole::User, {TextBlock{std::move(text)}}, std::nullopt};
    }
    static ConversationMessage assistant(std::vector<ContentBlock> blocks) {
        return {MessageRole::Assistant, std::move(blocks), std::nullopt};
    }
    static ConversationMessage tool_result(std::string_view id, std::string_view name,
                                           std::string out, bool is_error) {
        ToolResultBlock blk;
        blk.tool_use_id = std::string(id);
        blk.tool_name   = std::string(name);
        blk.output      = std::move(out);
        blk.is_error    = is_error;
        return {MessageRole::Tool, {blk}, std::nullopt};
    }
};

// ---- Session ----------------------------------------------------------------

struct SessionCompaction {
    std::uint32_t count{0};
    std::size_t   removed_message_count{0};
    std::string   summary;
};

struct SessionFork {
    std::string              parent_session_id;
    std::optional<std::string> branch_name;
};

struct Session {
    std::uint32_t                      version{1};
    std::string                        session_id;
    std::uint64_t                      created_at_ms{0};
    std::uint64_t                      updated_at_ms{0};
    std::vector<ConversationMessage>   messages;
    std::optional<SessionCompaction>   compaction;
    std::optional<SessionFork>         fork;

    static Session new_session();
    bool operator==(const Session&) const = default;
};

// ---- CompactionConfig & CompactionResult ------------------------------------

struct CompactionConfig {
    std::size_t preserve_recent_messages{4};
    std::size_t max_estimated_tokens{10'000};
};

struct CompactionResult {
    std::string   summary;
    std::string   formatted_summary;
    Session       compacted_session;
    std::size_t   removed_message_count{0};
};

[[nodiscard]] CompactionResult compact_session(const Session& session, CompactionConfig config);

// ---- ConfigSource -----------------------------------------------------------

enum class ConfigSource { User, Project, Local };

// ---- McpServerConfig variants -----------------------------------------------

struct McpStdioServerConfig {
    std::string                          command;
    std::vector<std::string>             args;
    std::map<std::string, std::string>   env;
    std::optional<std::uint64_t>         tool_call_timeout_ms;
};

struct McpRemoteServerConfig {
    std::string                          url;
    std::map<std::string, std::string>   headers;
    std::optional<std::string>           headers_helper;

    struct OAuthConfig {
        std::optional<std::string>       client_id;
        std::optional<std::uint16_t>     callback_port;
        std::optional<std::string>       auth_server_metadata_url;
        std::optional<bool>              xaa;
    };
    std::optional<OAuthConfig>           oauth;
};

struct McpWebSocketServerConfig {
    std::string                          url;
    std::map<std::string, std::string>   headers;
    std::optional<std::string>           headers_helper;
};

struct McpSdkServerConfig {
    std::string name;
};

struct McpManagedProxyServerConfig {
    std::string url;
    std::string id;
};

using McpServerConfig = std::variant<
    McpStdioServerConfig,
    McpRemoteServerConfig,   // Sse
    McpRemoteServerConfig,   // Http  (duplicated variant not allowed in std::variant;
                              // use a tag instead — see ScopedMcpServerConfig below)
    McpWebSocketServerConfig,
    McpSdkServerConfig,
    McpManagedProxyServerConfig>;

// Because std::variant cannot hold two identical types, we use a tagged union.
enum class McpTransportKind { Stdio, Sse, Http, Ws, Sdk, ManagedProxy };

struct TaggedMcpServerConfig {
    McpTransportKind kind;
    // payload — only one is active according to `kind`
    std::optional<McpStdioServerConfig>         stdio;
    std::optional<McpRemoteServerConfig>        remote;    // used for Sse and Http
    std::optional<McpWebSocketServerConfig>     ws;
    std::optional<McpSdkServerConfig>           sdk;
    std::optional<McpManagedProxyServerConfig>  managed_proxy;

    // Convenience constructors
    static TaggedMcpServerConfig make_stdio(McpStdioServerConfig c) {
        TaggedMcpServerConfig t; t.kind = McpTransportKind::Stdio; t.stdio = std::move(c); return t;
    }
    static TaggedMcpServerConfig make_sse(McpRemoteServerConfig c) {
        TaggedMcpServerConfig t; t.kind = McpTransportKind::Sse; t.remote = std::move(c); return t;
    }
    static TaggedMcpServerConfig make_http(McpRemoteServerConfig c) {
        TaggedMcpServerConfig t; t.kind = McpTransportKind::Http; t.remote = std::move(c); return t;
    }
    static TaggedMcpServerConfig make_ws(McpWebSocketServerConfig c) {
        TaggedMcpServerConfig t; t.kind = McpTransportKind::Ws; t.ws = std::move(c); return t;
    }
    static TaggedMcpServerConfig make_sdk(McpSdkServerConfig c) {
        TaggedMcpServerConfig t; t.kind = McpTransportKind::Sdk; t.sdk = std::move(c); return t;
    }
    static TaggedMcpServerConfig make_managed_proxy(McpManagedProxyServerConfig c) {
        TaggedMcpServerConfig t; t.kind = McpTransportKind::ManagedProxy; t.managed_proxy = std::move(c); return t;
    }
};

struct ScopedMcpServerConfig {
    ConfigSource         scope;
    TaggedMcpServerConfig config;
};

using McpOAuthConfig = McpRemoteServerConfig::OAuthConfig;

// ---- ConfigError ------------------------------------------------------------

class ConfigError : public std::exception {
public:
    enum class Kind { Io, Parse };
    explicit ConfigError(Kind k, std::string msg) : kind_(k), message_(std::move(msg)) {}
    [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }
    [[nodiscard]] Kind         kind() const noexcept          { return kind_; }

    static ConfigError io(std::string msg)    { return ConfigError{Kind::Io,    std::move(msg)}; }
    static ConfigError parse(std::string msg) { return ConfigError{Kind::Parse, std::move(msg)}; }

private:
    Kind        kind_;
    std::string message_;
};

// ---- McpConfigCollection & RuntimeConfig ------------------------------------

class McpConfigCollection {
public:
    [[nodiscard]] const std::map<std::string, ScopedMcpServerConfig>& servers() const { return servers_; }
    [[nodiscard]] const ScopedMcpServerConfig* get(std::string_view name) const {
        auto it = servers_.find(std::string(name));
        return it == servers_.end() ? nullptr : &it->second;
    }
    void add(std::string name, ScopedMcpServerConfig cfg) {
        servers_.emplace(std::move(name), std::move(cfg));
    }
private:
    std::map<std::string, ScopedMcpServerConfig> servers_;
};

class RuntimeConfig {
public:
    [[nodiscard]] const McpConfigCollection& mcp() const { return mcp_; }
private:
    McpConfigCollection mcp_;
};

// ---- ConfigLoader -----------------------------------------------------------

class ConfigLoader {
public:
    ConfigLoader(std::filesystem::path cwd, std::filesystem::path config_home)
        : cwd_(std::move(cwd)), config_home_(std::move(config_home)) {}

    [[nodiscard]] static ConfigLoader default_for(std::filesystem::path cwd);

    [[nodiscard]] RuntimeConfig load() const;

private:
    std::filesystem::path cwd_;
    std::filesystem::path config_home_;
};

} // namespace runtime
