// Full C++20 faithful conversion of config.rs
// Rust source: crates/runtime/src/config.rs (1532 lines)
//
// Key mapping conventions:
//   Result<T, E>        -> tl::expected<T, E>
//   Option<T>           -> std::optional<T>
//   Vec<T>              -> std::vector<T>
//   BTreeMap<K,V>       -> std::map<K,V>        (ordered, matches BTreeMap iteration order)
//   HashMap<K,V>        -> std::unordered_map<K,V>
//   Arc<T>              -> std::shared_ptr<T>
//   serde_json          -> nlohmann::json (but JSON parsing here uses a custom JsonValue
//                          defined in json_value.hpp, matching json.rs hand-written parser)
//
// The existing config.hpp has a simplified stub structure that differs from the Rust source.
// Per task instructions, all missing types are defined inline below with a comment.
// The implementation lives in namespace claw::runtime (matches the existing header).

#include "../include/config.hpp"

// ---- additional standard headers needed for this translation unit ----
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>        // std::getenv
#include <tl/expected.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// nlohmann/json is available in the project (used by the existing header)
#include <nlohmann/json.hpp>

// ============================================================
// Types NOT present in config.hpp that are needed by the
// faithful C++20 conversion of config.rs.  They are defined
// here (in namespace claw::runtime) with a comment so the linker
// finds them from any TU that includes this .cpp (or a
// corresponding updated header).
// ============================================================

namespace claw::runtime {

// --- ConfigSource -----------------------------------------------------------
// Rust: pub enum ConfigSource { User, Project, Local }
// NOTE: The existing config.hpp defines a different ConfigSource enum.
//       We shadow it inside the anonymous namespace helpers and use our own
//       strong enum below.  The existing header enum is left intact; callers
//       that use the new API include the types defined here.

enum class ClawConfigSource {
    User,
    Project,
    Local,
};

// --- ResolvedPermissionMode --------------------------------------------------
// Rust: pub enum ResolvedPermissionMode { ReadOnly, WorkspaceWrite, DangerFullAccess }
enum class ResolvedPermissionMode {
    ReadOnly,
    WorkspaceWrite,
    DangerFullAccess,
};

// --- FilesystemIsolationMode (faithful to sandbox.rs) -----------------------
// NOTE: sandbox.hpp defines a different FilesystemIsolationMode.
//       We define the Rust-faithful one here.
enum class ClawFilesystemIsolationMode {
    Off,
    WorkspaceOnly,
    AllowList,
};

static const char* filesystem_isolation_mode_str(ClawFilesystemIsolationMode m) {
    switch (m) {
        case ClawFilesystemIsolationMode::Off:           return "off";
        case ClawFilesystemIsolationMode::WorkspaceOnly: return "workspace-only";
        case ClawFilesystemIsolationMode::AllowList:     return "allow-list";
    }
    return "workspace-only";
}

// --- SandboxConfig (faithful to sandbox.rs) ----------------------------------
// NOTE: sandbox.hpp has a different SandboxConfig.  We use ClawSandboxConfig
//       here for the config.rs conversion.
struct ClawSandboxConfig {
    std::optional<bool>                       enabled;
    std::optional<bool>                       namespace_restrictions;
    std::optional<bool>                       network_isolation;
    std::optional<ClawFilesystemIsolationMode> filesystem_mode;
    std::vector<std::string>                  allowed_mounts;

    bool operator==(const ClawSandboxConfig&) const = default;
};

// --- JsonValue helpers -------------------------------------------------------
// The json_value.hpp JsonValue uses unordered_map; we need ordered map for
// faithful BTreeMap semantics.  We re-use nlohmann::json as our JsonValue
// here, which supports ordered iteration via nlohmann::ordered_json if needed,
// but for simplicity we use nlohmann::json with std::map order maintained
// through sorted iteration when needed.
//
// Helper aliases:
using NlJson = nlohmann::json;

// --- ConfigError ------------------------------------------------------------
// Rust: pub enum ConfigError { Io(std::io::Error), Parse(String) }
struct ConfigError {
    enum class Kind { Io, Parse } kind;
    std::string message;

    static ConfigError io(std::string msg) {
        return ConfigError{ Kind::Io, std::move(msg) };
    }
    static ConfigError parse(std::string msg) {
        return ConfigError{ Kind::Parse, std::move(msg) };
    }

    [[nodiscard]] std::string to_string() const { return message; }

    bool operator==(const ConfigError&) const = default;
};

// --- ConfigEntry ------------------------------------------------------------
// Rust: pub struct ConfigEntry { pub source: ConfigSource, pub path: PathBuf }
struct ConfigEntry {
    ClawConfigSource        source;
    std::filesystem::path   path;

    bool operator==(const ConfigEntry&) const = default;
};

// --- McpTransport -----------------------------------------------------------
// Rust: pub enum McpTransport { Stdio, Sse, Http, Ws, Sdk, ManagedProxy }
enum class McpTransport {
    Stdio,
    Sse,
    Http,
    Ws,
    Sdk,
    ManagedProxy,
};

// --- MCP OAuth config --------------------------------------------------------
// Rust: pub struct McpOAuthConfig { ... }
struct McpOAuthConfig {
    std::optional<std::string> client_id;
    std::optional<uint16_t>    callback_port;
    std::optional<std::string> auth_server_metadata_url;
    std::optional<bool>        xaa;

    bool operator==(const McpOAuthConfig&) const = default;
};

// --- Individual MCP server configs ------------------------------------------
// Rust: pub struct McpStdioServerConfig { ... }
struct ClawMcpStdioServerConfig {
    std::string                        command;
    std::vector<std::string>           args;
    std::map<std::string, std::string> env;
    std::optional<uint64_t>            tool_call_timeout_ms;

    bool operator==(const ClawMcpStdioServerConfig&) const = default;
};

// Rust: pub struct McpRemoteServerConfig { ... }
struct McpRemoteServerConfig {
    std::string                        url;
    std::map<std::string, std::string> headers;
    std::optional<std::string>         headers_helper;
    std::optional<McpOAuthConfig>      oauth;

    bool operator==(const McpRemoteServerConfig&) const = default;
};

// Rust: pub struct McpWebSocketServerConfig { ... }
struct McpWebSocketServerConfig {
    std::string                        url;
    std::map<std::string, std::string> headers;
    std::optional<std::string>         headers_helper;

    bool operator==(const McpWebSocketServerConfig&) const = default;
};

// Rust: pub struct McpSdkServerConfig { ... }
struct McpSdkServerConfig {
    std::string name;

    bool operator==(const McpSdkServerConfig&) const = default;
};

// Rust: pub struct McpManagedProxyServerConfig { ... }
struct McpManagedProxyServerConfig {
    std::string url;
    std::string id;

    bool operator==(const McpManagedProxyServerConfig&) const = default;
};

// Rust: pub enum McpServerConfig { Stdio(...), Sse(...), Http(...), Ws(...), Sdk(...), ManagedProxy(...) }
struct ClawMcpServerConfig {
    // Use a tagged union via std::variant
    using Payload = std::variant<
        ClawMcpStdioServerConfig,
        McpRemoteServerConfig,    // Sse
        McpRemoteServerConfig,    // Http  -- NOTE: same struct, tag distinguishes
        McpWebSocketServerConfig,
        McpSdkServerConfig,
        McpManagedProxyServerConfig
    >;
    McpTransport transport_kind;
    // Store as nlohmann::json internally to avoid complex variant-of-same-type;
    // we expose typed accessors below.

    // Concrete typed storage (one of these is active, indicated by transport_kind)
    std::optional<ClawMcpStdioServerConfig>    stdio;
    std::optional<McpRemoteServerConfig>       remote; // used for both Sse and Http
    std::optional<McpWebSocketServerConfig>    ws;
    std::optional<McpSdkServerConfig>          sdk;
    std::optional<McpManagedProxyServerConfig> managed_proxy;

    [[nodiscard]] McpTransport transport() const noexcept { return transport_kind; }

    static ClawMcpServerConfig make_stdio(ClawMcpStdioServerConfig c) {
        ClawMcpServerConfig s;
        s.transport_kind = McpTransport::Stdio;
        s.stdio = std::move(c);
        return s;
    }
    static ClawMcpServerConfig make_sse(McpRemoteServerConfig c) {
        ClawMcpServerConfig s;
        s.transport_kind = McpTransport::Sse;
        s.remote = std::move(c);
        return s;
    }
    static ClawMcpServerConfig make_http(McpRemoteServerConfig c) {
        ClawMcpServerConfig s;
        s.transport_kind = McpTransport::Http;
        s.remote = std::move(c);
        return s;
    }
    static ClawMcpServerConfig make_ws(McpWebSocketServerConfig c) {
        ClawMcpServerConfig s;
        s.transport_kind = McpTransport::Ws;
        s.ws = std::move(c);
        return s;
    }
    static ClawMcpServerConfig make_sdk(McpSdkServerConfig c) {
        ClawMcpServerConfig s;
        s.transport_kind = McpTransport::Sdk;
        s.sdk = std::move(c);
        return s;
    }
    static ClawMcpServerConfig make_managed_proxy(McpManagedProxyServerConfig c) {
        ClawMcpServerConfig s;
        s.transport_kind = McpTransport::ManagedProxy;
        s.managed_proxy = std::move(c);
        return s;
    }

    bool operator==(const ClawMcpServerConfig&) const = default;
};

// Rust: pub struct ScopedMcpServerConfig { pub scope: ConfigSource, pub config: McpServerConfig }
struct ScopedMcpServerConfig {
    ClawConfigSource   scope;
    ClawMcpServerConfig config;

    [[nodiscard]] McpTransport transport() const noexcept { return config.transport(); }

    bool operator==(const ScopedMcpServerConfig&) const = default;
};

// Rust: pub struct McpConfigCollection { servers: BTreeMap<String, ScopedMcpServerConfig> }
struct McpConfigCollection {
    std::map<std::string, ScopedMcpServerConfig> servers;

    [[nodiscard]] const std::map<std::string, ScopedMcpServerConfig>& get_servers() const noexcept {
        return servers;
    }
    [[nodiscard]] const ScopedMcpServerConfig* get(const std::string& name) const noexcept {
        auto it = servers.find(name);
        return (it != servers.end()) ? &it->second : nullptr;
    }

    bool operator==(const McpConfigCollection&) const = default;
};

// --- OAuthConfig ------------------------------------------------------------
// Rust: pub struct OAuthConfig { ... }
struct OAuthConfig {
    std::string                client_id;
    std::string                authorize_url;
    std::string                token_url;
    std::optional<uint16_t>    callback_port;
    std::optional<std::string> manual_redirect_url;
    std::vector<std::string>   scopes;

    bool operator==(const OAuthConfig&) const = default;
};

// --- RuntimeHookConfig ------------------------------------------------------
// Rust: pub struct RuntimeHookConfig { pre_tool_use, post_tool_use, post_tool_use_failure }
struct RuntimeHookConfig {
    std::vector<std::string> pre_tool_use;
    std::vector<std::string> post_tool_use;
    std::vector<std::string> post_tool_use_failure;

    RuntimeHookConfig() = default;
    RuntimeHookConfig(
        std::vector<std::string> pre,
        std::vector<std::string> post,
        std::vector<std::string> post_failure)
        : pre_tool_use(std::move(pre))
        , post_tool_use(std::move(post))
        , post_tool_use_failure(std::move(post_failure))
    {}

    [[nodiscard]] const std::vector<std::string>& get_pre_tool_use() const noexcept {
        return pre_tool_use;
    }
    [[nodiscard]] const std::vector<std::string>& get_post_tool_use() const noexcept {
        return post_tool_use;
    }
    [[nodiscard]] const std::vector<std::string>& get_post_tool_use_failure() const noexcept {
        return post_tool_use_failure;
    }

    // Rust: pub fn merged(&self, other: &Self) -> Self
    [[nodiscard]] RuntimeHookConfig merged(const RuntimeHookConfig& other) const {
        RuntimeHookConfig result = *this;
        result.extend(other);
        return result;
    }

    // Rust: pub fn extend(&mut self, other: &Self)
    void extend(const RuntimeHookConfig& other) {
        extend_unique_into(pre_tool_use,          other.pre_tool_use);
        extend_unique_into(post_tool_use,         other.post_tool_use);
        extend_unique_into(post_tool_use_failure, other.post_tool_use_failure);
    }

    bool operator==(const RuntimeHookConfig&) const = default;

private:
    static void push_unique(std::vector<std::string>& target, const std::string& value) {
        if (std::find(target.begin(), target.end(), value) == target.end()) {
            target.push_back(value);
        }
    }
    static void extend_unique_into(std::vector<std::string>& target,
                                    const std::vector<std::string>& values) {
        for (const auto& v : values) {
            push_unique(target, v);
        }
    }
};

// --- RuntimePermissionRuleConfig --------------------------------------------
// Rust: pub struct RuntimePermissionRuleConfig { allow, deny, ask }
struct RuntimePermissionRuleConfig {
    std::vector<std::string> allow;
    std::vector<std::string> deny;
    std::vector<std::string> ask;

    RuntimePermissionRuleConfig() = default;
    RuntimePermissionRuleConfig(
        std::vector<std::string> allow_,
        std::vector<std::string> deny_,
        std::vector<std::string> ask_)
        : allow(std::move(allow_))
        , deny(std::move(deny_))
        , ask(std::move(ask_))
    {}

    [[nodiscard]] const std::vector<std::string>& get_allow() const noexcept { return allow; }
    [[nodiscard]] const std::vector<std::string>& get_deny()  const noexcept { return deny; }
    [[nodiscard]] const std::vector<std::string>& get_ask()   const noexcept { return ask; }

    bool operator==(const RuntimePermissionRuleConfig&) const = default;
};

// --- RuntimePluginConfig ----------------------------------------------------
// Rust: pub struct RuntimePluginConfig { enabled_plugins, external_directories,
//                                        install_root, registry_path, bundled_root }
struct RuntimePluginConfig {
    std::map<std::string, bool>  enabled_plugins;
    std::vector<std::string>     external_directories;
    std::optional<std::string>   install_root;
    std::optional<std::string>   registry_path;
    std::optional<std::string>   bundled_root;

    [[nodiscard]] const std::map<std::string, bool>& get_enabled_plugins() const noexcept {
        return enabled_plugins;
    }
    [[nodiscard]] const std::vector<std::string>& get_external_directories() const noexcept {
        return external_directories;
    }
    [[nodiscard]] std::optional<std::string_view> get_install_root() const noexcept {
        if (install_root) return std::string_view(*install_root);
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string_view> get_registry_path() const noexcept {
        if (registry_path) return std::string_view(*registry_path);
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string_view> get_bundled_root() const noexcept {
        if (bundled_root) return std::string_view(*bundled_root);
        return std::nullopt;
    }

    // Rust: pub fn set_plugin_state(&mut self, plugin_id: String, enabled: bool)
    void set_plugin_state(std::string plugin_id, bool enabled) {
        enabled_plugins[std::move(plugin_id)] = enabled;
    }

    // Rust: pub fn state_for(&self, plugin_id: &str, default_enabled: bool) -> bool
    [[nodiscard]] bool state_for(const std::string& plugin_id, bool default_enabled) const {
        auto it = enabled_plugins.find(plugin_id);
        if (it != enabled_plugins.end()) return it->second;
        return default_enabled;
    }

    bool operator==(const RuntimePluginConfig&) const = default;
};

// --- RuntimeFeatureConfig ---------------------------------------------------
// Rust: pub struct RuntimeFeatureConfig { hooks, plugins, mcp, oauth, model,
//                                          permission_mode, permission_rules, sandbox }
struct ClawRuntimeFeatureConfig {
    RuntimeHookConfig               hooks;
    RuntimePluginConfig             plugins;
    McpConfigCollection             mcp;
    std::optional<OAuthConfig>      oauth;
    std::optional<std::string>      model;
    std::optional<ResolvedPermissionMode> permission_mode;
    RuntimePermissionRuleConfig     permission_rules;
    ClawSandboxConfig               sandbox;

    // Rust: pub fn with_hooks(mut self, hooks: RuntimeHookConfig) -> Self
    [[nodiscard]] ClawRuntimeFeatureConfig with_hooks(RuntimeHookConfig h) && {
        hooks = std::move(h);
        return std::move(*this);
    }
    // Rust: pub fn with_plugins(mut self, plugins: RuntimePluginConfig) -> Self
    [[nodiscard]] ClawRuntimeFeatureConfig with_plugins(RuntimePluginConfig p) && {
        plugins = std::move(p);
        return std::move(*this);
    }

    [[nodiscard]] const RuntimeHookConfig&           get_hooks()            const noexcept { return hooks; }
    [[nodiscard]] const RuntimePluginConfig&         get_plugins()          const noexcept { return plugins; }
    [[nodiscard]] const McpConfigCollection&         get_mcp()              const noexcept { return mcp; }
    [[nodiscard]] const std::optional<OAuthConfig>&  get_oauth()            const noexcept { return oauth; }
    [[nodiscard]] std::optional<std::string_view>    get_model()            const noexcept {
        if (model) return std::string_view(*model);
        return std::nullopt;
    }
    [[nodiscard]] std::optional<ResolvedPermissionMode> get_permission_mode() const noexcept {
        return permission_mode;
    }
    [[nodiscard]] const RuntimePermissionRuleConfig& get_permission_rules() const noexcept { return permission_rules; }
    [[nodiscard]] const ClawSandboxConfig&           get_sandbox()          const noexcept { return sandbox; }

    bool operator==(const ClawRuntimeFeatureConfig&) const = default;
};

// --- ClawRuntimeConfig ------------------------------------------------------
// Rust: pub struct RuntimeConfig { merged, loaded_entries, feature_config }
// Note: this mirrors the Rust struct; the header's RuntimeConfig is a different,
// simpler struct left unchanged.
struct ClawRuntimeConfig {
    std::map<std::string, NlJson>  merged;   // BTreeMap<String, JsonValue>
    std::vector<ConfigEntry>        loaded_entries;
    ClawRuntimeFeatureConfig        feature_config;

    // Rust: pub fn empty() -> Self
    [[nodiscard]] static ClawRuntimeConfig empty() {
        return ClawRuntimeConfig{};
    }

    [[nodiscard]] const std::map<std::string, NlJson>& get_merged() const noexcept {
        return merged;
    }
    [[nodiscard]] const std::vector<ConfigEntry>& get_loaded_entries() const noexcept {
        return loaded_entries;
    }
    // Rust: pub fn get(&self, key: &str) -> Option<&JsonValue>
    [[nodiscard]] const NlJson* get(const std::string& key) const {
        auto it = merged.find(key);
        return (it != merged.end()) ? &it->second : nullptr;
    }
    // Rust: pub fn as_json(&self) -> JsonValue
    [[nodiscard]] NlJson as_json() const {
        NlJson obj = NlJson::object();
        for (const auto& [k, v] : merged) obj[k] = v;
        return obj;
    }
    [[nodiscard]] const ClawRuntimeFeatureConfig& get_feature_config() const noexcept {
        return feature_config;
    }
    [[nodiscard]] const McpConfigCollection&      mcp()             const noexcept { return feature_config.mcp; }
    [[nodiscard]] const RuntimeHookConfig&        hooks()           const noexcept { return feature_config.hooks; }
    [[nodiscard]] const RuntimePluginConfig&      plugins()         const noexcept { return feature_config.plugins; }
    [[nodiscard]] const std::optional<OAuthConfig>& oauth()         const noexcept { return feature_config.oauth; }
    [[nodiscard]] std::optional<std::string_view> model()           const noexcept { return feature_config.get_model(); }
    [[nodiscard]] std::optional<ResolvedPermissionMode> permission_mode() const noexcept {
        return feature_config.permission_mode;
    }
    [[nodiscard]] const RuntimePermissionRuleConfig& permission_rules() const noexcept {
        return feature_config.permission_rules;
    }
    [[nodiscard]] const ClawSandboxConfig& sandbox() const noexcept { return feature_config.sandbox; }

    bool operator==(const ClawRuntimeConfig&) const = default;
};

// ============================================================
// CLAW_SETTINGS_SCHEMA_NAME constant
// Rust: pub const CLAW_SETTINGS_SCHEMA_NAME: &str = "SettingsSchema";
// ============================================================
inline constexpr const char* CLAW_SETTINGS_SCHEMA_NAME = "SettingsSchema";

// ============================================================
// Helper: default_config_home()
// Rust: pub fn default_config_home() -> PathBuf
//   std::env::var_os("CLAW_CONFIG_HOME")
//     .map(PathBuf::from)
//     .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".claw")))
//     .unwrap_or_else(|| PathBuf::from(".claw"))
// ============================================================
[[nodiscard]] static std::filesystem::path default_config_home() {
    if (const char* v = std::getenv("CLAW_CONFIG_HOME"); v && *v) {
        return std::filesystem::path(v);
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".claw";
    }
    return std::filesystem::path(".claw");
}

// ============================================================
// Anonymous-namespace helpers (mirror the private fn in config.rs)
// ============================================================
namespace {

// ---------------------------------------------------------------------------
// push_unique / extend_unique
// Rust: fn push_unique(target: &mut Vec<String>, value: String)
//       fn extend_unique(target: &mut Vec<String>, values: &[String])
// ---------------------------------------------------------------------------
void push_unique(std::vector<std::string>& target, std::string value) {
    if (std::find(target.begin(), target.end(), value) == target.end()) {
        target.push_back(std::move(value));
    }
}

void extend_unique(std::vector<std::string>& target, const std::vector<std::string>& values) {
    for (const auto& v : values) {
        push_unique(target, v);
    }
}

// ---------------------------------------------------------------------------
// deep_merge_objects
// Rust: fn deep_merge_objects(target: &mut BTreeMap<String, JsonValue>,
//                              source: &BTreeMap<String, JsonValue>)
// ---------------------------------------------------------------------------
void deep_merge_objects(std::map<std::string, NlJson>& target,
                        const std::map<std::string, NlJson>& source) {
    for (const auto& [key, value] : source) {
        auto it = target.find(key);
        if (it != target.end() && it->second.is_object() && value.is_object()) {
            // Both are objects — recurse
            std::map<std::string, NlJson> existing_map, incoming_map;
            for (auto& [k2, v2] : it->second.items()) existing_map[k2] = v2;
            for (auto& [k2, v2] : value.items())       incoming_map[k2] = v2;
            deep_merge_objects(existing_map, incoming_map);
            NlJson merged_obj = NlJson::object();
            for (auto& [k2, v2] : existing_map) merged_obj[k2] = v2;
            it->second = std::move(merged_obj);
        } else {
            target[key] = value;
        }
    }
}

// ---------------------------------------------------------------------------
// read_optional_json_object
// Rust: fn read_optional_json_object(path: &Path)
//           -> Result<Option<BTreeMap<String, JsonValue>>, ConfigError>
// ---------------------------------------------------------------------------
using JsonMap = std::map<std::string, NlJson>;

tl::expected<std::optional<JsonMap>, ConfigError>
read_optional_json_object(const std::filesystem::path& path) {
    // Determine if this is a legacy config file (.claw.json)
    bool is_legacy_config = (path.filename().string() == ".claw.json");

    std::ifstream file(path);
    if (!file) {
        // Distinguish "not found" from other I/O errors
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return std::optional<JsonMap>{std::nullopt};
        }
        return tl::unexpected(ConfigError::io(
            std::format("cannot open config file: {}", path.string())));
    }

    std::string contents((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());

    // Trim whitespace to check for empty file
    auto trimmed = contents;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

    if (trimmed.empty()) {
        return std::optional<JsonMap>{JsonMap{}};
    }

    NlJson parsed;
    try {
        parsed = NlJson::parse(contents);
    } catch (const std::exception& e) {
        if (is_legacy_config) {
            return std::optional<JsonMap>{std::nullopt};
        }
        return tl::unexpected(ConfigError::parse(
            std::format("{}: {}", path.string(), e.what())));
    }

    if (!parsed.is_object()) {
        if (is_legacy_config) {
            return std::optional<JsonMap>{std::nullopt};
        }
        return tl::unexpected(ConfigError::parse(
            std::format("{}: top-level settings value must be a JSON object",
                        path.string())));
    }

    JsonMap result;
    for (auto& [k, v] : parsed.items()) {
        result[k] = v;
    }
    return std::optional<JsonMap>{std::move(result)};
}

// ---------------------------------------------------------------------------
// expect_object
// Rust: fn expect_object<'a>(value: &'a JsonValue, context: &str)
//           -> Result<&'a BTreeMap<String, JsonValue>, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<const NlJson*, ConfigError>
expect_object_json(const NlJson& value, std::string_view context) {
    if (!value.is_object()) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: expected JSON object", context)));
    }
    return &value;
}

// Variant that takes a JsonMap entry value
tl::expected<NlJson, ConfigError>
expect_object_copy(const NlJson& value, std::string_view context) {
    if (!value.is_object()) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: expected JSON object", context)));
    }
    return value;
}

// Map-returning variant for callers that need a JsonMap
tl::expected<JsonMap, ConfigError>
expect_object_map(const NlJson& value, std::string_view context) {
    if (!value.is_object()) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: expected JSON object", context)));
    }
    JsonMap result;
    for (auto& [k, v] : value.items()) result[k] = v;
    return result;
}

// ---------------------------------------------------------------------------
// expect_string — required string field
// Rust: fn expect_string<'a>(object: &'a BTreeMap<String, JsonValue>,
//                             key: &str, context: &str) -> Result<&'a str, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<std::string, ConfigError>
expect_string(const JsonMap& object, std::string_view key, std::string_view context) {
    auto it = object.find(std::string(key));
    if (it == object.end() || !it->second.is_string()) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: missing string field {}", context, key)));
    }
    return it->second.get<std::string>();
}

// ---------------------------------------------------------------------------
// optional_string — optional string field
// Rust: fn optional_string<'a>(object: &'a BTreeMap<String, JsonValue>,
//                               key: &str, context: &str)
//           -> Result<Option<&'a str>, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<std::optional<std::string>, ConfigError>
optional_string(const JsonMap& object, std::string_view key, std::string_view context) {
    auto it = object.find(std::string(key));
    if (it == object.end()) return std::optional<std::string>{std::nullopt};
    if (!it->second.is_string()) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: field {} must be a string", context, key)));
    }
    return std::optional<std::string>{it->second.get<std::string>()};
}

// ---------------------------------------------------------------------------
// optional_bool — optional bool field
// Rust: fn optional_bool(object: &BTreeMap<String, JsonValue>, key: &str, context: &str)
//           -> Result<Option<bool>, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<std::optional<bool>, ConfigError>
optional_bool(const JsonMap& object, std::string_view key, std::string_view context) {
    auto it = object.find(std::string(key));
    if (it == object.end()) return std::optional<bool>{std::nullopt};
    if (!it->second.is_boolean()) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: field {} must be a boolean", context, key)));
    }
    return std::optional<bool>{it->second.get<bool>()};
}

// ---------------------------------------------------------------------------
// optional_u16 — optional u16 field
// Rust: fn optional_u16(object: &BTreeMap<String, JsonValue>, key: &str, context: &str)
//           -> Result<Option<u16>, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<std::optional<uint16_t>, ConfigError>
optional_u16(const JsonMap& object, std::string_view key, std::string_view context) {
    auto it = object.find(std::string(key));
    if (it == object.end()) return std::optional<uint16_t>{std::nullopt};
    if (!it->second.is_number_integer()) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: field {} must be an integer", context, key)));
    }
    int64_t n = it->second.get<int64_t>();
    if (n < 0 || n > 65535) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: field {} is out of range", context, key)));
    }
    return std::optional<uint16_t>{static_cast<uint16_t>(n)};
}

// ---------------------------------------------------------------------------
// optional_u64 — optional u64 field
// Rust: fn optional_u64(object: &BTreeMap<String, JsonValue>, key: &str, context: &str)
//           -> Result<Option<u64>, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<std::optional<uint64_t>, ConfigError>
optional_u64(const JsonMap& object, std::string_view key, std::string_view context) {
    auto it = object.find(std::string(key));
    if (it == object.end()) return std::optional<uint64_t>{std::nullopt};
    if (!it->second.is_number_integer()) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: field {} must be a non-negative integer", context, key)));
    }
    int64_t n = it->second.get<int64_t>();
    if (n < 0) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: field {} is out of range", context, key)));
    }
    return std::optional<uint64_t>{static_cast<uint64_t>(n)};
}

// ---------------------------------------------------------------------------
// optional_string_array — optional array-of-strings field
// Rust: fn optional_string_array(...) -> Result<Option<Vec<String>>, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<std::optional<std::vector<std::string>>, ConfigError>
optional_string_array(const JsonMap& object, std::string_view key, std::string_view context) {
    auto it = object.find(std::string(key));
    if (it == object.end()) return std::optional<std::vector<std::string>>{std::nullopt};
    if (!it->second.is_array()) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: field {} must be an array", context, key)));
    }
    std::vector<std::string> result;
    for (const auto& item : it->second) {
        if (!item.is_string()) {
            return tl::unexpected(ConfigError::parse(
                std::format("{}: field {} must contain only strings", context, key)));
        }
        result.push_back(item.get<std::string>());
    }
    return std::optional<std::vector<std::string>>{std::move(result)};
}

// ---------------------------------------------------------------------------
// optional_string_map — optional object-of-strings field
// Rust: fn optional_string_map(...) -> Result<Option<BTreeMap<String, String>>, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<std::optional<std::map<std::string, std::string>>, ConfigError>
optional_string_map(const JsonMap& object, std::string_view key, std::string_view context) {
    auto it = object.find(std::string(key));
    if (it == object.end()) return std::optional<std::map<std::string, std::string>>{std::nullopt};
    if (!it->second.is_object()) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: field {} must be an object", context, key)));
    }
    std::map<std::string, std::string> result;
    for (auto& [entry_key, entry_value] : it->second.items()) {
        if (!entry_value.is_string()) {
            return tl::unexpected(ConfigError::parse(
                std::format("{}: field {} must contain only string values", context, key)));
        }
        result[entry_key] = entry_value.get<std::string>();
    }
    return std::optional<std::map<std::string, std::string>>{std::move(result)};
}

// ---------------------------------------------------------------------------
// parse_bool_map
// Rust: fn parse_bool_map(value: &JsonValue, context: &str)
//           -> Result<BTreeMap<String, bool>, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<std::map<std::string, bool>, ConfigError>
parse_bool_map(const NlJson& value, std::string_view context) {
    if (!value.is_object()) {
        return tl::unexpected(ConfigError::parse(
            std::format("{}: expected JSON object", context)));
    }
    std::map<std::string, bool> result;
    for (auto& [k, v] : value.items()) {
        if (!v.is_boolean()) {
            return tl::unexpected(ConfigError::parse(
                std::format("{}: field {} must be a boolean", context, k)));
        }
        result[k] = v.get<bool>();
    }
    return result;
}

// ---------------------------------------------------------------------------
// parse_permission_mode_label
// Rust: fn parse_permission_mode_label(mode: &str, context: &str)
//           -> Result<ResolvedPermissionMode, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<ResolvedPermissionMode, ConfigError>
parse_permission_mode_label(std::string_view mode, std::string_view context) {
    if (mode == "default" || mode == "plan" || mode == "read-only") {
        return ResolvedPermissionMode::ReadOnly;
    }
    if (mode == "acceptEdits" || mode == "auto" || mode == "workspace-write") {
        return ResolvedPermissionMode::WorkspaceWrite;
    }
    if (mode == "dontAsk" || mode == "danger-full-access") {
        return ResolvedPermissionMode::DangerFullAccess;
    }
    return tl::unexpected(ConfigError::parse(
        std::format("{}: unsupported permission mode {}", context, mode)));
}

// ---------------------------------------------------------------------------
// parse_filesystem_mode_label
// Rust: fn parse_filesystem_mode_label(value: &str)
//           -> Result<FilesystemIsolationMode, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<ClawFilesystemIsolationMode, ConfigError>
parse_filesystem_mode_label(std::string_view value) {
    if (value == "off")             return ClawFilesystemIsolationMode::Off;
    if (value == "workspace-only")  return ClawFilesystemIsolationMode::WorkspaceOnly;
    if (value == "allow-list")      return ClawFilesystemIsolationMode::AllowList;
    return tl::unexpected(ConfigError::parse(
        std::format("merged settings.sandbox.filesystemMode: unsupported filesystem mode {}",
                    value)));
}

// ---------------------------------------------------------------------------
// parse_optional_mcp_oauth_config
// Rust: fn parse_optional_mcp_oauth_config(object: &BTreeMap<..>, context: &str)
//           -> Result<Option<McpOAuthConfig>, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<std::optional<McpOAuthConfig>, ConfigError>
parse_optional_mcp_oauth_config(const JsonMap& object, std::string_view context) {
    auto it = object.find("oauth");
    if (it == object.end()) return std::optional<McpOAuthConfig>{std::nullopt};

    auto oauth_map_res = expect_object_map(it->second, std::format("{}.oauth", context));
    if (!oauth_map_res) return tl::unexpected(oauth_map_res.error());
    const JsonMap& oauth = *oauth_map_res;

    McpOAuthConfig cfg;

    auto client_id_res = optional_string(oauth, "clientId", context);
    if (!client_id_res) return tl::unexpected(client_id_res.error());
    cfg.client_id = *client_id_res;

    auto port_res = optional_u16(oauth, "callbackPort", context);
    if (!port_res) return tl::unexpected(port_res.error());
    cfg.callback_port = *port_res;

    auto url_res = optional_string(oauth, "authServerMetadataUrl", context);
    if (!url_res) return tl::unexpected(url_res.error());
    cfg.auth_server_metadata_url = *url_res;

    auto xaa_res = optional_bool(oauth, "xaa", context);
    if (!xaa_res) return tl::unexpected(xaa_res.error());
    cfg.xaa = *xaa_res;

    return std::optional<McpOAuthConfig>{std::move(cfg)};
}

// ---------------------------------------------------------------------------
// parse_mcp_remote_server_config
// Rust: fn parse_mcp_remote_server_config(object: &BTreeMap<..>, context: &str)
//           -> Result<McpRemoteServerConfig, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<McpRemoteServerConfig, ConfigError>
parse_mcp_remote_server_config(const JsonMap& object, std::string_view context) {
    McpRemoteServerConfig cfg;

    auto url_res = expect_string(object, "url", context);
    if (!url_res) return tl::unexpected(url_res.error());
    cfg.url = *url_res;

    auto headers_res = optional_string_map(object, "headers", context);
    if (!headers_res) return tl::unexpected(headers_res.error());
    if (*headers_res) cfg.headers = **headers_res;

    auto helper_res = optional_string(object, "headersHelper", context);
    if (!helper_res) return tl::unexpected(helper_res.error());
    cfg.headers_helper = *helper_res;

    auto oauth_res = parse_optional_mcp_oauth_config(object, context);
    if (!oauth_res) return tl::unexpected(oauth_res.error());
    cfg.oauth = *oauth_res;

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_mcp_server_config
// Rust: fn parse_mcp_server_config(server_name: &str, value: &JsonValue, context: &str)
//           -> Result<McpServerConfig, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<ClawMcpServerConfig, ConfigError>
parse_mcp_server_config(const std::string& server_name,
                        const NlJson&      value,
                        std::string_view   context) {
    auto map_res = expect_object_map(value, context);
    if (!map_res) return tl::unexpected(map_res.error());
    const JsonMap& object = *map_res;

    auto type_res = optional_string(object, "type", context);
    if (!type_res) return tl::unexpected(type_res.error());
    std::string server_type = type_res->value_or("stdio");

    if (server_type == "stdio") {
        ClawMcpStdioServerConfig cfg;

        auto cmd_res = expect_string(object, "command", context);
        if (!cmd_res) return tl::unexpected(cmd_res.error());
        cfg.command = *cmd_res;

        auto args_res = optional_string_array(object, "args", context);
        if (!args_res) return tl::unexpected(args_res.error());
        if (*args_res) cfg.args = **args_res;

        auto env_res = optional_string_map(object, "env", context);
        if (!env_res) return tl::unexpected(env_res.error());
        if (*env_res) cfg.env = **env_res;

        auto timeout_res = optional_u64(object, "toolCallTimeoutMs", context);
        if (!timeout_res) return tl::unexpected(timeout_res.error());
        cfg.tool_call_timeout_ms = *timeout_res;

        return ClawMcpServerConfig::make_stdio(std::move(cfg));
    }

    if (server_type == "sse") {
        auto remote_res = parse_mcp_remote_server_config(object, context);
        if (!remote_res) return tl::unexpected(remote_res.error());
        return ClawMcpServerConfig::make_sse(std::move(*remote_res));
    }

    if (server_type == "http") {
        auto remote_res = parse_mcp_remote_server_config(object, context);
        if (!remote_res) return tl::unexpected(remote_res.error());
        return ClawMcpServerConfig::make_http(std::move(*remote_res));
    }

    if (server_type == "ws") {
        McpWebSocketServerConfig cfg;

        auto url_res = expect_string(object, "url", context);
        if (!url_res) return tl::unexpected(url_res.error());
        cfg.url = *url_res;

        auto headers_res = optional_string_map(object, "headers", context);
        if (!headers_res) return tl::unexpected(headers_res.error());
        if (*headers_res) cfg.headers = **headers_res;

        auto helper_res = optional_string(object, "headersHelper", context);
        if (!helper_res) return tl::unexpected(helper_res.error());
        cfg.headers_helper = *helper_res;

        return ClawMcpServerConfig::make_ws(std::move(cfg));
    }

    if (server_type == "sdk") {
        McpSdkServerConfig cfg;

        auto name_res = expect_string(object, "name", context);
        if (!name_res) return tl::unexpected(name_res.error());
        cfg.name = *name_res;

        return ClawMcpServerConfig::make_sdk(std::move(cfg));
    }

    if (server_type == "claudeai-proxy") {
        McpManagedProxyServerConfig cfg;

        auto url_res = expect_string(object, "url", context);
        if (!url_res) return tl::unexpected(url_res.error());
        cfg.url = *url_res;

        auto id_res = expect_string(object, "id", context);
        if (!id_res) return tl::unexpected(id_res.error());
        cfg.id = *id_res;

        return ClawMcpServerConfig::make_managed_proxy(std::move(cfg));
    }

    return tl::unexpected(ConfigError::parse(
        std::format("{}: unsupported MCP server type for {}: {}",
                    context, server_name, server_type)));
}

// ---------------------------------------------------------------------------
// merge_mcp_servers
// Rust: fn merge_mcp_servers(target: &mut BTreeMap<..>, source: ConfigSource,
//                             root: &BTreeMap<..>, path: &Path)
//           -> Result<(), ConfigError>
// ---------------------------------------------------------------------------
tl::expected<void, ConfigError>
merge_mcp_servers(std::map<std::string, ScopedMcpServerConfig>& target,
                  ClawConfigSource                               source,
                  const JsonMap&                                 root,
                  const std::filesystem::path&                   path) {
    auto it = root.find("mcpServers");
    if (it == root.end()) return {};

    auto servers_ctx = std::format("{}: mcpServers", path.string());
    auto servers_res = expect_object_map(it->second, servers_ctx);
    if (!servers_res) return tl::unexpected(servers_res.error());
    const JsonMap& servers = *servers_res;

    for (const auto& [name, value] : servers) {
        auto entry_ctx = std::format("{}: mcpServers.{}", path.string(), name);
        auto parsed_res = parse_mcp_server_config(name, value, entry_ctx);
        if (!parsed_res) return tl::unexpected(parsed_res.error());
        target[name] = ScopedMcpServerConfig{ source, std::move(*parsed_res) };
    }
    return {};
}

// ---------------------------------------------------------------------------
// parse_optional_model
// Rust: fn parse_optional_model(root: &JsonValue) -> Option<String>
// ---------------------------------------------------------------------------
std::optional<std::string> parse_optional_model(const JsonMap& root) {
    auto it = root.find("model");
    if (it == root.end() || !it->second.is_string()) return std::nullopt;
    return it->second.get<std::string>();
}

// ---------------------------------------------------------------------------
// parse_optional_hooks_config
// Rust: fn parse_optional_hooks_config(root: &JsonValue)
//           -> Result<RuntimeHookConfig, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<RuntimeHookConfig, ConfigError>
parse_optional_hooks_config(const JsonMap& root) {
    auto hooks_it = root.find("hooks");
    if (hooks_it == root.end()) return RuntimeHookConfig{};

    auto hooks_res = expect_object_map(hooks_it->second, "merged settings.hooks");
    if (!hooks_res) return tl::unexpected(hooks_res.error());
    const JsonMap& hooks = *hooks_res;

    RuntimeHookConfig cfg;

    auto pre_res = optional_string_array(hooks, "PreToolUse", "merged settings.hooks");
    if (!pre_res) return tl::unexpected(pre_res.error());
    if (*pre_res) cfg.pre_tool_use = **pre_res;

    auto post_res = optional_string_array(hooks, "PostToolUse", "merged settings.hooks");
    if (!post_res) return tl::unexpected(post_res.error());
    if (*post_res) cfg.post_tool_use = **post_res;

    auto fail_res = optional_string_array(hooks, "PostToolUseFailure", "merged settings.hooks");
    if (!fail_res) return tl::unexpected(fail_res.error());
    if (*fail_res) cfg.post_tool_use_failure = **fail_res;

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_optional_permission_rules
// Rust: fn parse_optional_permission_rules(root: &JsonValue)
//           -> Result<RuntimePermissionRuleConfig, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<RuntimePermissionRuleConfig, ConfigError>
parse_optional_permission_rules(const JsonMap& root) {
    auto perms_it = root.find("permissions");
    if (perms_it == root.end() || !perms_it->second.is_object()) {
        return RuntimePermissionRuleConfig{};
    }

    auto perms_res = expect_object_map(perms_it->second, "merged settings.permissions");
    if (!perms_res) return tl::unexpected(perms_res.error());
    const JsonMap& permissions = *perms_res;

    RuntimePermissionRuleConfig cfg;

    auto allow_res = optional_string_array(permissions, "allow", "merged settings.permissions");
    if (!allow_res) return tl::unexpected(allow_res.error());
    if (*allow_res) cfg.allow = **allow_res;

    auto deny_res = optional_string_array(permissions, "deny", "merged settings.permissions");
    if (!deny_res) return tl::unexpected(deny_res.error());
    if (*deny_res) cfg.deny = **deny_res;

    auto ask_res = optional_string_array(permissions, "ask", "merged settings.permissions");
    if (!ask_res) return tl::unexpected(ask_res.error());
    if (*ask_res) cfg.ask = **ask_res;

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_optional_plugin_config
// Rust: fn parse_optional_plugin_config(root: &JsonValue)
//           -> Result<RuntimePluginConfig, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<RuntimePluginConfig, ConfigError>
parse_optional_plugin_config(const JsonMap& root) {
    RuntimePluginConfig config;

    // enabledPlugins (top-level shorthand)
    auto ep_it = root.find("enabledPlugins");
    if (ep_it != root.end()) {
        auto ep_res = parse_bool_map(ep_it->second, "merged settings.enabledPlugins");
        if (!ep_res) return tl::unexpected(ep_res.error());
        config.enabled_plugins = *ep_res;
    }

    auto plugins_it = root.find("plugins");
    if (plugins_it == root.end()) return config;

    auto plugins_res = expect_object_map(plugins_it->second, "merged settings.plugins");
    if (!plugins_res) return tl::unexpected(plugins_res.error());
    const JsonMap& plugins = *plugins_res;

    // plugins.enabled overrides enabledPlugins
    auto enabled_it = plugins.find("enabled");
    if (enabled_it != plugins.end()) {
        auto enabled_res = parse_bool_map(enabled_it->second, "merged settings.plugins.enabled");
        if (!enabled_res) return tl::unexpected(enabled_res.error());
        config.enabled_plugins = *enabled_res;
    }

    auto ext_res = optional_string_array(plugins, "externalDirectories", "merged settings.plugins");
    if (!ext_res) return tl::unexpected(ext_res.error());
    if (*ext_res) config.external_directories = **ext_res;

    auto root_res = optional_string(plugins, "installRoot", "merged settings.plugins");
    if (!root_res) return tl::unexpected(root_res.error());
    config.install_root = *root_res;

    auto reg_res = optional_string(plugins, "registryPath", "merged settings.plugins");
    if (!reg_res) return tl::unexpected(reg_res.error());
    config.registry_path = *reg_res;

    auto bundled_res = optional_string(plugins, "bundledRoot", "merged settings.plugins");
    if (!bundled_res) return tl::unexpected(bundled_res.error());
    config.bundled_root = *bundled_res;

    return config;
}

// ---------------------------------------------------------------------------
// parse_optional_permission_mode
// Rust: fn parse_optional_permission_mode(root: &JsonValue)
//           -> Result<Option<ResolvedPermissionMode>, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<std::optional<ResolvedPermissionMode>, ConfigError>
parse_optional_permission_mode(const JsonMap& root) {
    // Check top-level "permissionMode" first
    auto pm_it = root.find("permissionMode");
    if (pm_it != root.end() && pm_it->second.is_string()) {
        auto mode_str = pm_it->second.get<std::string>();
        auto res = parse_permission_mode_label(mode_str, "merged settings.permissionMode");
        if (!res) return tl::unexpected(res.error());
        return std::optional<ResolvedPermissionMode>{*res};
    }

    // Fall back to permissions.defaultMode
    auto perms_it = root.find("permissions");
    if (perms_it == root.end() || !perms_it->second.is_object()) {
        return std::optional<ResolvedPermissionMode>{std::nullopt};
    }
    auto& perms_obj = perms_it->second;
    auto dm_it = perms_obj.find("defaultMode");
    if (dm_it == perms_obj.end() || !dm_it->is_string()) {
        return std::optional<ResolvedPermissionMode>{std::nullopt};
    }
    auto mode_str = dm_it->get<std::string>();
    auto res = parse_permission_mode_label(mode_str,
                                           "merged settings.permissions.defaultMode");
    if (!res) return tl::unexpected(res.error());
    return std::optional<ResolvedPermissionMode>{*res};
}

// ---------------------------------------------------------------------------
// parse_optional_sandbox_config
// Rust: fn parse_optional_sandbox_config(root: &JsonValue)
//           -> Result<SandboxConfig, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<ClawSandboxConfig, ConfigError>
parse_optional_sandbox_config(const JsonMap& root) {
    auto sb_it = root.find("sandbox");
    if (sb_it == root.end()) return ClawSandboxConfig{};

    auto sb_res = expect_object_map(sb_it->second, "merged settings.sandbox");
    if (!sb_res) return tl::unexpected(sb_res.error());
    const JsonMap& sandbox = *sb_res;

    ClawSandboxConfig cfg;

    auto enabled_res = optional_bool(sandbox, "enabled", "merged settings.sandbox");
    if (!enabled_res) return tl::unexpected(enabled_res.error());
    cfg.enabled = *enabled_res;

    auto ns_res = optional_bool(sandbox, "namespaceRestrictions", "merged settings.sandbox");
    if (!ns_res) return tl::unexpected(ns_res.error());
    cfg.namespace_restrictions = *ns_res;

    auto net_res = optional_bool(sandbox, "networkIsolation", "merged settings.sandbox");
    if (!net_res) return tl::unexpected(net_res.error());
    cfg.network_isolation = *net_res;

    // filesystemMode — parse the string label then map to enum
    auto fsm_str_res = optional_string(sandbox, "filesystemMode", "merged settings.sandbox");
    if (!fsm_str_res) return tl::unexpected(fsm_str_res.error());
    if (*fsm_str_res) {
        auto fsm_res = parse_filesystem_mode_label(**fsm_str_res);
        if (!fsm_res) return tl::unexpected(fsm_res.error());
        cfg.filesystem_mode = *fsm_res;
    }

    auto mounts_res = optional_string_array(sandbox, "allowedMounts", "merged settings.sandbox");
    if (!mounts_res) return tl::unexpected(mounts_res.error());
    if (*mounts_res) cfg.allowed_mounts = **mounts_res;

    return cfg;
}

// ---------------------------------------------------------------------------
// parse_optional_oauth_config
// Rust: fn parse_optional_oauth_config(root: &JsonValue, context: &str)
//           -> Result<Option<OAuthConfig>, ConfigError>
// ---------------------------------------------------------------------------
tl::expected<std::optional<OAuthConfig>, ConfigError>
parse_optional_oauth_config(const JsonMap& root, std::string_view context) {
    auto oauth_it = root.find("oauth");
    if (oauth_it == root.end()) return std::optional<OAuthConfig>{std::nullopt};

    auto obj_res = expect_object_map(oauth_it->second, context);
    if (!obj_res) return tl::unexpected(obj_res.error());
    const JsonMap& object = *obj_res;

    OAuthConfig cfg;

    auto client_id_res = expect_string(object, "clientId", context);
    if (!client_id_res) return tl::unexpected(client_id_res.error());
    cfg.client_id = *client_id_res;

    auto auth_url_res = expect_string(object, "authorizeUrl", context);
    if (!auth_url_res) return tl::unexpected(auth_url_res.error());
    cfg.authorize_url = *auth_url_res;

    auto token_url_res = expect_string(object, "tokenUrl", context);
    if (!token_url_res) return tl::unexpected(token_url_res.error());
    cfg.token_url = *token_url_res;

    auto port_res = optional_u16(object, "callbackPort", context);
    if (!port_res) return tl::unexpected(port_res.error());
    cfg.callback_port = *port_res;

    auto redirect_res = optional_string(object, "manualRedirectUrl", context);
    if (!redirect_res) return tl::unexpected(redirect_res.error());
    cfg.manual_redirect_url = *redirect_res;

    auto scopes_res = optional_string_array(object, "scopes", context);
    if (!scopes_res) return tl::unexpected(scopes_res.error());
    if (*scopes_res) cfg.scopes = **scopes_res;

    return std::optional<OAuthConfig>{std::move(cfg)};
}

} // anonymous namespace

// ============================================================
// ClawConfigLoader — faithful C++20 translation of ConfigLoader in config.rs
// ============================================================

// Rust: impl ConfigLoader
// NOTE: We name this ClawConfigLoader to avoid conflicting with the existing
//       ConfigLoader class declared in config.hpp.

struct ClawConfigLoader {
    std::filesystem::path cwd;
    std::filesystem::path config_home;

    // Rust: pub fn new(cwd: impl Into<PathBuf>, config_home: impl Into<PathBuf>) -> Self
    ClawConfigLoader(std::filesystem::path cwd_, std::filesystem::path config_home_)
        : cwd(std::move(cwd_))
        , config_home(std::move(config_home_))
    {}

    // Rust: pub fn default_for(cwd: impl Into<PathBuf>) -> Self
    [[nodiscard]] static ClawConfigLoader default_for(std::filesystem::path cwd_) {
        return ClawConfigLoader{ std::move(cwd_), default_config_home() };
    }

    // Rust: pub fn config_home(&self) -> &Path
    [[nodiscard]] const std::filesystem::path& get_config_home() const noexcept {
        return config_home;
    }

    // Rust: pub fn discover(&self) -> Vec<ConfigEntry>
    // Returns the canonical discovery list in priority order:
    //   1. user legacy  (~/.claw.json)
    //   2. user new     (~/.claw/settings.json)
    //   3. project compat  (<cwd>/.claw.json)
    //   4. project new     (<cwd>/.claw/settings.json)
    //   5. project local   (<cwd>/.claw/settings.local.json)
    [[nodiscard]] std::vector<ConfigEntry> discover() const {
        std::filesystem::path user_legacy_path;
        auto parent = config_home.parent_path();
        if (!parent.empty() && parent != config_home) {
            user_legacy_path = parent / ".claw.json";
        } else {
            user_legacy_path = std::filesystem::path(".claw.json");
        }

        return {
            ConfigEntry{ ClawConfigSource::User,    user_legacy_path },
            ConfigEntry{ ClawConfigSource::User,    config_home / "settings.json" },
            ConfigEntry{ ClawConfigSource::Project, cwd / ".claw.json" },
            ConfigEntry{ ClawConfigSource::Project, cwd / ".claw" / "settings.json" },
            ConfigEntry{ ClawConfigSource::Local,   cwd / ".claw" / "settings.local.json" },
        };
    }

    // Rust: pub fn load(&self) -> Result<RuntimeConfig, ConfigError>
    [[nodiscard]] tl::expected<ClawRuntimeConfig, ConfigError> load() const {
        JsonMap merged;
        std::vector<ConfigEntry> loaded_entries;
        std::map<std::string, ScopedMcpServerConfig> mcp_servers;

        for (const auto& entry : discover()) {
            auto value_res = read_optional_json_object(entry.path);
            if (!value_res) return tl::unexpected(value_res.error());
            if (!*value_res) continue;  // file not found — skip

            const JsonMap& value = **value_res;

            // merge MCP servers from this file
            auto mcp_res = merge_mcp_servers(mcp_servers, entry.source, value, entry.path);
            if (!mcp_res) return tl::unexpected(mcp_res.error());

            // deep-merge into the running merged map
            deep_merge_objects(merged, value);
            loaded_entries.push_back(entry);
        }

        // Parse typed feature config from fully merged map
        auto hooks_res = parse_optional_hooks_config(merged);
        if (!hooks_res) return tl::unexpected(hooks_res.error());

        auto plugins_res = parse_optional_plugin_config(merged);
        if (!plugins_res) return tl::unexpected(plugins_res.error());

        auto oauth_res = parse_optional_oauth_config(merged, "merged settings.oauth");
        if (!oauth_res) return tl::unexpected(oauth_res.error());

        auto pmode_res = parse_optional_permission_mode(merged);
        if (!pmode_res) return tl::unexpected(pmode_res.error());

        auto prules_res = parse_optional_permission_rules(merged);
        if (!prules_res) return tl::unexpected(prules_res.error());

        auto sandbox_res = parse_optional_sandbox_config(merged);
        if (!sandbox_res) return tl::unexpected(sandbox_res.error());

        ClawRuntimeFeatureConfig feature_config;
        feature_config.hooks           = std::move(*hooks_res);
        feature_config.plugins         = std::move(*plugins_res);
        feature_config.mcp             = McpConfigCollection{ std::move(mcp_servers) };
        feature_config.oauth           = std::move(*oauth_res);
        feature_config.model           = parse_optional_model(merged);
        feature_config.permission_mode = *pmode_res;
        feature_config.permission_rules = std::move(*prules_res);
        feature_config.sandbox         = std::move(*sandbox_res);

        ClawRuntimeConfig result;
        result.merged          = std::move(merged);
        result.loaded_entries  = std::move(loaded_entries);
        result.feature_config  = std::move(feature_config);
        return result;
    }
};

// ============================================================
// Implementations of the existing ConfigLoader methods
// (declared in config.hpp) — kept for ABI compatibility with
// code that already uses the original simplified interface.
// ============================================================

tl::expected<RuntimeConfig, std::string> ConfigLoader::load_file(
    const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in) {
        return tl::unexpected(std::format("cannot open config: {}", path.string()));
    }
    NlJson j;
    try {
        j = NlJson::parse(in);
    } catch (const std::exception& e) {
        return tl::unexpected(
            std::format("JSON parse error in {}: {}", path.string(), e.what()));
    }

    RuntimeConfig cfg;
    if (j.contains("model"))               cfg.model               = j["model"].get<std::string>();
    if (j.contains("api_key"))             cfg.api_key             = j["api_key"].get<std::string>();
    if (j.contains("base_url"))            cfg.base_url            = j["base_url"].get<std::string>();
    if (j.contains("system_prompt_extra")) cfg.system_prompt_extra = j["system_prompt_extra"].get<std::string>();

    if (j.contains("mcp_servers") && j["mcp_servers"].is_object()) {
        for (auto& [name, srv_j] : j["mcp_servers"].items()) {
            // Parse using the faithful MCP parser and convert to the legacy stub format
            std::string context = std::format("{}: mcpServers.{}", path.string(), name);
            // Convert nlohmann item to our JsonMap for parse_mcp_server_config
            JsonMap srv_map;
            if (srv_j.is_object()) {
                for (auto& [k, v] : srv_j.items()) srv_map[k] = v;
            }
            // Use a simple fallback: just store what we can in the existing struct
            auto type_str = srv_j.value("type", std::string{"stdio"});
            if (type_str == "sse") {
                McpSseServerConfig sse;
                sse.url = srv_j.value("url", std::string{});
                if (srv_j.contains("headers") && srv_j["headers"].is_object()) {
                    for (auto& [k, v] : srv_j["headers"].items())
                        sse.headers[k] = v.get<std::string>();
                }
                cfg.mcp_servers.push_back(McpServerEntry{ name, std::move(sse) });
            } else if (type_str == "http") {
                McpHttpServerConfig http;
                http.url = srv_j.value("url", std::string{});
                if (srv_j.contains("oauth_client_id"))
                    http.oauth_client_id = srv_j["oauth_client_id"].get<std::string>();
                cfg.mcp_servers.push_back(McpServerEntry{ name, std::move(http) });
            } else {
                // stdio (default)
                McpStdioServerConfig stdio;
                stdio.command = srv_j.value("command", std::string{});
                if (srv_j.contains("args") && srv_j["args"].is_array()) {
                    for (const auto& a : srv_j["args"]) stdio.args.push_back(a.get<std::string>());
                }
                if (srv_j.contains("env") && srv_j["env"].is_object()) {
                    for (auto& [k, v] : srv_j["env"].items()) stdio.env[k] = v.get<std::string>();
                }
                if (srv_j.contains("tool_call_timeout_ms"))
                    stdio.tool_call_timeout_ms = srv_j["tool_call_timeout_ms"].get<uint32_t>();
                cfg.mcp_servers.push_back(McpServerEntry{ name, std::move(stdio) });
            }
        }
    }

    if (j.contains("features") && j["features"].is_object()) {
        auto& f = j["features"];
        cfg.features.enable_caching    = f.value("enable_caching",    cfg.features.enable_caching);
        cfg.features.enable_compaction = f.value("enable_compaction", cfg.features.enable_compaction);
        cfg.features.enable_hooks      = f.value("enable_hooks",      cfg.features.enable_hooks);
        cfg.features.enable_sandbox    = f.value("enable_sandbox",    cfg.features.enable_sandbox);
        cfg.features.enable_lsp        = f.value("enable_lsp",        cfg.features.enable_lsp);
        if (f.contains("model_override"))   cfg.features.model_override   = f["model_override"].get<std::string>();
        if (f.contains("max_output_tokens")) cfg.features.max_output_tokens = f["max_output_tokens"].get<std::size_t>();
    }

    if (j.contains("allowed_tools") && j["allowed_tools"].is_array()) {
        for (const auto& t : j["allowed_tools"]) cfg.allowed_tools.push_back(t.get<std::string>());
    }
    if (j.contains("denied_tools") && j["denied_tools"].is_array()) {
        for (const auto& t : j["denied_tools"]) cfg.denied_tools.push_back(t.get<std::string>());
    }

    return cfg;
}

void ConfigLoader::merge(RuntimeConfig& dst, const RuntimeConfig& src) {
    if (src.api_key)             dst.api_key             = src.api_key;
    if (src.base_url)            dst.base_url            = src.base_url;
    if (src.system_prompt_extra) dst.system_prompt_extra = src.system_prompt_extra;
    if (!src.model.empty())      dst.model               = src.model;
    for (const auto& srv : src.mcp_servers)   dst.mcp_servers.push_back(srv);
    for (const auto& t   : src.allowed_tools) dst.allowed_tools.push_back(t);
    for (const auto& t   : src.denied_tools)  dst.denied_tools.push_back(t);
    // Feature flags: src overrides entire struct
    dst.features = src.features;
}

tl::expected<RuntimeConfig, std::string> ConfigLoader::load(
    const std::filesystem::path& cwd)
{
    RuntimeConfig merged;
    merged.primary_source = ConfigSource::DefaultBuiltin;

    // Discovery order: system → user → project (matches original stub behaviour)

    static const std::filesystem::path SYSTEM_CONFIG = "/etc/claw/config.json";
    {
        std::error_code ec;
        if (std::filesystem::exists(SYSTEM_CONFIG, ec)) {
            if (auto cfg = load_file(SYSTEM_CONFIG)) {
                cfg->primary_source = ConfigSource::SystemFile;
                merge(merged, *cfg);
            }
        }
    }

    if (const char* home = std::getenv("HOME"); home && *home) {
        auto user_config = std::filesystem::path(home) / ".claw" / "config.json";
        std::error_code ec;
        if (std::filesystem::exists(user_config, ec)) {
            if (auto cfg = load_file(user_config)) {
                cfg->primary_source = ConfigSource::UserFile;
                merge(merged, *cfg);
            }
        }
    }

    // Walk up from cwd looking for .claw/config.json
    auto dir = cwd;
    while (true) {
        auto project_config = dir / ".claw" / "config.json";
        std::error_code ec;
        if (std::filesystem::exists(project_config, ec)) {
            if (auto cfg = load_file(project_config)) {
                cfg->primary_source = ConfigSource::ProjectFile;
                merge(merged, *cfg);
                break;
            }
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }

    // Environment variable overrides
    if (const char* key = std::getenv("ANTHROPIC_API_KEY"); key && *key) {
        merged.api_key        = key;
        merged.primary_source = ConfigSource::EnvVar;
    }
    if (const char* url = std::getenv("ANTHROPIC_BASE_URL"); url && *url) {
        merged.base_url = url;
    }
    if (const char* model = std::getenv("CLAUDE_MODEL"); model && *model) {
        merged.model = model;
    }

    return merged;
}

} // namespace claw::runtime
