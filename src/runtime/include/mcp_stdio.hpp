#pragma once
// mcp_stdio.hpp — C++20 faithful conversion of mcp_stdio.rs
// Namespace: runtime  (matching existing codebase convention)

#include <string>
#include <vector>
#include <optional>
#include <map>
#include <variant>
#include <tl/expected.hpp>
#include <memory>
#include <cstdint>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sys/types.h>  // pid_t
#endif

namespace claw::runtime {

// ─── JSON-RPC id types (Rust: enum JsonRpcId { Number(u64), String(String), Null }) ────

struct JsonRpcIdNumber { int64_t value; };
struct JsonRpcIdString { std::string value; };
struct JsonRpcIdNull   {};

using JsonRpcId = std::variant<JsonRpcIdNumber, JsonRpcIdString, JsonRpcIdNull>;

[[nodiscard]] nlohmann::json jsonrpc_id_to_json(const JsonRpcId& id);
[[nodiscard]] JsonRpcId      jsonrpc_id_from_json(const nlohmann::json& j);
[[nodiscard]] bool           jsonrpc_ids_equal(const JsonRpcId& a, const JsonRpcId& b);
[[nodiscard]] std::string    jsonrpc_id_debug(const JsonRpcId& id);

// ─── JSON-RPC message types ───────────────────────────────────────────────────

struct JsonRpcRequest {
    std::string    jsonrpc{"2.0"};
    JsonRpcId      id;
    std::string    method;
    nlohmann::json params;  // null → omit from serialization
};

struct JsonRpcError {
    int         code{-32000};
    std::string message;
    nlohmann::json data;
};

struct JsonRpcResponse {
    std::string            jsonrpc{"2.0"};
    JsonRpcId              id;
    nlohmann::json         result;   // null → missing
    std::optional<JsonRpcError> error;
};

[[nodiscard]] nlohmann::json  jsonrpc_request_to_json(const JsonRpcRequest& req);
[[nodiscard]] JsonRpcResponse jsonrpc_response_from_json(const nlohmann::json& j);

// ─── MCP protocol types (Rust structs, camelCase serialization) ───────────────

struct McpClientInfo {
    std::string name;
    std::string version;
};

struct McpServerInfo {
    std::string name;
    std::string version;
};

struct McpInitializeParams {
    std::string    protocol_version{"2025-03-26"};
    McpClientInfo  client_info;
    nlohmann::json capabilities;  // default: {}
};

struct McpInitializeResult {
    std::string    protocol_version;
    nlohmann::json capabilities;
    McpServerInfo  server_info;
};

// Rust: McpListToolsParams { cursor: Option<String> }
struct McpListToolsParams {
    std::optional<std::string> cursor;
};

struct McpToolAnnotations {
    std::optional<bool> read_only_hint;
    std::optional<bool> destructive_hint;
    std::optional<bool> idempotent_hint;
    std::optional<bool> open_world_hint;
};

// Rust: McpTool { name, description, input_schema, annotations, meta }
struct McpTool {
    std::string                    name;
    std::optional<std::string>     description;
    nlohmann::json                 input_schema;  // JSON Schema object
    std::optional<McpToolAnnotations> annotations;
    nlohmann::json                 meta;          // _meta field
};

// Rust: McpListToolsResult { tools, next_cursor }
struct McpListToolsResult {
    std::vector<McpTool>       tools;
    std::optional<std::string> next_cursor;
};

// Rust: McpToolCallParams { name, arguments, meta }
struct McpToolCallParams {
    std::string    name;
    nlohmann::json arguments;  // null → omit
    nlohmann::json meta;       // _meta field
};

// Rust: McpToolCallContent { kind (type), data (flatten BTreeMap<String, JsonValue>) }
struct McpToolCallContent {
    std::string kind;  // "text", "image", "resource"
    // Rust: #[serde(flatten)] pub data: BTreeMap<String, JsonValue>
    std::map<std::string, nlohmann::json> data;
};

// Rust: McpToolCallResult { content, structured_content, is_error, meta }
struct McpToolCallResult {
    std::vector<McpToolCallContent> content;
    nlohmann::json                  structured_content;  // null → missing
    std::optional<bool>             is_error;
    nlohmann::json                  meta;
};

// Rust: McpListResourcesParams { cursor }
struct McpListResourcesParams {
    std::optional<std::string> cursor;
};

// Rust: McpResource { uri, name, description, mime_type, annotations, meta }
struct McpResource {
    std::string                uri;
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
    nlohmann::json             annotations;
    nlohmann::json             meta;
};

// Rust: McpListResourcesResult { resources, next_cursor }
struct McpListResourcesResult {
    std::vector<McpResource>   resources;
    std::optional<std::string> next_cursor;
};

// Rust: McpReadResourceParams { uri }
struct McpReadResourceParams {
    std::string uri;
};

// Rust: McpResourceContents { uri, mime_type, text, blob, meta }
struct McpResourceContents {
    std::string                uri;
    std::optional<std::string> mime_type;
    std::optional<std::string> text;
    std::optional<std::string> blob;  // base64
    nlohmann::json             meta;
};

// Rust: McpReadResourceResult { contents }
struct McpReadResourceResult {
    std::vector<McpResourceContents> contents;
};

// ─── Discovery / routing types ────────────────────────────────────────────────

// Rust: ManagedMcpTool { server_name, qualified_name, raw_name, tool }
struct ManagedMcpTool {
    std::string server_name;
    std::string qualified_name;  // mcp_tool_name(server, raw)
    std::string raw_name;
    McpTool     tool;
};

// Rust: UnsupportedMcpServer { server_name, transport, reason }
struct UnsupportedMcpServer {
    std::string name;        // server_name in Rust
    std::string reason;
};

// Rust: McpDiscoveryFailure { server_name, error }
struct McpDiscoveryFailure {
    std::string server_name;
    std::string error;
};

// Rust: McpToolDiscoveryReport { tools, failed_servers, unsupported_servers }
struct McpToolDiscoveryReport {
    std::vector<ManagedMcpTool>     tools;
    std::vector<McpDiscoveryFailure> failures;           // failed_servers in Rust
    std::vector<UnsupportedMcpServer> unsupported_servers;
};

// ─── McpServerManagerError — rich variant matching Rust enum ─────────────────
//
// Rust enum McpServerManagerError:
//   Io(io::Error)
//   Transport { server_name, method, source }
//   JsonRpc { server_name, method, error }
//   InvalidResponse { server_name, method, details }
//   Timeout { server_name, method, timeout_ms }
//   UnknownTool { qualified_name }
//   UnknownServer { server_name }

struct McpServerManagerError {
    // ── Variant inner types ──
    struct IoVariant              { std::string message; };
    struct TransportVariant       { std::string server_name; const char* method; std::string source_message; };
    struct JsonRpcVariant         { std::string server_name; const char* method; JsonRpcError error; };
    struct InvalidResponseVariant { std::string server_name; const char* method; std::string details; };
    struct TimeoutVariant         { std::string server_name; const char* method; uint64_t timeout_ms; };
    struct UnknownToolVariant     { std::string qualified_name; };
    struct UnknownServerVariant   { std::string server_name; };

    using Variant = std::variant<
        IoVariant,
        TransportVariant,
        JsonRpcVariant,
        InvalidResponseVariant,
        TimeoutVariant,
        UnknownToolVariant,
        UnknownServerVariant
    >;

    Variant variant{IoVariant{"uninitialized error"}};

    // Backward-compatible field: pre-computed human-readable error string.
    // Populated by all factory functions so that .message field access works
    // (e.g. result.error().message as used by existing callers).
    std::string message{"uninitialized error"};

    // ── Factory functions (mirrors Rust constructors) ──
    [[nodiscard]] static McpServerManagerError make_io(std::string msg);
    [[nodiscard]] static McpServerManagerError make_transport(
        std::string server_name, const char* method, std::string source_msg);
    [[nodiscard]] static McpServerManagerError make_jsonrpc(
        std::string server_name, const char* method, JsonRpcError error);
    [[nodiscard]] static McpServerManagerError make_invalid_response(
        std::string server_name, const char* method, std::string details);
    [[nodiscard]] static McpServerManagerError make_timeout(
        std::string server_name, const char* method, uint64_t timeout_ms);
    [[nodiscard]] static McpServerManagerError make_unknown_tool(std::string qualified_name);
    [[nodiscard]] static McpServerManagerError make_unknown_server(std::string server_name);

    // Display (mirrors Rust impl Display for McpServerManagerError)
    // Returns the pre-computed .message field (same as to_string()).
    [[nodiscard]] std::string to_string() const { return message; }

    // Type checks
    [[nodiscard]] bool is_transport()        const;
    [[nodiscard]] bool is_timeout()          const;
    [[nodiscard]] bool is_invalid_response() const;
    [[nodiscard]] bool is_io()               const;
};

// ─── Content-Length framing (free-function versions for POSIX fd) ─────────────

[[nodiscard]] tl::expected<void,          std::string> write_frame(int fd, const nlohmann::json& msg);
[[nodiscard]] tl::expected<nlohmann::json, std::string> read_frame(int fd);

// ─── McpStdioProcess ──────────────────────────────────────────────────────────
//
// Mirrors Rust struct McpStdioProcess { child, stdin, stdout: BufReader<ChildStdout> }
// with all pub async fn methods converted to synchronous expected<T, string>.

class McpStdioProcess {
public:
    McpStdioProcess() = default;
    ~McpStdioProcess();

    // Non-copyable; movable (move leaves source in a safely-destructible state)
    McpStdioProcess(const McpStdioProcess&)            = delete;
    McpStdioProcess& operator=(const McpStdioProcess&) = delete;
    McpStdioProcess(McpStdioProcess&&)                 noexcept;
    McpStdioProcess& operator=(McpStdioProcess&&)      noexcept;

    // ── Spawn ──
    // Mirrors Rust McpStdioProcess::spawn(transport: &McpStdioTransport)
    [[nodiscard]] static tl::expected<McpStdioProcess, std::string>
        spawn(const std::string& command,
              const std::vector<std::string>& args,
              const std::map<std::string, std::string>& env_extras);

    // ── Raw I/O (mirrors Rust write_all, flush, write_line, read_line, read_available) ──
    [[nodiscard]] tl::expected<void, std::string>
        write_all(const uint8_t* data, std::size_t len);

    [[nodiscard]] tl::expected<void, std::string> flush();

    [[nodiscard]] tl::expected<void, std::string>
        write_line(const std::string& line);

    // timeout_ms == 0 → no timeout (block forever)
    [[nodiscard]] tl::expected<std::string, std::string>
        read_line(uint64_t timeout_ms = 0);

    [[nodiscard]] tl::expected<std::vector<uint8_t>, std::string>
        read_available();

    // ── Content-Length framing ──
    // Mirrors Rust write_frame(payload: &[u8]) / read_frame() -> Vec<u8>
    [[nodiscard]] tl::expected<void, std::string>
        write_frame(const std::vector<uint8_t>& payload);

    [[nodiscard]] tl::expected<std::vector<uint8_t>, std::string>
        read_frame(uint64_t timeout_ms = 0);

    // ── JSON-RPC message I/O ──
    // Mirrors Rust write_jsonrpc_message<T: Serialize> / read_jsonrpc_message<T>
    [[nodiscard]] tl::expected<void, std::string>
        write_jsonrpc_message(const nlohmann::json& msg);

    [[nodiscard]] tl::expected<nlohmann::json, std::string>
        read_jsonrpc_message(uint64_t timeout_ms = 0);

    // Mirrors Rust send_request<T: Serialize>(request: &JsonRpcRequest<T>)
    [[nodiscard]] tl::expected<void, std::string>
        send_request(const nlohmann::json& request_json);

    // Mirrors Rust read_response<T: DeserializeOwned>() -> JsonRpcResponse<T>
    [[nodiscard]] tl::expected<JsonRpcResponse, std::string>
        read_response(uint64_t timeout_ms = 0);

    // Mirrors Rust request<TParams, TResult>(id, method, params) -> JsonRpcResponse<TResult>
    // Validates jsonrpc version and id match; returns InvalidData on mismatch.
    [[nodiscard]] tl::expected<JsonRpcResponse, std::string>
        request(const JsonRpcId& id,
                const std::string& method,
                const nlohmann::json& params,
                uint64_t timeout_ms = 0);

    // ── Typed MCP methods (mirrors Rust pub async fn) ──

    // Mirrors Rust initialize(id, params) -> JsonRpcResponse<McpInitializeResult>
    [[nodiscard]] tl::expected<JsonRpcResponse, std::string>
        initialize(const JsonRpcId& id,
                   const McpInitializeParams& params,
                   uint64_t timeout_ms = 0);

    // Mirrors Rust list_tools(id, params) -> JsonRpcResponse<McpListToolsResult>
    [[nodiscard]] tl::expected<JsonRpcResponse, std::string>
        list_tools(const JsonRpcId& id,
                   const std::optional<std::string>& cursor = std::nullopt,
                   uint64_t timeout_ms = 0);

    // Mirrors Rust call_tool(id, params) -> JsonRpcResponse<McpToolCallResult>
    [[nodiscard]] tl::expected<JsonRpcResponse, std::string>
        call_tool(const JsonRpcId& id,
                  const std::string& name,
                  const nlohmann::json& arguments,
                  uint64_t timeout_ms = 0);

    // Mirrors Rust list_resources(id, params) -> JsonRpcResponse<McpListResourcesResult>
    [[nodiscard]] tl::expected<JsonRpcResponse, std::string>
        list_resources(const JsonRpcId& id,
                       const std::optional<std::string>& cursor = std::nullopt,
                       uint64_t timeout_ms = 0);

    // Mirrors Rust read_resource(id, params) -> JsonRpcResponse<McpReadResourceResult>
    [[nodiscard]] tl::expected<JsonRpcResponse, std::string>
        read_resource(const JsonRpcId& id,
                      const std::string& uri,
                      uint64_t timeout_ms = 0);

    // ── Process lifecycle ──
    // Mirrors Rust terminate() -> io::Result<()>
    [[nodiscard]] tl::expected<void, std::string> terminate();

    // Mirrors Rust wait() -> io::Result<ExitStatus>  (returns exit code)
    [[nodiscard]] tl::expected<int, std::string> wait_for_exit();

    // Mirrors Rust has_exited() -> io::Result<bool>
    [[nodiscard]] tl::expected<bool, std::string> has_exited();

    // Mirrors Rust async fn shutdown(&mut self) -> io::Result<()>
    // Kills if running, waits for exit.
    [[nodiscard]] tl::expected<void, std::string> shutdown();

    // Legacy alias
    void kill_and_wait();

private:
#ifdef _WIN32
public:
    struct Impl;
    std::unique_ptr<Impl> impl_;
#else
    int     stdin_fd_{-1};
    int     stdout_fd_{-1};
    pid_t   pid_{-1};

    // Internal read buffer (mirrors Rust BufReader)
    std::vector<uint8_t> read_buf_{std::vector<uint8_t>(8192)};
    std::size_t          read_pos_{0};
    std::size_t          read_end_{0};
#endif
};

// ─── Default initialize params ────────────────────────────────────────────────
[[nodiscard]] McpInitializeParams default_initialize_params();

// ─── McpServerManager ─────────────────────────────────────────────────────────
//
// Mirrors Rust struct McpServerManager {
//     servers: BTreeMap<String, ManagedMcpServer>,
//     unsupported_servers: Vec<UnsupportedMcpServer>,
//     tool_index: BTreeMap<String, ToolRoute>,
//     next_request_id: u64,
// }
//
// Processes are spawned lazily on first use and reset on error (retry logic).

// Forward-declare the internal state struct (defined in .cpp)
struct McpServerManagerState;

class McpServerManager {
public:
    // Server config (mirrors ScopedMcpServerConfig / McpStdioTransport in Rust)
    struct ServerConfig {
        std::string                        name;
        std::string                        command;
        std::vector<std::string>           args;
        std::map<std::string, std::string> env;
        uint32_t                           tool_call_timeout_ms{60'000};
    };

    McpServerManager();
    ~McpServerManager();
    McpServerManager(McpServerManager&&);
    McpServerManager& operator=(McpServerManager&&);
    McpServerManager(const McpServerManager&)            = delete;
    McpServerManager& operator=(const McpServerManager&) = delete;

    // ── Construction ──

    // Mirrors Rust from_servers(servers: &BTreeMap<String, ScopedMcpServerConfig>) -> Self
    // (all configs are assumed to be stdio; non-stdio callers push to unsupported manually)
    [[nodiscard]] static tl::expected<std::shared_ptr<McpServerManager>, McpServerManagerError>
        from_servers(std::vector<ServerConfig> configs);

    // ── Accessors ──

    // Mirrors Rust unsupported_servers(&self) -> &[UnsupportedMcpServer]
    [[nodiscard]] const std::vector<UnsupportedMcpServer>& unsupported_servers() const;

    // Mirrors Rust server_names(&self) -> Vec<String>
    [[nodiscard]] std::vector<std::string> server_names() const;

    // ── Discovery ──

    // Mirrors Rust pub async fn discover_tools(&mut self) -> Result<Vec<ManagedMcpTool>, ...>
    // Fails on first server error.
    [[nodiscard]] tl::expected<std::vector<ManagedMcpTool>, McpServerManagerError>
        discover_tools();

    // Mirrors Rust pub async fn discover_tools_best_effort(&mut self) -> McpToolDiscoveryReport
    // Never fails — errors are collected in McpToolDiscoveryReport::failures.
    [[nodiscard]] McpToolDiscoveryReport discover_tools_best_effort();

    // Legacy: returns wrapped report (kept for backward compatibility)
    [[nodiscard]] tl::expected<McpToolDiscoveryReport, McpServerManagerError>
        discover_tools_report();

    // ── Tool calls ──

    // Mirrors Rust pub async fn call_tool(qualified_tool_name, arguments)
    //   -> Result<JsonRpcResponse<McpToolCallResult>, McpServerManagerError>
    //
    // Returns unpacked McpToolCallResult (JSON-RPC errors → McpServerManagerError::JsonRpc).
    // Use call_tool_rpc() to get the raw JsonRpcResponse including protocol-level errors.
    [[nodiscard]] tl::expected<McpToolCallResult, McpServerManagerError>
        call_tool(const std::string& qualified_tool_name,
                  const nlohmann::json& arguments);

    // Returns the full JsonRpcResponse (faithfully mirrors Rust return type).
    [[nodiscard]] tl::expected<JsonRpcResponse, McpServerManagerError>
        call_tool_rpc(const std::string& qualified_tool_name,
                      const nlohmann::json& arguments);

    // ── Resources ──

    // Mirrors Rust pub async fn list_resources(&mut self, server_name) -> Result<McpListResourcesResult, ...>
    [[nodiscard]] tl::expected<McpListResourcesResult, McpServerManagerError>
        list_resources(const std::string& server_name);

    // Mirrors Rust pub async fn read_resource(&mut self, server_name, uri) -> Result<McpReadResourceResult, ...>
    [[nodiscard]] tl::expected<McpReadResourceResult, McpServerManagerError>
        read_resource(const std::string& server_name, const std::string& uri);

    // ── Shutdown ──

    // Mirrors Rust pub async fn shutdown(&mut self) -> Result<(), McpServerManagerError>
    // Note: not [[nodiscard]] to allow callers to ignore the result.
    tl::expected<void, McpServerManagerError> shutdown();

private:
    std::unique_ptr<McpServerManagerState> state_;
};

} // namespace claw::runtime
