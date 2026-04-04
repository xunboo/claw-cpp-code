// =============================================================================
// hooks.cpp  –  C++20 faithful translation of
//               crates/runtime/src/hooks.rs  (987 lines)
//
// Every Rust fn / pub fn / impl method has a matching real C++ body.
// Rust constructs mapped:
//   Result<T,E>        → tl::expected<T,E>
//   Option<T>          → std::optional<T>
//   Vec<T>             → std::vector<T>
//   HashMap            → std::unordered_map
//   serde_json / json! → nlohmann::json
//   async fn           → regular function (synchronous)
//   Arc<AtomicBool>    → std::shared_ptr<std::atomic<bool>>
//   thread::sleep      → platform sleep (see below)
//   Command / spawn    → POSIX fork+exec  OR  Win32 CreateProcess
//
// namespace: runtime
// =============================================================================

#include "hooks.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>
#include <vector>
#include <optional>

// ---- platform headers -------------------------------------------------------
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace claw::runtime {

// =============================================================================
// Internal helpers  (not in the header – mirrors Rust private items)
// =============================================================================

// ---------------------------------------------------------------------------
// ParsedHookOutput  (mirrors Rust struct ParsedHookOutput)
// ---------------------------------------------------------------------------
struct ParsedHookOutput {
    std::vector<std::string>          messages;
    bool                              deny{false};
    std::optional<PermissionOverride> permission_override;
    std::optional<std::string>        permission_reason;
    std::optional<std::string>        updated_input;

    // Rust: fn with_fallback_message(mut self, fallback: String) -> Self
    ParsedHookOutput with_fallback_message(std::string fallback) && {
        if (messages.empty()) {
            messages.push_back(std::move(fallback));
        }
        return std::move(*this);
    }

    // Rust: fn primary_message(&self) -> Option<&str>
    [[nodiscard]] std::optional<std::string_view> primary_message() const noexcept {
        if (messages.empty()) return std::nullopt;
        return std::string_view{messages.front()};
    }
};

// ---------------------------------------------------------------------------
// HookCommandOutcome  (mirrors Rust enum HookCommandOutcome)
// ---------------------------------------------------------------------------
enum class HookCommandOutcomeKind { Allow, Deny, Failed, Cancelled };

struct HookCommandOutcome {
    HookCommandOutcomeKind kind;
    ParsedHookOutput       parsed;   // for Allow / Deny / Failed
    std::string            message;  // for Cancelled

    static HookCommandOutcome allow(ParsedHookOutput p) {
        return {HookCommandOutcomeKind::Allow, std::move(p), {}};
    }
    static HookCommandOutcome deny(ParsedHookOutput p) {
        return {HookCommandOutcomeKind::Deny, std::move(p), {}};
    }
    static HookCommandOutcome failed(ParsedHookOutput p) {
        return {HookCommandOutcomeKind::Failed, std::move(p), {}};
    }
    static HookCommandOutcome cancelled(std::string msg) {
        return {HookCommandOutcomeKind::Cancelled, {}, std::move(msg)};
    }
};

// ---------------------------------------------------------------------------
// merge_parsed_hook_output  (mirrors Rust fn merge_parsed_hook_output)
// ---------------------------------------------------------------------------
static void merge_parsed_hook_output(HookRunResult& target, ParsedHookOutput parsed) {
    for (auto& m : parsed.messages) {
        target.messages.push_back(std::move(m));
    }
    if (parsed.permission_override.has_value()) {
        target.permission_override = parsed.permission_override;
    }
    if (parsed.permission_reason.has_value()) {
        target.permission_reason = std::move(parsed.permission_reason);
    }
    if (parsed.updated_input.has_value()) {
        target.updated_input = std::move(parsed.updated_input);
    }
}

// ---------------------------------------------------------------------------
// parse_tool_input  (mirrors Rust fn parse_tool_input)
//
//   fn parse_tool_input(tool_input: &str) -> Value {
//       serde_json::from_str(tool_input)
//           .unwrap_or_else(|_| json!({ "raw": tool_input }))
//   }
// ---------------------------------------------------------------------------
static nlohmann::json parse_tool_input(const std::string& tool_input) {
    try {
        return nlohmann::json::parse(tool_input);
    } catch (...) {
        return nlohmann::json{{"raw", tool_input}};
    }
}

// ---------------------------------------------------------------------------
// hook_payload  (mirrors Rust fn hook_payload)
//
// PostToolUseFailure uses "tool_error" key; all others use "tool_output".
// ---------------------------------------------------------------------------
static nlohmann::json hook_payload(
    HookEvent           event,
    const std::string&  tool_name,
    const std::string&  tool_input,
    const std::optional<std::string>& tool_output,
    bool                is_error)
{
    nlohmann::json j;
    j["hook_event_name"]    = hook_event_as_str(event);
    j["tool_name"]          = tool_name;
    j["tool_input"]         = parse_tool_input(tool_input);
    j["tool_input_json"]    = tool_input;

    if (event == HookEvent::PostToolUseFailure) {
        // Rust: "tool_error": tool_output,  "tool_result_is_error": true
        if (tool_output.has_value()) {
            j["tool_error"] = *tool_output;
        } else {
            j["tool_error"] = nullptr;
        }
        j["tool_result_is_error"] = true;
    } else {
        // Rust: "tool_output": tool_output,  "tool_result_is_error": is_error
        if (tool_output.has_value()) {
            j["tool_output"] = *tool_output;
        } else {
            j["tool_output"] = nullptr;
        }
        j["tool_result_is_error"] = is_error;
    }

    return j;
}

// ---------------------------------------------------------------------------
// format_hook_failure  (mirrors Rust fn format_hook_failure)
//
//   fn format_hook_failure(command, code, stdout, stderr) -> String
// ---------------------------------------------------------------------------
static std::string format_hook_failure(
    const std::string&              command,
    int                             code,
    const std::optional<std::string>& stdout_msg,
    const std::string&              stderr_msg)
{
    std::string message = std::format("Hook `{}` exited with status {}", command, code);
    if (stdout_msg.has_value() && !stdout_msg->empty()) {
        message += ": ";
        message += *stdout_msg;
    } else if (!stderr_msg.empty()) {
        message += ": ";
        message += stderr_msg;
    }
    return message;
}

// ---------------------------------------------------------------------------
// parse_hook_output  (mirrors Rust fn parse_hook_output)
//
// Tries to decode stdout as JSON; falls back to treating the whole string
// as a plain text message.
// ---------------------------------------------------------------------------
static ParsedHookOutput parse_hook_output(const std::string& stdout_str) {
    if (stdout_str.empty()) {
        return ParsedHookOutput{};
    }

    // Try JSON parse
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(stdout_str);
    } catch (...) {
        // Not JSON: treat entire output as a message
        return ParsedHookOutput{
            .messages = {stdout_str},
        };
    }

    if (!root.is_object()) {
        return ParsedHookOutput{.messages = {stdout_str}};
    }

    ParsedHookOutput parsed;

    // systemMessage
    if (auto it = root.find("systemMessage"); it != root.end() && it->is_string()) {
        parsed.messages.push_back(it->get<std::string>());
    }

    // reason
    if (auto it = root.find("reason"); it != root.end() && it->is_string()) {
        parsed.messages.push_back(it->get<std::string>());
    }

    // "continue": false   OR   "decision": "block"  → deny
    {
        auto it_cont = root.find("continue");
        if (it_cont != root.end() && it_cont->is_boolean() && !it_cont->get<bool>()) {
            parsed.deny = true;
        }
        auto it_dec = root.find("decision");
        if (it_dec != root.end() && it_dec->is_string() && it_dec->get<std::string>() == "block") {
            parsed.deny = true;
        }
    }

    // hookSpecificOutput
    if (auto it_hso = root.find("hookSpecificOutput");
        it_hso != root.end() && it_hso->is_object())
    {
        const auto& specific = *it_hso;

        // additionalContext
        if (auto it = specific.find("additionalContext");
            it != specific.end() && it->is_string())
        {
            parsed.messages.push_back(it->get<std::string>());
        }

        // permissionDecision  → "allow" | "deny" | "ask"
        if (auto it = specific.find("permissionDecision");
            it != specific.end() && it->is_string())
        {
            const std::string& dec = it->get<std::string>();
            if (dec == "allow") {
                parsed.permission_override = PermissionOverride::Allow;
            } else if (dec == "deny") {
                parsed.permission_override = PermissionOverride::Deny;
            } else if (dec == "ask") {
                parsed.permission_override = PermissionOverride::Ask;
            }
            // unknown value → no override (mirrors Rust `_ => None`)
        }

        // permissionDecisionReason
        if (auto it = specific.find("permissionDecisionReason");
            it != specific.end() && it->is_string())
        {
            parsed.permission_reason = it->get<std::string>();
        }

        // updatedInput – stored as serialised JSON string
        if (auto it = specific.find("updatedInput"); it != specific.end()) {
            parsed.updated_input = it->dump();
        }
    }

    // If we still have no messages, fall back to the raw stdout
    if (parsed.messages.empty()) {
        parsed.messages.push_back(stdout_str);
    }

    return parsed;
}

// =============================================================================
// Platform-specific process execution
//
// Mirrors Rust's CommandWithStdin + CommandExecution + output_with_stdin().
// We run the command under a login shell:
//   POSIX:    sh -lc "<command>"
//   Windows:  cmd /C "<command>"
//
// Environment variables passed in via envp additions:
//   HOOK_EVENT, HOOK_TOOL_NAME, HOOK_TOOL_INPUT, HOOK_TOOL_IS_ERROR,
//   HOOK_TOOL_OUTPUT  (optional)
// =============================================================================

struct CommandOutput {
    int         exit_code{0};  // -1 means "killed by signal / unknown"
    std::string stdout_data;
    std::string stderr_data;
};

enum class CommandExecutionKind { Finished, Cancelled };

struct CommandExecution {
    CommandExecutionKind kind;
    CommandOutput        output; // valid only when kind == Finished
};

// ---- helper: trim whitespace in-place -------------------------------------
static void trim_inplace(std::string& s) {
    // trim right
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' ||
                          s.back() == '\r' || s.back() == '\t')) {
        s.pop_back();
    }
    // trim left
    std::size_t start = s.find_first_not_of(" \n\r\t");
    if (start == std::string::npos) { s.clear(); return; }
    if (start > 0) s.erase(0, start);
}

// ---- helper: drain an fd into a string (POSIX only) -----------------------
#ifndef _WIN32
static std::string drain_fd(int fd) {
    std::string buf;
    char tmp[4096];
    for (;;) {
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf.append(tmp, static_cast<std::size_t>(n));
    }
    return buf;
}
#endif

// ---------------------------------------------------------------------------
// run_shell_command
//
// Mirrors the Rust CommandWithStdin::output_with_stdin() polling loop.
// Sends `stdin_data` to the child's stdin, then polls every 20 ms while
// checking the abort signal.  Returns Cancelled if aborted.
//
// Extra env pairs are passed as { "KEY", "VALUE" } entries in `extra_env`.
// ---------------------------------------------------------------------------
static CommandExecution run_shell_command(
    const std::string&                           command_str,
    const std::string&                           stdin_data,
    const std::vector<std::pair<std::string, std::string>>& extra_env,
    const HookAbortSignal*                       abort_signal)
{
#ifdef _WIN32
    // -----------------------------------------------------------------------
    // Windows  – CreateProcess
    // -----------------------------------------------------------------------

    // Build environment block: current environment + extra_env.
    // We use LPCH (null-terminated concatenated strings, doubly null-terminated).
    std::vector<char> env_block;
    {
        // Copy existing environment
        LPCH existing = GetEnvironmentStrings();
        if (existing) {
            for (LPCH p = existing; *p; ) {
                std::size_t len = strlen(p);
                env_block.insert(env_block.end(), p, p + len + 1);
                p += len + 1;
            }
            FreeEnvironmentStrings(existing);
        }
        // Append extra env
        for (const auto& [k, v] : extra_env) {
            std::string kv = k + "=" + v;
            env_block.insert(env_block.end(), kv.begin(), kv.end());
            env_block.push_back('\0');
        }
        env_block.push_back('\0'); // double-null terminator
    }

    // cmd /C "<command>"
    std::string cmd_line = std::string("cmd /C ") + command_str;
    std::vector<char> cmd_buf(cmd_line.begin(), cmd_line.end());
    cmd_buf.push_back('\0');

    // Pipes for stdin/stdout/stderr
    HANDLE h_stdin_rd  = INVALID_HANDLE_VALUE;
    HANDLE h_stdin_wr  = INVALID_HANDLE_VALUE;
    HANDLE h_stdout_rd = INVALID_HANDLE_VALUE;
    HANDLE h_stdout_wr = INVALID_HANDLE_VALUE;
    HANDLE h_stderr_rd = INVALID_HANDLE_VALUE;
    HANDLE h_stderr_wr = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

    auto fail_io = [&]() -> CommandExecution {
        for (HANDLE h : {h_stdin_rd, h_stdin_wr, h_stdout_rd, h_stdout_wr, h_stderr_rd, h_stderr_wr})
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        CommandOutput o; o.exit_code = -1;
        o.stdout_data = "CreateProcess pipe setup failed";
        return {CommandExecutionKind::Finished, o};
    };

    if (!CreatePipe(&h_stdin_rd,  &h_stdin_wr,  &sa, 0)) return fail_io();
    if (!CreatePipe(&h_stdout_rd, &h_stdout_wr, &sa, 0)) return fail_io();
    if (!CreatePipe(&h_stderr_rd, &h_stderr_wr, &sa, 0)) return fail_io();

    // Ensure the read/write handles we keep are not inheritable
    SetHandleInformation(h_stdin_wr,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(h_stdout_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(h_stderr_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdInput   = h_stdin_rd;
    si.hStdOutput  = h_stdout_wr;
    si.hStdError   = h_stderr_wr;

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessA(
        nullptr, cmd_buf.data(),
        nullptr, nullptr,
        /*bInheritHandles=*/TRUE,
        /*dwCreationFlags=*/0,
        env_block.data(),
        nullptr,
        &si, &pi);

    // Close child-side handles
    CloseHandle(h_stdin_rd);
    CloseHandle(h_stdout_wr);
    CloseHandle(h_stderr_wr);

    if (!created) {
        for (HANDLE h : {h_stdin_wr, h_stdout_rd, h_stderr_rd})
            CloseHandle(h);
        CommandOutput o; o.exit_code = -1;
        o.stdout_data = "CreateProcess failed";
        return {CommandExecutionKind::Finished, o};
    }
    CloseHandle(pi.hThread);

    // Write stdin
    if (!stdin_data.empty()) {
        DWORD written = 0;
        WriteFile(h_stdin_wr, stdin_data.data(),
                  static_cast<DWORD>(stdin_data.size()), &written, nullptr);
    }
    CloseHandle(h_stdin_wr);
    h_stdin_wr = INVALID_HANDLE_VALUE;

    // Poll loop – mirrors Rust's 20 ms sleep loop
    for (;;) {
        if (abort_signal && abort_signal->is_aborted()) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(h_stdout_rd);
            CloseHandle(h_stderr_rd);
            return {CommandExecutionKind::Cancelled, {}};
        }

        DWORD wait_result = WaitForSingleObject(pi.hProcess, 20 /*ms*/);
        if (wait_result == WAIT_OBJECT_0) break;
        // WAIT_TIMEOUT: keep polling
    }

    // Collect stdout / stderr
    auto drain_handle = [](HANDLE h) -> std::string {
        std::string buf;
        char tmp[4096];
        DWORD avail = 0;
        while (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD to_read = std::min<DWORD>(avail, sizeof(tmp));
            DWORD n = 0;
            if (ReadFile(h, tmp, to_read, &n, nullptr) && n > 0)
                buf.append(tmp, n);
        }
        // Final blocking read
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(h, tmp, sizeof(tmp), &n, nullptr) || n == 0) break;
            buf.append(tmp, n);
        }
        return buf;
    };

    CommandOutput out;
    out.stdout_data = drain_handle(h_stdout_rd);
    out.stderr_data = drain_handle(h_stderr_rd);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    out.exit_code = static_cast<int>(exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(h_stdout_rd);
    CloseHandle(h_stderr_rd);

    trim_inplace(out.stdout_data);
    trim_inplace(out.stderr_data);

    return {CommandExecutionKind::Finished, std::move(out)};

#else
    // -----------------------------------------------------------------------
    // POSIX  – fork + exec
    // -----------------------------------------------------------------------

    // stdin pipe  [0]=read  [1]=write
    // stdout pipe [0]=read  [1]=write
    // stderr pipe [0]=read  [1]=write
    int p_stdin[2]  = {-1, -1};
    int p_stdout[2] = {-1, -1};
    int p_stderr[2] = {-1, -1};

    auto fail_pipes = [&]() -> CommandExecution {
        for (int* p : {p_stdin, p_stdout, p_stderr}) {
            if (p[0] != -1) ::close(p[0]);
            if (p[1] != -1) ::close(p[1]);
        }
        CommandOutput o; o.exit_code = -1;
        o.stdout_data = "pipe() failed";
        return {CommandExecutionKind::Finished, o};
    };

    if (::pipe(p_stdin)  == -1) return fail_pipes();
    if (::pipe(p_stdout) == -1) return fail_pipes();
    if (::pipe(p_stderr) == -1) return fail_pipes();

    pid_t pid = ::fork();
    if (pid == -1) {
        // fork failed
        for (int* p : {p_stdin, p_stdout, p_stderr}) {
            ::close(p[0]); ::close(p[1]);
        }
        CommandOutput o; o.exit_code = -1;
        o.stdout_data = "fork() failed";
        return {CommandExecutionKind::Finished, o};
    }

    if (pid == 0) {
        // --- child ---
        // Wire up pipes
        ::dup2(p_stdin[0],  STDIN_FILENO);
        ::dup2(p_stdout[1], STDOUT_FILENO);
        ::dup2(p_stderr[1], STDERR_FILENO);

        // Close all pipe ends we inherited
        for (int* p : {p_stdin, p_stdout, p_stderr}) {
            ::close(p[0]); ::close(p[1]);
        }

        // Set extra environment variables
        for (const auto& [k, v] : extra_env) {
            ::setenv(k.c_str(), v.c_str(), /*overwrite=*/1);
        }

        // sh -lc "<command>"
        ::execl("/bin/sh", "sh", "-lc", command_str.c_str(),
                static_cast<char*>(nullptr));
        // If exec fails:
        ::_exit(127);
    }

    // --- parent ---
    // Close child-side ends
    ::close(p_stdin[0]);
    ::close(p_stdout[1]);
    ::close(p_stderr[1]);

    // Write stdin (non-blocking attempt; small payloads fit in pipe buffer)
    if (!stdin_data.empty()) {
        // Make the write end non-blocking to avoid deadlock on large payloads
        int flags = ::fcntl(p_stdin[1], F_GETFL, 0);
        ::fcntl(p_stdin[1], F_SETFL, flags | O_NONBLOCK);

        const char* ptr = stdin_data.data();
        std::size_t remaining = stdin_data.size();
        while (remaining > 0) {
            ssize_t n = ::write(p_stdin[1], ptr, remaining);
            if (n > 0) {
                ptr       += n;
                remaining -= static_cast<std::size_t>(n);
            } else if (n == -1 && errno == EAGAIN) {
                // Pipe full; wait a bit
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                break; // broken pipe or error
            }
        }
    }
    ::close(p_stdin[1]);

    // Set stdout/stderr read ends to non-blocking
    {
        int f = ::fcntl(p_stdout[0], F_GETFL, 0);
        ::fcntl(p_stdout[0], F_SETFL, f | O_NONBLOCK);
    }
    {
        int f = ::fcntl(p_stderr[0], F_GETFL, 0);
        ::fcntl(p_stderr[0], F_SETFL, f | O_NONBLOCK);
    }

    // Collect output while polling for process exit (mirrors Rust 20 ms loop)
    std::string stdout_buf;
    std::string stderr_buf;
    char tmp[4096];

    auto drain = [&](int fd, std::string& buf) {
        for (;;) {
            ssize_t n = ::read(fd, tmp, sizeof(tmp));
            if (n > 0) buf.append(tmp, static_cast<std::size_t>(n));
            else break;
        }
    };

    int wstatus   = 0;
    int exit_code = -1;
    for (;;) {
        // Check abort signal first (mirrors Rust abort_signal.is_aborted())
        if (abort_signal && abort_signal->is_aborted()) {
            ::kill(pid, SIGTERM);
            ::waitpid(pid, &wstatus, 0);
            ::close(p_stdout[0]);
            ::close(p_stderr[0]);
            return {CommandExecutionKind::Cancelled, {}};
        }

        // Drain any available output
        drain(p_stdout[0], stdout_buf);
        drain(p_stderr[0], stderr_buf);

        // Non-blocking wait
        pid_t waited = ::waitpid(pid, &wstatus, WNOHANG);
        if (waited == pid) {
            // Process exited – do a final drain
            drain(p_stdout[0], stdout_buf);
            drain(p_stderr[0], stderr_buf);
            if (WIFEXITED(wstatus)) {
                exit_code = WEXITSTATUS(wstatus);
            } else if (WIFSIGNALED(wstatus)) {
                exit_code = -1; // killed by signal → matches Rust `None` code path
            }
            break;
        }

        // Sleep 20 ms (mirrors Rust thread::sleep(Duration::from_millis(20)))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ::close(p_stdout[0]);
    ::close(p_stderr[0]);

    trim_inplace(stdout_buf);
    trim_inplace(stderr_buf);

    CommandOutput out;
    out.exit_code    = exit_code;
    out.stdout_data  = std::move(stdout_buf);
    out.stderr_data  = std::move(stderr_buf);

    return {CommandExecutionKind::Finished, std::move(out)};
#endif
}

// =============================================================================
// HookRunner::run_command  (mirrors Rust HookRunner::run_command)
//
// Builds the child process, feeds it the JSON payload on stdin, interprets
// the exit code and stdout according to the Rust rules:
//
//   exit 0  → Allow (unless parsed output says deny)
//   exit 2  → Deny  with fallback message
//   other   → Failed with fallback message
//   signal  → Failed with "terminated by signal" message
//   aborted → Cancelled
// =============================================================================
static HookCommandOutcome run_command_impl(
    const std::string&               command,
    HookEvent                        event,
    const std::string&               tool_name,
    const std::string&               tool_input,
    const std::optional<std::string>& tool_output,
    bool                             is_error,
    const std::string&               payload,
    const HookAbortSignal*           abort_signal)
{
    // Build environment additions  (mirrors Rust child.env("HOOK_*", ...))
    std::vector<std::pair<std::string,std::string>> env_extras;
    env_extras.emplace_back("HOOK_EVENT",         hook_event_as_str(event));
    env_extras.emplace_back("HOOK_TOOL_NAME",     tool_name);
    env_extras.emplace_back("HOOK_TOOL_INPUT",    tool_input);
    env_extras.emplace_back("HOOK_TOOL_IS_ERROR", is_error ? "1" : "0");
    if (tool_output.has_value()) {
        env_extras.emplace_back("HOOK_TOOL_OUTPUT", *tool_output);
    }

    // Execute
    CommandExecution exec = run_shell_command(command, payload, env_extras, abort_signal);

    // Cancelled
    if (exec.kind == CommandExecutionKind::Cancelled) {
        return HookCommandOutcome::cancelled(
            std::format("{} hook `{}` cancelled while handling `{}`",
                        hook_event_as_str(event), command, tool_name));
    }

    // Finished
    const CommandOutput& out = exec.output;

    ParsedHookOutput parsed = parse_hook_output(out.stdout_data);
    std::optional<std::string> primary_msg;
    if (auto pm = parsed.primary_message()) {
        primary_msg = std::string{*pm};
    }

    if (out.exit_code == 0) {
        // Rust: if parsed.deny → Deny, else Allow
        if (parsed.deny) {
            return HookCommandOutcome::deny(std::move(parsed));
        }
        return HookCommandOutcome::allow(std::move(parsed));
    }
    if (out.exit_code == 2) {
        // Rust: HookCommandOutcome::Deny { parsed: parsed.with_fallback_message(...) }
        std::string fallback = std::format("{} hook denied tool `{}`",
                                           hook_event_as_str(event), tool_name);
        return HookCommandOutcome::deny(
            std::move(parsed).with_fallback_message(std::move(fallback)));
    }
    if (out.exit_code == -1) {
        // Killed by signal (Rust: None code path)
        std::string fallback = std::format(
            "{} hook `{}` terminated by signal while handling `{}`",
            hook_event_as_str(event), command, tool_name);
        return HookCommandOutcome::failed(
            std::move(parsed).with_fallback_message(std::move(fallback)));
    }

    // Non-zero, non-2 exit (Rust: Some(code) → Failed)
    {
        std::string fallback = format_hook_failure(
            command,
            out.exit_code,
            primary_msg,
            out.stderr_data);
        return HookCommandOutcome::failed(
            std::move(parsed).with_fallback_message(std::move(fallback)));
    }
}

// =============================================================================
// HookRunner::run_commands  (mirrors Rust HookRunner::run_commands)
//
// Iterates over `commands`.  For each command:
//   Allow   → merge output, continue
//   Deny    → merge output, set denied = true, return early
//   Failed  → merge output, set failed = true, return early
//   Cancelled → set cancelled = true, push message, return early
// =============================================================================
/*static*/
HookRunResult HookRunner::run_commands(
    HookEvent                        event,
    const std::vector<std::string>&  commands,
    const std::string&               tool_name,
    const std::string&               tool_input,
    const std::optional<std::string>& tool_output,
    bool                             is_error,
    const HookAbortSignal*           abort_signal,
    HookProgressReporter*            reporter)
{
    // Rust: if commands.is_empty() { return HookRunResult::allow(Vec::new()); }
    if (commands.empty()) {
        return HookRunResult::allow();
    }

    // Rust: if abort_signal.is_some_and(HookAbortSignal::is_aborted) { ... }
    if (abort_signal && abort_signal->is_aborted()) {
        HookRunResult r;
        r.cancelled = true;
        r.messages.push_back(
            std::format("{} hook cancelled before execution",
                        hook_event_as_str(event)));
        return r;
    }

    // Build the JSON payload once for all commands
    const std::string payload =
        hook_payload(event, tool_name, tool_input, tool_output, is_error).dump();

    HookRunResult result = HookRunResult::allow();

    for (const std::string& command : commands) {
        // Report Started
        if (reporter) {
            reporter->on_event(HookProgressEvent{
                HookProgressKind::Started,
                event,
                tool_name,
                command,
            });
        }

        HookCommandOutcome outcome = run_command_impl(
            command, event,
            tool_name, tool_input, tool_output,
            is_error,
            payload,
            abort_signal);

        switch (outcome.kind) {
            case HookCommandOutcomeKind::Allow: {
                if (reporter) {
                    reporter->on_event(HookProgressEvent{
                        HookProgressKind::Completed, event, tool_name, command});
                }
                merge_parsed_hook_output(result, std::move(outcome.parsed));
                break;
            }
            case HookCommandOutcomeKind::Deny: {
                if (reporter) {
                    reporter->on_event(HookProgressEvent{
                        HookProgressKind::Completed, event, tool_name, command});
                }
                merge_parsed_hook_output(result, std::move(outcome.parsed));
                result.denied = true;
                return result;
            }
            case HookCommandOutcomeKind::Failed: {
                if (reporter) {
                    reporter->on_event(HookProgressEvent{
                        HookProgressKind::Completed, event, tool_name, command});
                }
                merge_parsed_hook_output(result, std::move(outcome.parsed));
                result.failed = true;
                return result;
            }
            case HookCommandOutcomeKind::Cancelled: {
                if (reporter) {
                    reporter->on_event(HookProgressEvent{
                        HookProgressKind::Cancelled, event, tool_name, command});
                }
                result.cancelled = true;
                result.messages.push_back(std::move(outcome.message));
                return result;
            }
        }
    }

    return result;
}

// =============================================================================
// HookRunner public methods  (mirrors every pub fn in Rust impl HookRunner)
// =============================================================================

// ---------------------------------------------------------------------------
// run_pre_tool_use
// ---------------------------------------------------------------------------
HookRunResult HookRunner::run_pre_tool_use(
    const std::string& tool_name,
    const std::string& tool_input) const
{
    return run_pre_tool_use_with_context(tool_name, tool_input, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// run_pre_tool_use_with_signal
// ---------------------------------------------------------------------------
HookRunResult HookRunner::run_pre_tool_use_with_signal(
    const std::string&      tool_name,
    const std::string&      tool_input,
    const HookAbortSignal*  abort_signal) const
{
    return run_pre_tool_use_with_context(tool_name, tool_input, abort_signal, nullptr);
}

// ---------------------------------------------------------------------------
// run_pre_tool_use_with_context
// ---------------------------------------------------------------------------
HookRunResult HookRunner::run_pre_tool_use_with_context(
    const std::string&      tool_name,
    const std::string&      tool_input,
    const HookAbortSignal*  abort_signal,
    HookProgressReporter*   reporter) const
{
    return run_commands(
        HookEvent::PreToolUse,
        config_.pre_tool_use,
        tool_name,
        tool_input,
        std::nullopt,   // tool_output = None
        false,          // is_error = false
        abort_signal,
        reporter);
}

// ---------------------------------------------------------------------------
// run_post_tool_use
// ---------------------------------------------------------------------------
HookRunResult HookRunner::run_post_tool_use(
    const std::string& tool_name,
    const std::string& tool_input,
    const std::string& tool_output,
    bool               is_error) const
{
    return run_post_tool_use_with_context(
        tool_name, tool_input, tool_output, is_error, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// run_post_tool_use_with_signal
// ---------------------------------------------------------------------------
HookRunResult HookRunner::run_post_tool_use_with_signal(
    const std::string&      tool_name,
    const std::string&      tool_input,
    const std::string&      tool_output,
    bool                    is_error,
    const HookAbortSignal*  abort_signal) const
{
    return run_post_tool_use_with_context(
        tool_name, tool_input, tool_output, is_error, abort_signal, nullptr);
}

// ---------------------------------------------------------------------------
// run_post_tool_use_with_context
// ---------------------------------------------------------------------------
HookRunResult HookRunner::run_post_tool_use_with_context(
    const std::string&      tool_name,
    const std::string&      tool_input,
    const std::string&      tool_output,
    bool                    is_error,
    const HookAbortSignal*  abort_signal,
    HookProgressReporter*   reporter) const
{
    return run_commands(
        HookEvent::PostToolUse,
        config_.post_tool_use,
        tool_name,
        tool_input,
        tool_output,    // Some(tool_output)
        is_error,
        abort_signal,
        reporter);
}

// ---------------------------------------------------------------------------
// run_post_tool_use_failure
// ---------------------------------------------------------------------------
HookRunResult HookRunner::run_post_tool_use_failure(
    const std::string& tool_name,
    const std::string& tool_input,
    const std::string& tool_error) const
{
    return run_post_tool_use_failure_with_context(
        tool_name, tool_input, tool_error, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// run_post_tool_use_failure_with_signal
// ---------------------------------------------------------------------------
HookRunResult HookRunner::run_post_tool_use_failure_with_signal(
    const std::string&      tool_name,
    const std::string&      tool_input,
    const std::string&      tool_error,
    const HookAbortSignal*  abort_signal) const
{
    return run_post_tool_use_failure_with_context(
        tool_name, tool_input, tool_error, abort_signal, nullptr);
}

// ---------------------------------------------------------------------------
// run_post_tool_use_failure_with_context
// ---------------------------------------------------------------------------
HookRunResult HookRunner::run_post_tool_use_failure_with_context(
    const std::string&      tool_name,
    const std::string&      tool_input,
    const std::string&      tool_error,
    const HookAbortSignal*  abort_signal,
    HookProgressReporter*   reporter) const
{
    return run_commands(
        HookEvent::PostToolUseFailure,
        config_.post_tool_use_failure,
        tool_name,
        tool_input,
        tool_error,     // Some(tool_error) – used as "tool_error" in payload
        true,           // is_error = true (always for failure hooks)
        abort_signal,
        reporter);
}

} // namespace claw::runtime
