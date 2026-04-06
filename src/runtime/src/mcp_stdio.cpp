// mcp_stdio.cpp — Full C++20 faithful conversion of mcp_stdio.rs
// Namespace: runtime  (kept as runtime:: to match existing headers)
//
// Key design notes:
//  - tokio async → synchronous blocking I/O (fits single-threaded manager use pattern)
//  - McpServerManagerError → rich variant-style error via std::variant
//  - Timeout: uses POSIX select/poll on POSIX, WaitForSingleObject on Windows
//  - Process spawn: POSIX fork/exec on POSIX, CreateProcess on Windows
//  - Content-Length framing is identical to Rust encode_frame / read_frame
//  - Tool routing uses an std::map<string, ToolRoute> (BTreeMap equivalent)
//  - Lazy server initialization: servers are spawned on first use, reset on error

#include "mcp_stdio.hpp"
#include "mcp.hpp"
#include "win32_arg_escape.hpp"
#include <format>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <stdexcept>
#include <map>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <sys/select.h>
#  include <fcntl.h>
#  include <signal.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Timeout constants (mirror Rust cfg(test) / cfg(not(test)))
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint64_t MCP_INITIALIZE_TIMEOUT_MS = 10'000;
static constexpr uint64_t MCP_LIST_TOOLS_TIMEOUT_MS  = 30'000;

namespace claw::runtime {

// ─────────────────────────────────────────────────────────────────────────────
// §1  McpServerManagerError — rich variant matching Rust enum
//     Each factory function populates both .variant and .message so that
//     callers can use either .message (field) or .to_string() / to_string().
// ─────────────────────────────────────────────────────────────────────────────

McpServerManagerError McpServerManagerError::make_io(std::string msg) {
    McpServerManagerError e;
    e.variant = IoVariant{ msg };
    e.message = std::move(msg);
    return e;
}

McpServerManagerError McpServerManagerError::make_transport(
        std::string server_name, const char* method, std::string source_msg) {
    McpServerManagerError e;
    e.message = std::format("MCP server `{}` transport failed during {}: {}",
        server_name, method, source_msg);
    e.variant = TransportVariant{ std::move(server_name), method, std::move(source_msg) };
    return e;
}

McpServerManagerError McpServerManagerError::make_jsonrpc(
        std::string server_name, const char* method, JsonRpcError error) {
    McpServerManagerError e;
    e.message = std::format(
        "MCP server `{}` returned JSON-RPC error for {}: {} ({})",
        server_name, method, error.message, error.code);
    e.variant = JsonRpcVariant{ std::move(server_name), method, std::move(error) };
    return e;
}

McpServerManagerError McpServerManagerError::make_invalid_response(
        std::string server_name, const char* method, std::string details) {
    McpServerManagerError e;
    e.message = std::format(
        "MCP server `{}` returned invalid response for {}: {}",
        server_name, method, details);
    e.variant = InvalidResponseVariant{ std::move(server_name), method, std::move(details) };
    return e;
}

McpServerManagerError McpServerManagerError::make_timeout(
        std::string server_name, const char* method, uint64_t timeout_ms) {
    McpServerManagerError e;
    e.message = std::format(
        "MCP server `{}` timed out after {} ms while handling {}",
        server_name, timeout_ms, method);
    e.variant = TimeoutVariant{ std::move(server_name), method, timeout_ms };
    return e;
}

McpServerManagerError McpServerManagerError::make_unknown_tool(std::string qualified_name) {
    McpServerManagerError e;
    e.message = std::format("unknown MCP tool `{}`", qualified_name);
    e.variant = UnknownToolVariant{ std::move(qualified_name) };
    return e;
}

McpServerManagerError McpServerManagerError::make_unknown_server(std::string server_name) {
    McpServerManagerError e;
    e.message = std::format("unknown MCP server `{}`", server_name);
    e.variant = UnknownServerVariant{ std::move(server_name) };
    return e;
}

// to_string() is defined inline in the header as { return message; }

bool McpServerManagerError::is_transport() const {
    return std::holds_alternative<TransportVariant>(variant);
}
bool McpServerManagerError::is_timeout() const {
    return std::holds_alternative<TimeoutVariant>(variant);
}
bool McpServerManagerError::is_invalid_response() const {
    return std::holds_alternative<InvalidResponseVariant>(variant);
}
bool McpServerManagerError::is_io() const {
    return std::holds_alternative<IoVariant>(variant);
}

// ── lifecycle integration helpers (mirrors Rust McpServerManagerError methods) ──

static McpLifecyclePhase lifecycle_phase_for_method(const char* method) {
    std::string_view m(method);
    if (m == "initialize")     return McpLifecyclePhase::InitializeHandshake;
    if (m == "tools/list")     return McpLifecyclePhase::ToolDiscovery;
    if (m == "resources/list") return McpLifecyclePhase::ResourceDiscovery;
    if (m == "resources/read" || m == "tools/call") return McpLifecyclePhase::Invocation;
    return McpLifecyclePhase::ErrorSurfacing;
}

McpLifecyclePhase McpServerManagerError::lifecycle_phase() const {
    return std::visit([](const auto& v) -> McpLifecyclePhase {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, IoVariant>) {
            return McpLifecyclePhase::SpawnConnect;
        } else if constexpr (std::is_same_v<T, TransportVariant>
                          || std::is_same_v<T, JsonRpcVariant>
                          || std::is_same_v<T, InvalidResponseVariant>
                          || std::is_same_v<T, TimeoutVariant>) {
            return lifecycle_phase_for_method(v.method);
        } else if constexpr (std::is_same_v<T, UnknownToolVariant>) {
            return McpLifecyclePhase::ToolDiscovery;
        } else if constexpr (std::is_same_v<T, UnknownServerVariant>) {
            return McpLifecyclePhase::ServerRegistration;
        }
        return McpLifecyclePhase::ErrorSurfacing;
    }, variant);
}

bool McpServerManagerError::recoverable() const {
    auto phase = lifecycle_phase();
    if (phase == McpLifecyclePhase::InitializeHandshake) return false;
    return is_transport() || is_timeout();
}

std::map<std::string, std::string> McpServerManagerError::error_context() const {
    return std::visit([](const auto& v) -> std::map<std::string, std::string> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, IoVariant>) {
            return {{"kind", v.message}};
        } else if constexpr (std::is_same_v<T, TransportVariant>) {
            return {{"server", v.server_name}, {"method", v.method}, {"io_kind", v.source_message}};
        } else if constexpr (std::is_same_v<T, JsonRpcVariant>) {
            return {{"server", v.server_name}, {"method", v.method},
                    {"jsonrpc_code", std::to_string(v.error.code)}};
        } else if constexpr (std::is_same_v<T, InvalidResponseVariant>) {
            return {{"server", v.server_name}, {"method", v.method}, {"details", v.details}};
        } else if constexpr (std::is_same_v<T, TimeoutVariant>) {
            return {{"server", v.server_name}, {"method", v.method},
                    {"timeout_ms", std::to_string(v.timeout_ms)}};
        } else if constexpr (std::is_same_v<T, UnknownToolVariant>) {
            return {{"qualified_tool", v.qualified_name}};
        } else if constexpr (std::is_same_v<T, UnknownServerVariant>) {
            return {{"server", v.server_name}};
        }
        return {};
    }, variant);
}

McpDiscoveryFailure McpServerManagerError::discovery_failure(const std::string& server_name) const {
    return McpDiscoveryFailure{
        server_name,
        lifecycle_phase(),
        to_string(),
        recoverable(),
        error_context(),
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  JSON-RPC id helpers
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json jsonrpc_id_to_json(const JsonRpcId& id) {
    return std::visit([](const auto& v) -> nlohmann::json {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, JsonRpcIdNumber>) return v.value;
        else if constexpr (std::is_same_v<T, JsonRpcIdString>) return v.value;
        else return nullptr;
    }, id);
}

JsonRpcId jsonrpc_id_from_json(const nlohmann::json& j) {
    if (j.is_number_integer())   return JsonRpcIdNumber{ j.get<int64_t>() };
    if (j.is_number_unsigned())  return JsonRpcIdNumber{ static_cast<int64_t>(j.get<uint64_t>()) };
    if (j.is_string())           return JsonRpcIdString{ j.get<std::string>() };
    return JsonRpcIdNull{};
}

bool jsonrpc_ids_equal(const JsonRpcId& a, const JsonRpcId& b) {
    return std::visit([&b](const auto& av) -> bool {
        return std::visit([&av](const auto& bv) -> bool {
            using A = std::decay_t<decltype(av)>;
            using B = std::decay_t<decltype(bv)>;
            if constexpr (std::is_same_v<A, B>) {
                if constexpr (std::is_same_v<A, JsonRpcIdNull>) return true;
                else return av.value == bv.value;
            }
            return false;
        }, b);
    }, a);
}

std::string jsonrpc_id_debug(const JsonRpcId& id) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, JsonRpcIdNumber>)
            return std::format("Number({})", v.value);
        else if constexpr (std::is_same_v<T, JsonRpcIdString>)
            return std::format("String(\"{}\")", v.value);
        else return "Null";
    }, id);
}

nlohmann::json jsonrpc_request_to_json(const JsonRpcRequest& req) {
    nlohmann::json j = {
        {"jsonrpc", req.jsonrpc},
        {"id",      jsonrpc_id_to_json(req.id)},
        {"method",  req.method},
    };
    if (!req.params.is_null()) j["params"] = req.params;
    return j;
}

JsonRpcResponse jsonrpc_response_from_json(const nlohmann::json& j) {
    JsonRpcResponse resp;
    resp.jsonrpc = j.value("jsonrpc", std::string{"2.0"});
    if (j.contains("id"))     resp.id     = jsonrpc_id_from_json(j["id"]);
    if (j.contains("result")) resp.result = j["result"];
    if (j.contains("error")) {
        const auto& e = j["error"];
        resp.error = JsonRpcError{
            e.value("code",    -32000),
            e.value("message", std::string{"unknown error"}),
            e.contains("data") ? e["data"] : nlohmann::json{},
        };
    }
    return resp;
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  encode_frame / decode_frame (Content-Length framing)
// ─────────────────────────────────────────────────────────────────────────────

// Mirrors Rust fn encode_frame(payload: &[u8]) -> Vec<u8>
static std::vector<uint8_t> encode_frame(const std::vector<uint8_t>& payload) {
    std::string header = std::format("Content-Length: {}\r\n\r\n", payload.size());
    std::vector<uint8_t> framed(header.begin(), header.end());
    framed.insert(framed.end(), payload.begin(), payload.end());
    return framed;
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  select()-based wait helper (POSIX only, used by read_line / read_frame)
//     Returns true if the fd is readable within timeout_ms, false on timeout.
//     timeout_ms == 0 means no timeout (caller does plain ::read).
// ─────────────────────────────────────────────────────────────────────────────

#ifndef _WIN32
static bool posix_wait_readable(int fd, uint64_t timeout_ms) {
    if (timeout_ms == 0) return true; // no timeout — caller blocks on read()
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec  = static_cast<long>(timeout_ms / 1000);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
    return ::select(fd + 1, &rfds, nullptr, nullptr, &tv) > 0;
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// §5  McpStdioProcess — low-level process I/O (POSIX / Windows)
// ─────────────────────────────────────────────────────────────────────────────

#ifdef _WIN32

struct McpStdioProcess::Impl {
    HANDLE child_handle{INVALID_HANDLE_VALUE};
    HANDLE stdin_write{INVALID_HANDLE_VALUE};
    HANDLE stdout_read{INVALID_HANDLE_VALUE};
    // buffered reader for stdout
    std::vector<uint8_t> read_buf;
    std::size_t read_pos{0};
    std::size_t read_end{0};
};

static tl::expected<McpStdioProcess, std::string>
spawn_process_win32(const std::string& command,
                    const std::vector<std::string>& args,
                    const std::map<std::string,std::string>& env_extras)
{
    // Create pipes
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

    HANDLE stdin_read{INVALID_HANDLE_VALUE},  stdin_write{INVALID_HANDLE_VALUE};
    HANDLE stdout_read{INVALID_HANDLE_VALUE}, stdout_write{INVALID_HANDLE_VALUE};

    if (!CreatePipe(&stdin_read,  &stdin_write,  &sa, 0) ||
        !CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        return tl::unexpected(std::format("CreatePipe failed: {}", GetLastError()));
    }
    // Make parent ends non-inheritable
    SetHandleInformation(stdin_write,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read,  HANDLE_FLAG_INHERIT, 0);

    // Build command line
    std::string cmdline = claw::util::escape_win32_arg(command);
    for (const auto& a : args) {
        cmdline += " ";
        cmdline += claw::util::escape_win32_arg(a);
    }

    // Build environment block (null-separated, double-null terminated)
    // Start from current environment, apply extras on top
    std::vector<char> env_block;
    {
        LPCH cur_env = GetEnvironmentStrings();
        // copy existing env
        std::map<std::string, std::string> full_env;
        if (cur_env) {
            LPCH p = cur_env;
            while (*p) {
                std::string entry(p);
                auto eq = entry.find('=');
                if (eq != std::string::npos && eq > 0)
                    full_env[entry.substr(0, eq)] = entry.substr(eq + 1);
                p += strlen(p) + 1;
            }
            FreeEnvironmentStrings(cur_env);
        }
        for (const auto& [k, v] : env_extras) full_env[k] = v;
        for (const auto& [k, v] : full_env) {
            std::string entry = k + "=" + v;
            env_block.insert(env_block.end(), entry.begin(), entry.end());
            env_block.push_back('\0');
        }
        env_block.push_back('\0');
    }

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.hStdInput   = stdin_read;
    si.hStdOutput  = stdout_write;
    si.hStdError   = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags     = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(
        nullptr,
        const_cast<LPSTR>(cmdline.c_str()),
        nullptr, nullptr,
        TRUE,  // inherit handles
        0,
        env_block.data(),
        nullptr,
        &si, &pi);

    // Close child-side handles in parent
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);

    if (!ok) {
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        return tl::unexpected(std::format("CreateProcess failed: {}", GetLastError()));
    }
    CloseHandle(pi.hThread);

    auto impl = std::make_unique<McpStdioProcess::Impl>();
    impl->child_handle = pi.hProcess;
    impl->stdin_write  = stdin_write;
    impl->stdout_read  = stdout_read;
    impl->read_buf.resize(8192);

    McpStdioProcess proc;
    proc.impl_ = std::move(impl);
    return proc;
}

// Timed read on Windows using OVERLAPPED or a short polling loop
static bool win32_timed_read_byte(HANDLE h, uint8_t& out, uint64_t timeout_ms) {
    DWORD available = 0;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : INFINITE);
    while (true) {
        PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr);
        if (available > 0) {
            DWORD read = 0;
            ReadFile(h, &out, 1, &read, nullptr);
            return read == 1;
        }
        if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) return false;
        Sleep(1);
    }
}

#endif // _WIN32 (Impl + spawn_process_win32 + win32_timed_read_byte)

// ─────────────────────────────────────────────────────────────────────────────
// McpStdioProcess method implementations — split by platform
// ─────────────────────────────────────────────────────────────────────────────

#ifdef _WIN32

// ── Windows: move operations ──────────────────────────────────────────────────

McpStdioProcess::McpStdioProcess(McpStdioProcess&& o) noexcept
    : impl_(std::move(o.impl_))
{}

McpStdioProcess& McpStdioProcess::operator=(McpStdioProcess&& o) noexcept {
    if (this != &o) {
        (void)shutdown();
        impl_ = std::move(o.impl_);
    }
    return *this;
}

// ── Windows: wire McpStdioProcess::spawn to spawn_process_win32 ──────────────

tl::expected<McpStdioProcess, std::string>
McpStdioProcess::spawn(const std::string& command,
                       const std::vector<std::string>& args,
                       const std::map<std::string, std::string>& env_extras)
{
    return spawn_process_win32(command, args, env_extras);
}

tl::expected<void, std::string>
McpStdioProcess::write_all(const uint8_t* data, std::size_t len) {
    if (!impl_ || impl_->stdin_write == INVALID_HANDLE_VALUE)
        return tl::unexpected("process not running");
    while (len > 0) {
        DWORD written = 0;
        if (!WriteFile(impl_->stdin_write, data, static_cast<DWORD>(len), &written, nullptr))
            return tl::unexpected(std::format("WriteFile: {}", GetLastError()));
        data += written;
        len  -= written;
    }
    return {};
}

tl::expected<void, std::string> McpStdioProcess::flush() { return {}; }

tl::expected<void, std::string> McpStdioProcess::write_line(const std::string& line) {
    auto r = write_all(reinterpret_cast<const uint8_t*>(line.data()), line.size());
    if (!r) return r;
    uint8_t nl = '\n';
    return write_all(&nl, 1);
}

tl::expected<std::string, std::string>
McpStdioProcess::read_line(uint64_t timeout_ms) {
    if (!impl_ || impl_->stdout_read == INVALID_HANDLE_VALUE)
        return tl::unexpected("process not running");
    std::string line;
    while (true) {
        if (impl_->read_pos < impl_->read_end) {
            char c = static_cast<char>(impl_->read_buf[impl_->read_pos++]);
            line += c;
            if (c == '\n') return line;
            continue;
        }
        uint8_t dummy;
        if (!win32_timed_read_byte(impl_->stdout_read, dummy, timeout_ms))
            return tl::unexpected("timeout or EOF reading line");
        line += static_cast<char>(dummy);
        if (dummy == '\n') return line;
    }
}

tl::expected<std::vector<uint8_t>, std::string>
McpStdioProcess::read_available() {
    if (!impl_) return tl::unexpected("process not running");
    std::vector<uint8_t> buf(4096);
    DWORD read = 0;
    if (!ReadFile(impl_->stdout_read, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr))
        return tl::unexpected(std::format("ReadFile: {}", GetLastError()));
    buf.resize(read);
    return buf;
}

tl::expected<std::vector<uint8_t>, std::string>
McpStdioProcess::read_frame(uint64_t timeout_ms) {
    // Header parsing via read_line
    std::optional<std::size_t> content_length;
    while (true) {
        auto lr = read_line(timeout_ms);
        if (!lr) return tl::unexpected(lr.error());
        std::string line = *lr;
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name_lower = line.substr(0, colon);
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            if (name_lower == "content-length") {
                std::string val = line.substr(colon + 1);
                auto vs = val.find_first_not_of(" \t");
                if (vs != std::string::npos) val = val.substr(vs);
                try { content_length = std::stoul(val); } catch (...) {
                    return tl::unexpected("invalid Content-Length");
                }
            }
        }
    }
    if (!content_length) return tl::unexpected("missing Content-Length header");
    std::vector<uint8_t> payload(*content_length, 0);
    std::size_t offset = 0;
    while (offset < *content_length) {
        DWORD to_read = static_cast<DWORD>(*content_length - offset);
        DWORD read = 0;
        if (!ReadFile(impl_->stdout_read, payload.data() + offset, to_read, &read, nullptr))
            return tl::unexpected("ReadFile error reading body");
        if (read == 0) return tl::unexpected("EOF reading frame body");
        offset += read;
    }
    return payload;
}

tl::expected<void, std::string>
McpStdioProcess::write_frame(const std::vector<uint8_t>& payload) {
    auto framed = encode_frame(payload);
    return write_all(framed.data(), framed.size());
}

tl::expected<void, std::string>
McpStdioProcess::write_jsonrpc_message(const nlohmann::json& msg) {
    std::string body = msg.dump();
    std::vector<uint8_t> payload(body.begin(), body.end());
    return write_frame(payload);
}

tl::expected<nlohmann::json, std::string>
McpStdioProcess::read_jsonrpc_message(uint64_t timeout_ms) {
    auto r = read_frame(timeout_ms);
    if (!r) return tl::unexpected(r.error());
    try { return nlohmann::json::parse(*r); }
    catch (const std::exception& ex) {
        return tl::unexpected(std::format("JSON parse error: {}", ex.what()));
    }
}

tl::expected<void, std::string>
McpStdioProcess::send_request(const nlohmann::json& req) {
    return write_jsonrpc_message(req);
}

tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::read_response(uint64_t timeout_ms) {
    auto j = read_jsonrpc_message(timeout_ms);
    if (!j) return tl::unexpected(j.error());
    return jsonrpc_response_from_json(*j);
}

tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::request(const JsonRpcId& id, const std::string& method,
                          const nlohmann::json& params, uint64_t timeout_ms) {
    nlohmann::json req_json = {
        {"jsonrpc", "2.0"}, {"id", jsonrpc_id_to_json(id)}, {"method", method}
    };
    if (!params.is_null()) req_json["params"] = params;
    { auto r = send_request(req_json); if (!r) return tl::unexpected(r.error()); }
    auto resp_r = read_response(timeout_ms);
    if (!resp_r) return tl::unexpected(resp_r.error());
    if (resp_r->jsonrpc != "2.0")
        return tl::unexpected(std::format("unsupported jsonrpc version: {}", resp_r->jsonrpc));
    if (!jsonrpc_ids_equal(resp_r->id, id))
        return tl::unexpected(std::format("mismatched id: expected {}, got {}",
            jsonrpc_id_debug(id), jsonrpc_id_debug(resp_r->id)));
    return resp_r;
}

tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::initialize(const JsonRpcId& id, const McpInitializeParams& p, uint64_t tms) {
    nlohmann::json j = {
        {"protocolVersion", p.protocol_version},
        {"capabilities", p.capabilities},
        {"clientInfo", {{"name", p.client_info.name}, {"version", p.client_info.version}}}
    };
    return request(id, "initialize", j, tms);
}

tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::list_tools(const JsonRpcId& id, const std::optional<std::string>& cursor, uint64_t tms) {
    nlohmann::json j = nlohmann::json::object();
    if (cursor) j["cursor"] = *cursor;
    return request(id, "tools/list", j, tms);
}

tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::call_tool(const JsonRpcId& id, const std::string& name,
                             const nlohmann::json& arguments, uint64_t tms) {
    nlohmann::json j = {{"name", name}};
    if (!arguments.is_null()) j["arguments"] = arguments;
    return request(id, "tools/call", j, tms);
}

tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::list_resources(const JsonRpcId& id, const std::optional<std::string>& cursor, uint64_t tms) {
    nlohmann::json j = nlohmann::json::object();
    if (cursor) j["cursor"] = *cursor;
    return request(id, "resources/list", j, tms);
}

tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::read_resource(const JsonRpcId& id, const std::string& uri, uint64_t tms) {
    return request(id, "resources/read", {{"uri", uri}}, tms);
}

tl::expected<void, std::string> McpStdioProcess::terminate() {
    if (!impl_ || impl_->child_handle == INVALID_HANDLE_VALUE) return {};
    TerminateProcess(impl_->child_handle, 1);
    return {};
}

tl::expected<int, std::string> McpStdioProcess::wait_for_exit() {
    if (!impl_ || impl_->child_handle == INVALID_HANDLE_VALUE) return 0;
    WaitForSingleObject(impl_->child_handle, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(impl_->child_handle, &code);
    CloseHandle(impl_->child_handle);
    impl_->child_handle = INVALID_HANDLE_VALUE;
    return static_cast<int>(code);
}

tl::expected<bool, std::string> McpStdioProcess::has_exited() {
    if (!impl_ || impl_->child_handle == INVALID_HANDLE_VALUE) return true;
    DWORD code = 0;
    if (!GetExitCodeProcess(impl_->child_handle, &code)) return true;
    return code != STILL_ACTIVE;
}

McpStdioProcess::~McpStdioProcess() { (void)shutdown(); }

tl::expected<void, std::string> McpStdioProcess::shutdown() {
    if (impl_) {
        if (impl_->child_handle != INVALID_HANDLE_VALUE) {
            DWORD code = 0;
            GetExitCodeProcess(impl_->child_handle, &code);
            if (code == STILL_ACTIVE) {
                TerminateProcess(impl_->child_handle, 1);
                WaitForSingleObject(impl_->child_handle, 5000);
            }
            CloseHandle(impl_->child_handle);
            impl_->child_handle = INVALID_HANDLE_VALUE;
        }
        if (impl_->stdin_write  != INVALID_HANDLE_VALUE) {
            CloseHandle(impl_->stdin_write);  impl_->stdin_write  = INVALID_HANDLE_VALUE;
        }
        if (impl_->stdout_read  != INVALID_HANDLE_VALUE) {
            CloseHandle(impl_->stdout_read);  impl_->stdout_read  = INVALID_HANDLE_VALUE;
        }
    }
    return {};
}

void McpStdioProcess::kill_and_wait() { (void)shutdown(); }

#endif // _WIN32

// ─────────────────────────────────────────────────────────────────────────────
// POSIX implementation
// ─────────────────────────────────────────────────────────────────────────────

#ifndef _WIN32

// ── POSIX: move operations ────────────────────────────────────────────────────

McpStdioProcess::McpStdioProcess(McpStdioProcess&& o) noexcept
    : stdin_fd_(o.stdin_fd_), stdout_fd_(o.stdout_fd_), pid_(o.pid_),
      read_buf_(std::move(o.read_buf_)),
      read_pos_(o.read_pos_), read_end_(o.read_end_)
{
    // Invalidate source so its destructor is a no-op
    o.stdin_fd_  = -1;
    o.stdout_fd_ = -1;
    o.pid_       = -1;
    o.read_pos_  = 0;
    o.read_end_  = 0;
}

McpStdioProcess& McpStdioProcess::operator=(McpStdioProcess&& o) noexcept {
    if (this != &o) {
        (void)shutdown();
        stdin_fd_  = o.stdin_fd_;  o.stdin_fd_  = -1;
        stdout_fd_ = o.stdout_fd_; o.stdout_fd_ = -1;
        pid_       = o.pid_;       o.pid_       = -1;
        read_buf_  = std::move(o.read_buf_);
        read_pos_  = o.read_pos_;  o.read_pos_  = 0;
        read_end_  = o.read_end_;  o.read_end_  = 0;
    }
    return *this;
}

tl::expected<McpStdioProcess, std::string>
McpStdioProcess::spawn(const std::string& command,
                       const std::vector<std::string>& args,
                       const std::map<std::string,std::string>& env_extras)
{
    int stdin_pipe[2]  = {-1, -1};
    int stdout_pipe[2] = {-1, -1};

    if (::pipe(stdin_pipe) != 0) {
        return tl::unexpected(std::format("pipe(stdin): {}", strerror(errno)));
    }
    if (::pipe(stdout_pipe) != 0) {
        ::close(stdin_pipe[0]); ::close(stdin_pipe[1]);
        return tl::unexpected(std::format("pipe(stdout): {}", strerror(errno)));
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(stdin_pipe[0]);  ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        return tl::unexpected(std::format("fork: {}", strerror(errno)));
    }

    if (pid == 0) {
        // Child process
        ::dup2(stdin_pipe[0],  STDIN_FILENO);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        // stderr inherited

        ::close(stdin_pipe[0]);  ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);

        // Apply env extras
        for (const auto& [k, v] : env_extras) {
            ::setenv(k.c_str(), v.c_str(), 1);
        }

        // Build argv
        std::vector<const char*> argv_ptrs;
        argv_ptrs.push_back(command.c_str());
        for (const auto& a : args) argv_ptrs.push_back(a.c_str());
        argv_ptrs.push_back(nullptr);

        ::execvp(command.c_str(), const_cast<char**>(argv_ptrs.data()));
        ::_exit(127);
    }

    // Parent process
    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);

    McpStdioProcess proc;
    proc.stdin_fd_  = stdin_pipe[1];
    proc.stdout_fd_ = stdout_pipe[0];
    proc.pid_       = pid;
    return proc;
}

// ── Raw I/O helpers ──────────────────────────────────────────────────────────

tl::expected<void, std::string>
McpStdioProcess::write_all(const uint8_t* data, std::size_t len) {
    while (len > 0) {
        ssize_t w = ::write(stdin_fd_, data, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return tl::unexpected(std::format("write: {}", strerror(errno)));
        }
        data += w;
        len  -= static_cast<std::size_t>(w);
    }
    return {};
}

tl::expected<void, std::string> McpStdioProcess::flush() {
    // POSIX pipe: no buffering on our side, nothing to flush
    return {};
}

tl::expected<void, std::string> McpStdioProcess::write_line(const std::string& line) {
    auto r = write_all(reinterpret_cast<const uint8_t*>(line.data()), line.size());
    if (!r) return r;
    uint8_t newline = '\n';
    return write_all(&newline, 1);
}

// Read a newline-terminated line from stdout_fd_.
// Mirrors Rust read_line() / BufReader::read_line().
tl::expected<std::string, std::string>
McpStdioProcess::read_line(uint64_t timeout_ms) {
    std::string line;
    while (true) {
        // Check buffered bytes first
        if (read_pos_ < read_end_) {
            char c = static_cast<char>(read_buf_[read_pos_++]);
            line += c;
            if (c == '\n') return line;
            continue;
        }
        // Wait for data (with optional timeout)
        if (timeout_ms > 0 && !posix_wait_readable(stdout_fd_, timeout_ms)) {
            return tl::unexpected("timeout reading line");
        }
        ssize_t r = ::read(stdout_fd_, read_buf_.data(), read_buf_.size());
        if (r == 0) {
            if (line.empty())
                return tl::unexpected("MCP stdio stream closed while reading line");
            return line; // partial line at EOF
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            return tl::unexpected(std::format("read: {}", strerror(errno)));
        }
        read_pos_ = 0;
        read_end_ = static_cast<std::size_t>(r);
    }
}

// Read whatever bytes are currently available (non-blocking peek + read).
// Mirrors Rust read_available().
tl::expected<std::vector<uint8_t>, std::string>
McpStdioProcess::read_available() {
    std::vector<uint8_t> buf(4096);
    ssize_t r = ::read(stdout_fd_, buf.data(), buf.size());
    if (r < 0) return tl::unexpected(std::format("read: {}", strerror(errno)));
    buf.resize(static_cast<std::size_t>(r));
    return buf;
}

// Read a Content-Length framed message.
// Mirrors Rust McpStdioProcess::read_frame().
tl::expected<std::vector<uint8_t>, std::string>
McpStdioProcess::read_frame(uint64_t timeout_ms) {
    std::optional<std::size_t> content_length;

    // Read header lines until bare \r\n
    while (true) {
        auto line_r = read_line(timeout_ms);
        if (!line_r) {
            return tl::unexpected(
                std::format("MCP stdio stream closed while reading headers: {}", line_r.error()));
        }
        std::string line = *line_r;
        // Strip trailing \r\n
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.empty()) {
            // End of headers
            break;
        }

        // Parse "Name: Value"
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name  = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            // trim leading whitespace from value
            auto vs = value.find_first_not_of(" \t");
            if (vs != std::string::npos) value = value.substr(vs);

            // case-insensitive compare
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            if (name_lower == "content-length") {
                try {
                    content_length = std::stoul(value);
                } catch (...) {
                    return tl::unexpected("invalid Content-Length value");
                }
            }
        }
    }

    if (!content_length) {
        return tl::unexpected("missing Content-Length header");
    }

    // Read exactly content_length bytes
    std::vector<uint8_t> payload(*content_length, 0);
    std::size_t offset = 0;

    while (offset < *content_length) {
        // Check buffer first
        if (read_pos_ < read_end_) {
            std::size_t avail   = read_end_ - read_pos_;
            std::size_t needed  = *content_length - offset;
            std::size_t to_copy = std::min(avail, needed);
            std::memcpy(payload.data() + offset, read_buf_.data() + read_pos_, to_copy);
            read_pos_ += to_copy;
            offset    += to_copy;
            continue;
        }
        // Refill (with optional timeout)
        if (timeout_ms > 0 && !posix_wait_readable(stdout_fd_, timeout_ms)) {
            return tl::unexpected("timeout reading frame body");
        }
        ssize_t r = ::read(stdout_fd_, read_buf_.data(), read_buf_.size());
        if (r == 0) return tl::unexpected("EOF reading frame body");
        if (r < 0) {
            if (errno == EINTR) continue;
            return tl::unexpected(std::format("read: {}", strerror(errno)));
        }
        read_pos_ = 0;
        read_end_ = static_cast<std::size_t>(r);
    }

    return payload;
}

// Write a Content-Length framed message.
// Mirrors Rust McpStdioProcess::write_frame().
tl::expected<void, std::string>
McpStdioProcess::write_frame(const std::vector<uint8_t>& payload) {
    auto framed = encode_frame(payload);
    return write_all(framed.data(), framed.size());
}

// ── High-level JSON-RPC helpers ───────────────────────────────────────────────

// Mirrors Rust write_jsonrpc_message<T: Serialize>
tl::expected<void, std::string>
McpStdioProcess::write_jsonrpc_message(const nlohmann::json& msg) {
    std::string body = msg.dump();
    std::vector<uint8_t> payload(body.begin(), body.end());
    return write_frame(payload);
}

// Mirrors Rust read_jsonrpc_message<T: DeserializeOwned>
tl::expected<nlohmann::json, std::string>
McpStdioProcess::read_jsonrpc_message(uint64_t timeout_ms) {
    auto payload_r = read_frame(timeout_ms);
    if (!payload_r) return tl::unexpected(payload_r.error());
    try {
        return nlohmann::json::parse(*payload_r);
    } catch (const std::exception& ex) {
        // mirrors io::ErrorKind::InvalidData
        return tl::unexpected(std::format("JSON parse error: {}", ex.what()));
    }
}

// Mirrors Rust send_request<T: Serialize>
tl::expected<void, std::string>
McpStdioProcess::send_request(const nlohmann::json& request_json) {
    return write_jsonrpc_message(request_json);
}

// Mirrors Rust read_response<T: DeserializeOwned>
tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::read_response(uint64_t timeout_ms) {
    auto j_r = read_jsonrpc_message(timeout_ms);
    if (!j_r) return tl::unexpected(j_r.error());
    return jsonrpc_response_from_json(*j_r);
}

// Mirrors Rust request<TParams, TResult>(id, method, params)
tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::request(const JsonRpcId& id,
                          const std::string& method,
                          const nlohmann::json& params,
                          uint64_t timeout_ms)
{
    // Build and send request
    nlohmann::json req_json = {
        {"jsonrpc", "2.0"},
        {"id",      jsonrpc_id_to_json(id)},
        {"method",  method},
    };
    if (!params.is_null()) req_json["params"] = params;

    {
        auto r = send_request(req_json);
        if (!r) return tl::unexpected(r.error());
    }

    // Read response
    auto resp_r = read_response(timeout_ms);
    if (!resp_r) return tl::unexpected(resp_r.error());
    JsonRpcResponse& resp = *resp_r;

    // Validate jsonrpc version
    if (resp.jsonrpc != "2.0") {
        return tl::unexpected(std::format(
            "MCP response for {} used unsupported jsonrpc version `{}`",
            method, resp.jsonrpc));
    }

    // Validate response id matches
    if (!jsonrpc_ids_equal(resp.id, id)) {
        return tl::unexpected(std::format(
            "MCP response for {} used mismatched id: expected {}, got {}",
            method, jsonrpc_id_debug(id), jsonrpc_id_debug(resp.id)));
    }

    return resp;
}

// ── Typed convenience methods (mirror Rust pub async fn) ─────────────────────

// Mirrors Rust initialize(id, params) -> io::Result<JsonRpcResponse<McpInitializeResult>>
tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::initialize(const JsonRpcId& id,
                             const McpInitializeParams& params,
                             uint64_t timeout_ms)
{
    nlohmann::json j_params = {
        {"protocolVersion", params.protocol_version},
        {"capabilities",    params.capabilities},
        {"clientInfo", {
            {"name",    params.client_info.name},
            {"version", params.client_info.version},
        }},
    };
    return request(id, "initialize", j_params, timeout_ms);
}

// Mirrors Rust list_tools(id, params) -> io::Result<JsonRpcResponse<McpListToolsResult>>
tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::list_tools(const JsonRpcId& id,
                              const std::optional<std::string>& cursor,
                              uint64_t timeout_ms)
{
    nlohmann::json j_params = nlohmann::json::object();
    if (cursor) j_params["cursor"] = *cursor;
    return request(id, "tools/list", j_params, timeout_ms);
}

// Mirrors Rust call_tool(id, params) -> io::Result<JsonRpcResponse<McpToolCallResult>>
tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::call_tool(const JsonRpcId& id,
                             const std::string& name,
                             const nlohmann::json& arguments,
                             uint64_t timeout_ms)
{
    nlohmann::json j_params = {{"name", name}};
    if (!arguments.is_null()) j_params["arguments"] = arguments;
    return request(id, "tools/call", j_params, timeout_ms);
}

// Mirrors Rust list_resources(id, params) -> io::Result<JsonRpcResponse<McpListResourcesResult>>
tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::list_resources(const JsonRpcId& id,
                                  const std::optional<std::string>& cursor,
                                  uint64_t timeout_ms)
{
    nlohmann::json j_params = nlohmann::json::object();
    if (cursor) j_params["cursor"] = *cursor;
    return request(id, "resources/list", j_params, timeout_ms);
}

// Mirrors Rust read_resource(id, params) -> io::Result<JsonRpcResponse<McpReadResourceResult>>
tl::expected<JsonRpcResponse, std::string>
McpStdioProcess::read_resource(const JsonRpcId& id,
                                 const std::string& uri,
                                 uint64_t timeout_ms)
{
    nlohmann::json j_params = {{"uri", uri}};
    return request(id, "resources/read", j_params, timeout_ms);
}

// Mirrors Rust terminate() -> io::Result<()>
tl::expected<void, std::string> McpStdioProcess::terminate() {
    if (pid_ > 0) {
        if (::kill(pid_, SIGTERM) != 0 && errno != ESRCH) {
            return tl::unexpected(std::format("kill(SIGTERM): {}", strerror(errno)));
        }
    }
    return {};
}

// Mirrors Rust wait() -> io::Result<ExitStatus>
tl::expected<int, std::string> McpStdioProcess::wait_for_exit() {
    if (pid_ <= 0) return 0;
    int status = 0;
    if (::waitpid(pid_, &status, 0) < 0) {
        return tl::unexpected(std::format("waitpid: {}", strerror(errno)));
    }
    pid_ = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Mirrors Rust has_exited() -> io::Result<bool>
tl::expected<bool, std::string> McpStdioProcess::has_exited() {
    if (pid_ <= 0) return true;
    int status = 0;
    pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r < 0) return tl::unexpected(std::format("waitpid: {}", strerror(errno)));
    if (r == 0) return false;
    pid_ = -1;
    return true;
}

// Mirrors Rust async fn shutdown(&mut self) -> io::Result<()>
// Kill the child if still running, then wait.
tl::expected<void, std::string> McpStdioProcess::shutdown() {
    if (pid_ > 0) {
        int status = 0;
        pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == 0) {
            // Still running: send SIGTERM
            ::kill(pid_, SIGTERM);
            // Wait with timeout
            for (int i = 0; i < 50; ++i) {
                struct timespec ts{ 0, 20'000'000 }; // 20ms
                ::nanosleep(&ts, nullptr);
                r = ::waitpid(pid_, &status, WNOHANG);
                if (r != 0) break;
            }
            if (r == 0) {
                // Force kill
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, &status, 0);
            }
        }
        pid_ = -1;
    }
    if (stdin_fd_ >= 0)  { ::close(stdin_fd_);  stdin_fd_  = -1; }
    if (stdout_fd_ >= 0) { ::close(stdout_fd_); stdout_fd_ = -1; }
    return {};
}

// kill_and_wait: alias used by old API
void McpStdioProcess::kill_and_wait() {
    (void)shutdown();
}

#endif // !_WIN32

// ─────────────────────────────────────────────────────────────────────────────
// §6  Free functions
// ─────────────────────────────────────────────────────────────────────────────

// Mirrors Rust fn default_initialize_params() -> McpInitializeParams
McpInitializeParams default_initialize_params() {
    const char* ver = std::getenv("CARGO_PKG_VERSION");
    return McpInitializeParams{
        .protocol_version = "2025-03-26",
        .client_info = McpClientInfo{
            .name    = "runtime",
            .version = ver ? ver : "0.0.0",
        },
        .capabilities = nlohmann::json::object(),
    };
}

// Mirrors Rust write_frame / read_frame (the standalone free-function versions
// used by McpServerManagerError::to_string display etc.)
tl::expected<void, std::string> write_frame(int fd, const nlohmann::json& msg) {
    std::string body = msg.dump();
    std::vector<uint8_t> payload(body.begin(), body.end());
    auto framed = encode_frame(payload);
    const uint8_t* ptr = framed.data();
    std::size_t remaining = framed.size();
    while (remaining > 0) {
#ifdef _WIN32
        // fd is a POSIX integer — not usable directly on Windows.
        (void)ptr; (void)remaining;
        return tl::unexpected("write_frame(int fd) not supported on Windows; use McpStdioProcess");
#else
        ssize_t w = ::write(fd, ptr, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            return tl::unexpected(std::format("write: {}", strerror(errno)));
        }
        ptr       += static_cast<std::size_t>(w);
        remaining -= static_cast<std::size_t>(w);
#endif
    }
    return {};
}

tl::expected<nlohmann::json, std::string> read_frame(int fd) {
    // Read header bytes one-at-a-time until \r\n\r\n
    std::string header_buf;
    while (true) {
#ifdef _WIN32
        return tl::unexpected("read_frame(int fd) not supported on Windows");
#else
        char ch;
        ssize_t r = ::read(fd, &ch, 1);
        if (r <= 0) return tl::unexpected("EOF or error reading frame header");
        header_buf += ch;
        if (header_buf.size() >= 4 &&
            header_buf.compare(header_buf.size() - 4, 4, "\r\n\r\n") == 0) break;
#endif
    }

    // Parse Content-Length (case-insensitive)
    std::size_t content_length = 0;
    std::istringstream hss(header_buf);
    std::string hline;
    while (std::getline(hss, hline)) {
        if (!hline.empty() && hline.back() == '\r') hline.pop_back();
        std::string lower = hline;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        static constexpr std::string_view KEY = "content-length: ";
        if (lower.starts_with(KEY)) {
            try { content_length = std::stoul(hline.substr(KEY.size())); }
            catch (...) {}
        }
    }
    if (content_length == 0) return tl::unexpected("no Content-Length in frame");

    // Read body
    std::string body(content_length, '\0');
    std::size_t offset = 0;
    while (offset < content_length) {
#ifndef _WIN32
        ssize_t r = ::read(fd, body.data() + offset, content_length - offset);
        if (r <= 0) return tl::unexpected("EOF reading frame body");
        offset += static_cast<std::size_t>(r);
#endif
    }

    try {
        return nlohmann::json::parse(body);
    } catch (const std::exception& ex) {
        return tl::unexpected(std::format("JSON parse error: {}", ex.what()));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §7  McpServerManager internal types
// ─────────────────────────────────────────────────────────────────────────────

// Mirrors Rust struct ToolRoute { server_name, raw_name }
struct ToolRoute {
    std::string server_name;
    std::string raw_name;
};

// Mirrors Rust struct ManagedMcpServer { bootstrap, process, initialized }
struct ManagedMcpServer {
    McpServerManager::ServerConfig config;    // mirrors bootstrap
    std::optional<McpStdioProcess>  process;  // None until first use
    bool initialized{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// §8  McpServerManager internal state (PIMPL)
//     We store state directly in the class via a hidden inner struct accessed
//     through a void* opaque handle to avoid polluting the public header.
//     For simplicity we use a plain struct stored in a unique_ptr.
// ─────────────────────────────────────────────────────────────────────────────

struct McpServerManagerState {
    // BTreeMap<String, ManagedMcpServer> — ordered by server name
    std::map<std::string, ManagedMcpServer>   servers;
    std::vector<UnsupportedMcpServer>          unsupported_servers;
    // BTreeMap<String, ToolRoute>
    std::map<std::string, ToolRoute>           tool_index;
    uint64_t                                   next_request_id{1};
};

// ─────────────────────────────────────────────────────────────────────────────
// §9  McpServerManager — public API (mirrors Rust impl McpServerManager)
// ─────────────────────────────────────────────────────────────────────────────

McpServerManager::McpServerManager()
    : state_(std::make_unique<McpServerManagerState>())
{}

McpServerManager::~McpServerManager() = default;
McpServerManager::McpServerManager(McpServerManager&&) = default;
McpServerManager& McpServerManager::operator=(McpServerManager&&) = default;

// Mirrors Rust from_servers(servers: &BTreeMap<String, ScopedMcpServerConfig>) -> Self
// (Here we accept Vec<ServerConfig> as per C++ header)
tl::expected<std::shared_ptr<McpServerManager>, McpServerManagerError>
McpServerManager::from_servers(std::vector<ServerConfig> configs) {
    auto mgr = std::shared_ptr<McpServerManager>(new McpServerManager());
    auto& st = *mgr->state_;

    for (auto& cfg : configs) {
        // All configs here are stdio; non-stdio goes to unsupported
        ManagedMcpServer srv;
        srv.config      = cfg;
        srv.initialized = false;
        st.servers.emplace(cfg.name, std::move(srv));
    }

    return mgr;
}

// Mirrors Rust unsupported_servers(&self) -> &[UnsupportedMcpServer]
const std::vector<UnsupportedMcpServer>&
McpServerManager::unsupported_servers() const {
    return state_->unsupported_servers;
}

// Mirrors Rust server_names(&self) -> Vec<String>
std::vector<std::string> McpServerManager::server_names() const {
    std::vector<std::string> names;
    for (const auto& [k, _] : state_->servers) names.push_back(k);
    return names;
}

// ── Internal helpers ──────────────────────────────────────────────────────────

// Mirrors Rust fn take_request_id(&mut self) -> JsonRpcId
static JsonRpcId take_request_id(McpServerManagerState& st) {
    uint64_t id = st.next_request_id;
    // saturating_add
    st.next_request_id = (st.next_request_id < UINT64_MAX)
                       ? st.next_request_id + 1
                       : UINT64_MAX;
    return JsonRpcIdNumber{ static_cast<int64_t>(id) };
}

// Mirrors Rust fn server_mut(...) -> Result<&mut ManagedMcpServer, ...>
static ManagedMcpServer*
server_mut(McpServerManagerState& st, const std::string& server_name,
           McpServerManagerError* err_out) {
    auto it = st.servers.find(server_name);
    if (it == st.servers.end()) {
        if (err_out)
            *err_out = McpServerManagerError::make_unknown_server(server_name);
        return nullptr;
    }
    return &it->second;
}

// Mirrors Rust fn clear_routes_for_server(&mut self, server_name: &str)
static void clear_routes_for_server(McpServerManagerState& st, const std::string& server_name) {
    for (auto it = st.tool_index.begin(); it != st.tool_index.end(); ) {
        if (it->second.server_name == server_name)
            it = st.tool_index.erase(it);
        else
            ++it;
    }
}

// Mirrors Rust fn is_retryable_error(error: &McpServerManagerError) -> bool
static bool is_retryable_error(const McpServerManagerError& err) {
    return err.is_transport() || err.is_timeout();
}

// Mirrors Rust fn should_reset_server(error: &McpServerManagerError) -> bool
static bool should_reset_server(const McpServerManagerError& err) {
    return err.is_transport() || err.is_timeout() || err.is_invalid_response();
}

// Mirrors Rust fn tool_call_timeout_ms(&self, server_name: &str) -> Result<u64, ...>
static tl::expected<uint64_t, McpServerManagerError>
tool_call_timeout_ms(const McpServerManagerState& st, const std::string& server_name) {
    auto it = st.servers.find(server_name);
    if (it == st.servers.end()) {
        return tl::unexpected(McpServerManagerError::make_unknown_server(server_name));
    }
    const auto& cfg = it->second.config;
    uint64_t tms = cfg.tool_call_timeout_ms > 0
                 ? static_cast<uint64_t>(cfg.tool_call_timeout_ms)
                 : 60'000;
    return tms;
}

// Mirrors Rust fn server_process_exited(&mut self, server_name: &str) -> Result<bool, ...>
static tl::expected<bool, McpServerManagerError>
server_process_exited(McpServerManagerState& st, const std::string& server_name) {
    auto* srv = server_mut(st, server_name, nullptr);
    if (!srv) return tl::unexpected(McpServerManagerError::make_unknown_server(server_name));
    if (!srv->process) return false;
    auto r = srv->process->has_exited();
    if (!r) return tl::unexpected(McpServerManagerError::make_transport(
        server_name, "has_exited", r.error()));
    return *r;
}

// Mirrors Rust async fn reset_server(&mut self, server_name: &str) -> Result<(), ...>
static tl::expected<void, McpServerManagerError>
reset_server(McpServerManagerState& st, const std::string& server_name) {
    McpServerManagerError dummy;
    auto* srv = server_mut(st, server_name, &dummy);
    if (!srv) return tl::unexpected(dummy);
    srv->initialized = false;
    if (srv->process) {
        (void)srv->process->shutdown(); // best-effort
        srv->process.reset();
    }
    return {};
}

// Spawn a new process for the given server config.
// Mirrors Rust spawn_mcp_stdio_process(bootstrap).
static tl::expected<McpStdioProcess, McpServerManagerError>
spawn_server(const McpServerManager::ServerConfig& cfg) {
    auto r = McpStdioProcess::spawn(cfg.command, cfg.args, cfg.env);
    if (!r) {
        return tl::unexpected(McpServerManagerError::make_transport(
            cfg.name, "spawn", r.error()));
    }
    return std::move(*r);
}

// Wrap a process I/O result with timeout, mapping errors to McpServerManagerError.
// Mirrors Rust run_process_request(server_name, method, timeout_ms, future).
// In C++ (synchronous) the timeout is already applied at the fd level.
// We just need to classify the error type correctly.
template<typename Fn>
static auto run_process_request(const std::string& server_name,
                                 const char* method,
                                 uint64_t /*timeout_ms*/,
                                 Fn&& fn)
    -> tl::expected<JsonRpcResponse, McpServerManagerError>
{
    auto r = fn();
    if (!r) {
        const std::string& errmsg = r.error();
        // Classify: if the message mentions "timeout" → Timeout
        if (errmsg.find("timeout") != std::string::npos) {
            return tl::unexpected(McpServerManagerError::make_timeout(
                server_name, method, 0 /* timeout embedded in msg */));
        }
        // JSON parse / mismatched id → InvalidResponse (mirrors InvalidData)
        if (errmsg.find("JSON parse") != std::string::npos ||
            errmsg.find("mismatched id") != std::string::npos ||
            errmsg.find("unsupported jsonrpc") != std::string::npos ||
            errmsg.find("missing Content-Length") != std::string::npos ||
            errmsg.find("invalid Content-Length") != std::string::npos) {
            return tl::unexpected(McpServerManagerError::make_invalid_response(
                server_name, method, errmsg));
        }
        // Everything else → Transport
        return tl::unexpected(McpServerManagerError::make_transport(
            server_name, method, errmsg));
    }
    return *r;
}

// Overload that accepts explicit timeout_ms and passes it to the process call.
template<typename Fn>
static auto run_process_request_timed(const std::string& server_name,
                                       const char* method,
                                       uint64_t timeout_ms,
                                       Fn&& fn)
    -> tl::expected<JsonRpcResponse, McpServerManagerError>
{
    (void)timeout_ms; // already wired into Fn via capture
    return run_process_request(server_name, method, timeout_ms, std::forward<Fn>(fn));
}

// ─────────────────────────────────────────────────────────────────────────────
// §10  ensure_server_ready
//      Mirrors Rust async fn ensure_server_ready(&mut self, server_name: &str)
// ─────────────────────────────────────────────────────────────────────────────

static tl::expected<void, McpServerManagerError>
ensure_server_ready(McpServerManagerState& st, const std::string& server_name) {
    // Reset if child has exited
    {
        auto exited = server_process_exited(st, server_name);
        if (!exited) return tl::unexpected(exited.error());
        if (*exited) {
            auto r = reset_server(st, server_name);
            if (!r) return r;
        }
    }

    int attempts = 0;
    while (true) {
        // ── Check if spawn needed ──
        {
            auto it = st.servers.find(server_name);
            if (it == st.servers.end()) {
                return tl::unexpected(
                    McpServerManagerError::make_unknown_server(server_name));
            }
            if (!it->second.process) {
                // Spawn
                auto proc = spawn_server(it->second.config);
                if (!proc) {
                    return tl::unexpected(proc.error());
                }
                it->second.process = std::move(*proc);
                it->second.initialized = false;
            }
        }

        // ── Check if initialize needed ──
        {
            auto it = st.servers.find(server_name);
            if (it == st.servers.end()) {
                return tl::unexpected(
                    McpServerManagerError::make_unknown_server(server_name));
            }
            if (it->second.initialized) return {}; // done
        }

        // ── Send initialize request ──
        JsonRpcId req_id = take_request_id(st);

        auto response_r = [&]() -> tl::expected<JsonRpcResponse, McpServerManagerError> {
            auto it = st.servers.find(server_name);
            if (it == st.servers.end())
                return tl::unexpected(McpServerManagerError::make_unknown_server(server_name));
            if (!it->second.process)
                return tl::unexpected(McpServerManagerError::make_invalid_response(
                    server_name, "initialize", "server process missing before initialize"));

            McpStdioProcess& proc = *it->second.process;
            return run_process_request(server_name, "initialize",
                MCP_INITIALIZE_TIMEOUT_MS,
                [&]() {
                    return proc.initialize(req_id, default_initialize_params(),
                                           MCP_INITIALIZE_TIMEOUT_MS);
                });
        }();

        if (!response_r) {
            const auto& err = response_r.error();
            if (attempts == 0 && is_retryable_error(err)) {
                auto r = reset_server(st, server_name);
                if (!r) return r;
                ++attempts;
                continue;
            }
            if (should_reset_server(err)) {
                (void)reset_server(st, server_name);
            }
            return tl::unexpected(err);
        }

        const JsonRpcResponse& resp = *response_r;

        if (resp.error) {
            return tl::unexpected(McpServerManagerError::make_jsonrpc(
                server_name, "initialize", *resp.error));
        }

        if (resp.result.is_null()) {
            auto err = McpServerManagerError::make_invalid_response(
                server_name, "initialize", "missing result payload");
            (void)reset_server(st, server_name);
            return tl::unexpected(err);
        }

        // Mark initialized
        {
            auto it = st.servers.find(server_name);
            if (it != st.servers.end()) it->second.initialized = true;
        }
        return {};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §11  discover_tools_for_server_once
//      Mirrors Rust async fn discover_tools_for_server_once
// ─────────────────────────────────────────────────────────────────────────────

static tl::expected<std::vector<ManagedMcpTool>, McpServerManagerError>
discover_tools_for_server_once(McpServerManagerState& st, const std::string& server_name) {
    {
        auto r = ensure_server_ready(st, server_name);
        if (!r) return tl::unexpected(r.error());
    }

    std::vector<ManagedMcpTool> discovered;
    std::optional<std::string>  cursor;

    while (true) {
        JsonRpcId req_id = take_request_id(st);

        auto response_r = [&]() -> tl::expected<JsonRpcResponse, McpServerManagerError> {
            auto it = st.servers.find(server_name);
            if (it == st.servers.end())
                return tl::unexpected(McpServerManagerError::make_unknown_server(server_name));
            if (!it->second.process)
                return tl::unexpected(McpServerManagerError::make_invalid_response(
                    server_name, "tools/list",
                    "server process missing after initialization"));

            McpStdioProcess& proc = *it->second.process;
            return run_process_request(server_name, "tools/list",
                MCP_LIST_TOOLS_TIMEOUT_MS,
                [&]() {
                    return proc.list_tools(req_id, cursor, MCP_LIST_TOOLS_TIMEOUT_MS);
                });
        }();

        if (!response_r) return tl::unexpected(response_r.error());
        const JsonRpcResponse& resp = *response_r;

        if (resp.error) {
            return tl::unexpected(McpServerManagerError::make_jsonrpc(
                server_name, "tools/list", *resp.error));
        }

        if (resp.result.is_null()) {
            return tl::unexpected(McpServerManagerError::make_invalid_response(
                server_name, "tools/list", "missing result payload"));
        }

        const nlohmann::json& result = resp.result;

        // Parse tools array
        if (result.contains("tools") && result["tools"].is_array()) {
            for (const auto& jt : result["tools"]) {
                McpTool tool;
                tool.name        = jt.value("name", std::string{});
                if (jt.contains("description") && jt["description"].is_string())
                    tool.description = jt["description"].get<std::string>();
                if (jt.contains("inputSchema"))
                    tool.input_schema = jt["inputSchema"];
                if (jt.contains("annotations"))
                    tool.meta = jt["annotations"]; // store in meta for now
                if (jt.contains("_meta"))
                    tool.meta = jt["_meta"];

                std::string qualified = mcp_tool_name(server_name, tool.name);
                discovered.push_back(ManagedMcpTool{
                    server_name,
                    qualified,
                    tool.name,
                    tool,
                });
            }
        }

        // Check for pagination cursor
        if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
            cursor = result["nextCursor"].get<std::string>();
        } else {
            break;
        }
    }

    return discovered;
}

// ─────────────────────────────────────────────────────────────────────────────
// §12  discover_tools_for_server (with retry)
//      Mirrors Rust async fn discover_tools_for_server
// ─────────────────────────────────────────────────────────────────────────────

static tl::expected<std::vector<ManagedMcpTool>, McpServerManagerError>
discover_tools_for_server(McpServerManagerState& st, const std::string& server_name) {
    int attempts = 0;
    while (true) {
        auto result = discover_tools_for_server_once(st, server_name);
        if (result) return result;

        const auto& err = result.error();
        if (attempts == 0 && is_retryable_error(err)) {
            auto r = reset_server(st, server_name);
            if (!r) return tl::unexpected(r.error());
            ++attempts;
            continue;
        }
        if (should_reset_server(err)) {
            (void)reset_server(st, server_name);
        }
        return result;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §13  McpServerManager::discover_tools
//      Mirrors Rust pub async fn discover_tools(&mut self)
// ─────────────────────────────────────────────────────────────────────────────

tl::expected<std::vector<ManagedMcpTool>, McpServerManagerError>
McpServerManager::discover_tools() {
    auto& st = *state_;
    std::vector<std::string> names = server_names();
    std::vector<ManagedMcpTool> discovered;

    for (const auto& srv_name : names) {
        auto tools_r = discover_tools_for_server(st, srv_name);
        if (!tools_r) return tl::unexpected(tools_r.error());

        clear_routes_for_server(st, srv_name);
        for (const auto& tool : *tools_r) {
            st.tool_index.emplace(tool.qualified_name,
                ToolRoute{ tool.server_name, tool.raw_name });
            discovered.push_back(tool);
        }
    }

    return discovered;
}

// ─────────────────────────────────────────────────────────────────────────────
// §14  McpServerManager::discover_tools_best_effort
//      Mirrors Rust pub async fn discover_tools_best_effort(&mut self)
// ─────────────────────────────────────────────────────────────────────────────

McpToolDiscoveryReport McpServerManager::discover_tools_best_effort() {
    auto& st = *state_;
    std::vector<std::string> names = server_names();

    McpToolDiscoveryReport report;
    report.unsupported_servers = st.unsupported_servers;

    std::vector<std::string> working_servers;

    for (const auto& srv_name : names) {
        auto tools_r = discover_tools_for_server(st, srv_name);
        if (tools_r) {
            working_servers.push_back(srv_name);
            clear_routes_for_server(st, srv_name);
            for (const auto& tool : *tools_r) {
                st.tool_index.emplace(tool.qualified_name,
                    ToolRoute{ tool.server_name, tool.raw_name });
                report.tools.push_back(tool);
            }
        } else {
            clear_routes_for_server(st, srv_name);
            report.failures.push_back(tools_r.error().discovery_failure(srv_name));
        }
    }

    // Build degraded report when some servers work but others failed
    std::vector<McpFailedServer> degraded_failed;
    for (const auto& failure : report.failures) {
        degraded_failed.push_back(McpFailedServer{
            failure.server_name,
            failure.phase,
            McpErrorSurface::make(
                failure.phase,
                failure.server_name,
                failure.error,
                failure.context,
                failure.recoverable),
        });
    }
    for (const auto& unsup : st.unsupported_servers) {
        degraded_failed.push_back(McpFailedServer{
            unsup.server_name,
            McpLifecyclePhase::ServerRegistration,
            McpErrorSurface::make(
                McpLifecyclePhase::ServerRegistration,
                unsup.server_name,
                unsup.reason,
                {{"transport", unsup.transport}},
                false),
        });
    }

    if (!working_servers.empty() && !degraded_failed.empty()) {
        std::vector<std::string> available_tools;
        for (const auto& tool : report.tools) {
            available_tools.push_back(tool.qualified_name);
        }
        report.degraded_startup = McpDegradedReport{
            std::move(working_servers),
            std::move(degraded_failed),
            std::move(available_tools),
            {}, // missing_tools
        };
    }

    return report;
}

// ─────────────────────────────────────────────────────────────────────────────
// §15  McpServerManager::call_tool_rpc / call_tool
//      Mirrors Rust pub async fn call_tool(&mut self, qualified_tool_name, arguments)
//      -> Result<JsonRpcResponse<McpToolCallResult>, McpServerManagerError>
//
// call_tool_rpc: returns the raw JsonRpcResponse (faithful to Rust return type).
// call_tool:     unpacks JsonRpcResponse → McpToolCallResult for backward compat.
// ─────────────────────────────────────────────────────────────────────────────

static tl::expected<JsonRpcResponse, McpServerManagerError>
call_tool_impl(McpServerManagerState& st, const std::string& qualified_tool_name,
               const nlohmann::json& arguments)
{
    // Look up route
    auto route_it = st.tool_index.find(qualified_tool_name);
    if (route_it == st.tool_index.end()) {
        return tl::unexpected(
            McpServerManagerError::make_unknown_tool(qualified_tool_name));
    }
    ToolRoute route = route_it->second; // copy (server may be reset)

    auto timeout_r = tool_call_timeout_ms(st, route.server_name);
    if (!timeout_r) return tl::unexpected(timeout_r.error());
    uint64_t tms = *timeout_r;

    {
        auto r = ensure_server_ready(st, route.server_name);
        if (!r) return tl::unexpected(r.error());
    }

    JsonRpcId req_id = take_request_id(st);

    auto response_r = [&]() -> tl::expected<JsonRpcResponse, McpServerManagerError> {
        auto it = st.servers.find(route.server_name);
        if (it == st.servers.end())
            return tl::unexpected(
                McpServerManagerError::make_unknown_server(route.server_name));
        if (!it->second.process)
            return tl::unexpected(McpServerManagerError::make_invalid_response(
                route.server_name, "tools/call",
                "server process missing after initialization"));

        McpStdioProcess& proc = *it->second.process;
        return run_process_request(route.server_name, "tools/call", tms,
            [&]() {
                return proc.call_tool(req_id, route.raw_name, arguments, tms);
            });
    }();

    if (!response_r) {
        if (should_reset_server(response_r.error())) {
            (void)reset_server(st, route.server_name);
        }
        return tl::unexpected(response_r.error());
    }

    return response_r;
}

// Raw JSON-RPC response (faithful Rust equivalent)
tl::expected<JsonRpcResponse, McpServerManagerError>
McpServerManager::call_tool_rpc(const std::string& qualified_tool_name,
                                 const nlohmann::json& arguments)
{
    return call_tool_impl(*state_, qualified_tool_name, arguments);
}

// Unpacked result — for backward compatibility with existing callers
// (mirrors what the Rust manager returns after unwrapping the response).
tl::expected<McpToolCallResult, McpServerManagerError>
McpServerManager::call_tool(const std::string& qualified_tool_name,
                             const nlohmann::json& arguments)
{
    auto resp_r = call_tool_impl(*state_, qualified_tool_name, arguments);
    if (!resp_r) return tl::unexpected(resp_r.error());

    const JsonRpcResponse& resp = *resp_r;

    // Propagate JSON-RPC errors
    if (resp.error) {
        // Find server name for error context
        auto route_it = state_->tool_index.find(qualified_tool_name);
        std::string srv = route_it != state_->tool_index.end()
                        ? route_it->second.server_name
                        : qualified_tool_name;
        return tl::unexpected(McpServerManagerError::make_jsonrpc(
            srv, "tools/call", *resp.error));
    }

    // Unpack result from JSON
    McpToolCallResult result;
    const nlohmann::json& j = resp.result;

    if (j.contains("content") && j["content"].is_array()) {
        for (const auto& item : j["content"]) {
            McpToolCallContent c;
            c.kind = item.value("type", std::string{"text"});
            for (auto it = item.begin(); it != item.end(); ++it) {
                if (it.key() == "type") continue;
                c.data[it.key()] = it.value();
            }
            result.content.push_back(std::move(c));
        }
    }
    if (j.contains("structuredContent"))
        result.structured_content = j["structuredContent"];
    if (j.contains("isError") && j["isError"].is_boolean())
        result.is_error = j["isError"].get<bool>();
    if (j.contains("_meta"))
        result.meta = j["_meta"];

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// §16  list_resources_once / list_resources
//      Mirrors Rust async fn list_resources_once / pub async fn list_resources
// ─────────────────────────────────────────────────────────────────────────────

static tl::expected<McpListResourcesResult, McpServerManagerError>
list_resources_once(McpServerManagerState& st, const std::string& server_name) {
    {
        auto r = ensure_server_ready(st, server_name);
        if (!r) return tl::unexpected(r.error());
    }

    std::vector<McpResource> resources;
    std::optional<std::string> cursor;

    while (true) {
        JsonRpcId req_id = take_request_id(st);

        auto response_r = [&]() -> tl::expected<JsonRpcResponse, McpServerManagerError> {
            auto it = st.servers.find(server_name);
            if (it == st.servers.end())
                return tl::unexpected(McpServerManagerError::make_unknown_server(server_name));
            if (!it->second.process)
                return tl::unexpected(McpServerManagerError::make_invalid_response(
                    server_name, "resources/list",
                    "server process missing after initialization"));

            McpStdioProcess& proc = *it->second.process;
            return run_process_request(server_name, "resources/list",
                MCP_LIST_TOOLS_TIMEOUT_MS,
                [&]() {
                    return proc.list_resources(req_id, cursor, MCP_LIST_TOOLS_TIMEOUT_MS);
                });
        }();

        if (!response_r) return tl::unexpected(response_r.error());
        const JsonRpcResponse& resp = *response_r;

        if (resp.error) {
            return tl::unexpected(McpServerManagerError::make_jsonrpc(
                server_name, "resources/list", *resp.error));
        }
        if (resp.result.is_null()) {
            return tl::unexpected(McpServerManagerError::make_invalid_response(
                server_name, "resources/list", "missing result payload"));
        }

        const nlohmann::json& result = resp.result;
        if (result.contains("resources") && result["resources"].is_array()) {
            for (const auto& jr : result["resources"]) {
                McpResource res;
                res.uri = jr.value("uri", std::string{});
                if (jr.contains("name")        && jr["name"].is_string())
                    res.name = jr["name"].get<std::string>();
                if (jr.contains("description") && jr["description"].is_string())
                    res.description = jr["description"].get<std::string>();
                if (jr.contains("mimeType")    && jr["mimeType"].is_string())
                    res.mime_type = jr["mimeType"].get<std::string>();
                resources.push_back(std::move(res));
            }
        }

        if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
            cursor = result["nextCursor"].get<std::string>();
        } else {
            break;
        }
    }

    return McpListResourcesResult{ std::move(resources), std::nullopt };
}

tl::expected<McpListResourcesResult, McpServerManagerError>
McpServerManager::list_resources(const std::string& server_name) {
    auto& st = *state_;
    int attempts = 0;
    while (true) {
        auto r = list_resources_once(st, server_name);
        if (r) return r;
        const auto& err = r.error();
        if (attempts == 0 && is_retryable_error(err)) {
            auto rr = reset_server(st, server_name);
            if (!rr) return tl::unexpected(rr.error());
            ++attempts;
            continue;
        }
        if (should_reset_server(err)) (void)reset_server(st, server_name);
        return r;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §17  read_resource_once / read_resource
//      Mirrors Rust async fn read_resource_once / pub async fn read_resource
// ─────────────────────────────────────────────────────────────────────────────

static tl::expected<McpReadResourceResult, McpServerManagerError>
read_resource_once(McpServerManagerState& st,
                   const std::string& server_name,
                   const std::string& uri)
{
    {
        auto r = ensure_server_ready(st, server_name);
        if (!r) return tl::unexpected(r.error());
    }

    JsonRpcId req_id = take_request_id(st);

    auto response_r = [&]() -> tl::expected<JsonRpcResponse, McpServerManagerError> {
        auto it = st.servers.find(server_name);
        if (it == st.servers.end())
            return tl::unexpected(McpServerManagerError::make_unknown_server(server_name));
        if (!it->second.process)
            return tl::unexpected(McpServerManagerError::make_invalid_response(
                server_name, "resources/read",
                "server process missing after initialization"));

        McpStdioProcess& proc = *it->second.process;
        return run_process_request(server_name, "resources/read",
            MCP_LIST_TOOLS_TIMEOUT_MS,
            [&]() {
                return proc.read_resource(req_id, uri, MCP_LIST_TOOLS_TIMEOUT_MS);
            });
    }();

    if (!response_r) return tl::unexpected(response_r.error());
    const JsonRpcResponse& resp = *response_r;

    if (resp.error) {
        return tl::unexpected(McpServerManagerError::make_jsonrpc(
            server_name, "resources/read", *resp.error));
    }
    if (resp.result.is_null()) {
        return tl::unexpected(McpServerManagerError::make_invalid_response(
            server_name, "resources/read", "missing result payload"));
    }

    McpReadResourceResult out;
    const nlohmann::json& result = resp.result;
    if (result.contains("contents") && result["contents"].is_array()) {
        for (const auto& jc : result["contents"]) {
            McpResourceContents c;
            c.uri = jc.value("uri", std::string{});
            if (jc.contains("mimeType") && jc["mimeType"].is_string())
                c.mime_type = jc["mimeType"].get<std::string>();
            if (jc.contains("text") && jc["text"].is_string())
                c.text = jc["text"].get<std::string>();
            if (jc.contains("blob") && jc["blob"].is_string())
                c.blob = jc["blob"].get<std::string>();
            out.contents.push_back(std::move(c));
        }
    }
    return out;
}

tl::expected<McpReadResourceResult, McpServerManagerError>
McpServerManager::read_resource(const std::string& server_name, const std::string& uri) {
    auto& st = *state_;
    int attempts = 0;
    while (true) {
        auto r = read_resource_once(st, server_name, uri);
        if (r) return r;
        const auto& err = r.error();
        if (attempts == 0 && is_retryable_error(err)) {
            auto rr = reset_server(st, server_name);
            if (!rr) return tl::unexpected(rr.error());
            ++attempts;
            continue;
        }
        if (should_reset_server(err)) (void)reset_server(st, server_name);
        return r;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §18  McpServerManager::shutdown
//      Mirrors Rust pub async fn shutdown(&mut self) -> Result<(), McpServerManagerError>
// ─────────────────────────────────────────────────────────────────────────────

tl::expected<void, McpServerManagerError> McpServerManager::shutdown() {
    auto& st = *state_;
    std::vector<std::string> names;
    for (auto& [k, _] : st.servers) names.push_back(k);

    for (const auto& srv_name : names) {
        auto it = st.servers.find(srv_name);
        if (it == st.servers.end()) continue;
        if (it->second.process) {
            (void)it->second.process->shutdown(); // best-effort
            it->second.process.reset();
        }
        it->second.initialized = false;
    }
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// §19  discover_tools_report — wrapper around discover_tools_best_effort
//      Kept so callers expecting expected<McpToolDiscoveryReport,...> compile.
// ─────────────────────────────────────────────────────────────────────────────

tl::expected<McpToolDiscoveryReport, McpServerManagerError>
McpServerManager::discover_tools_report() {
    return discover_tools_best_effort();
}

// ─────────────────────────────────────────────────────────────────────────────
// §20  Notes on McpServerManagerError
//      .message is populated by every factory function.
//      .to_string() is defined inline in the header as { return message; }.
// ─────────────────────────────────────────────────────────────────────────────

} // namespace claw::runtime
