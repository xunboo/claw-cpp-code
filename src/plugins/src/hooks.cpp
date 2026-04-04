#include "hooks.hpp"

#include <filesystem>
#include <format>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace claw::plugins {

// ─── HookRunResult ────────────────────────────────────────────────────────────

HookRunResult HookRunResult::allow(std::vector<std::string> messages) {
    HookRunResult r;
    r.messages_ = std::move(messages);
    return r;
}

HookRunResult HookRunResult::deny(std::vector<std::string> messages) {
    HookRunResult r;
    r.denied_   = true;
    r.messages_ = std::move(messages);
    return r;
}

HookRunResult HookRunResult::failed(std::vector<std::string> messages) {
    HookRunResult r;
    r.failed_   = true;
    r.messages_ = std::move(messages);
    return r;
}

// ─── HookRunner ───────────────────────────────────────────────────────────────

HookRunner::HookRunner(PluginHooks hooks) : hooks_(std::move(hooks)) {}

tl::expected<HookRunner, PluginError>
HookRunner::from_registry(const PluginRegistry& registry) {
    auto hooks = registry.aggregated_hooks();
    if (!hooks) return tl::unexpected(std::move(hooks.error()));
    return HookRunner{std::move(*hooks)};
}

HookRunResult HookRunner::run_pre_tool_use(
    std::string_view tool_name, std::string_view tool_input) const {
    return run_commands(HookEvent::PreToolUse,
                        hooks_.pre_tool_use,
                        tool_name, tool_input,
                        std::nullopt, false);
}

HookRunResult HookRunner::run_post_tool_use(
    std::string_view tool_name,
    std::string_view tool_input,
    std::string_view tool_output,
    bool is_error) const {
    return run_commands(HookEvent::PostToolUse,
                        hooks_.post_tool_use,
                        tool_name, tool_input,
                        tool_output, is_error);
}

HookRunResult HookRunner::run_post_tool_use_failure(
    std::string_view tool_name,
    std::string_view tool_input,
    std::string_view tool_error) const {
    return run_commands(HookEvent::PostToolUseFailure,
                        hooks_.post_tool_use_failure,
                        tool_name, tool_input,
                        tool_error, true);
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

namespace {

// Trim trailing ASCII whitespace — mirrors Rust str::trim() applied to output.
std::string trim_trailing(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.pop_back();
    return s;
}

// Build the JSON payload sent to each hook command on stdin.
// Mirrors Rust hook_payload() and parse_tool_input().
nlohmann::json hook_payload(
    HookEvent event,
    std::string_view tool_name,
    std::string_view tool_input,
    std::optional<std::string_view> tool_output,
    bool is_error) {

    // parse_tool_input: try to parse tool_input as JSON; fall back to {raw: <string>}
    nlohmann::json parsed_input;
    try {
        parsed_input = nlohmann::json::parse(tool_input);
    } catch (...) {
        parsed_input = {{"raw", std::string(tool_input)}};
    }

    if (event == HookEvent::PostToolUseFailure) {
        return {
            {"hook_event_name",      hook_event_str(event)},
            {"tool_name",            std::string(tool_name)},
            {"tool_input",           parsed_input},
            {"tool_input_json",      std::string(tool_input)},
            {"tool_error",           tool_output
                                         ? nlohmann::json(std::string(*tool_output))
                                         : nlohmann::json(nullptr)},
            {"tool_result_is_error", true},
        };
    }
    return {
        {"hook_event_name",      hook_event_str(event)},
        {"tool_name",            std::string(tool_name)},
        {"tool_input",           parsed_input},
        {"tool_input_json",      std::string(tool_input)},
        {"tool_output",          tool_output
                                     ? nlohmann::json(std::string(*tool_output))
                                     : nlohmann::json(nullptr)},
        {"tool_result_is_error", is_error},
    };
}

// Format a warning message for a hook that exited with a non-zero, non-2 code.
// Mirrors Rust format_hook_warning().
std::string format_hook_warning(std::string_view command,
                                int code,
                                std::optional<std::string_view> stdout_msg,
                                std::string_view stderr_msg) {
    std::string msg = std::format("Hook `{}` exited with status {}", command, code);
    if (stdout_msg && !stdout_msg->empty()) {
        msg += ": ";
        msg += *stdout_msg;
    } else if (!stderr_msg.empty()) {
        msg += ": ";
        msg += stderr_msg;
    }
    return msg;
}

// ── Process spawning ──────────────────────────────────────────────────────────
// Spawn a shell command with piped stdin/stdout/stderr and extra env vars.
// Mirrors Rust shell_command() + CommandWithStdin::output_with_stdin().

struct ProcResult { int code; std::string out; std::string err; };

#ifdef _WIN32
ProcResult spawn_shell(std::string_view command,
                       std::string_view stdin_data,
                       const std::vector<std::pair<std::string,std::string>>& env_extra) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE si_rd{}, si_wr{}, so_rd{}, so_wr{}, se_rd{}, se_wr{};
    CreatePipe(&si_rd, &si_wr, &sa, 0);
    CreatePipe(&so_rd, &so_wr, &sa, 0);
    CreatePipe(&se_rd, &se_wr, &sa, 0);
    // Parent-side ends must not be inherited
    SetHandleInformation(si_wr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(so_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(se_rd, HANDLE_FLAG_INHERIT, 0);

    // cmd /C <command>  — mirrors Rust shell_command() on Windows
    std::string cmdline = std::format("cmd /C {}", command);

    // Build environment block: current env + extras
    std::string env_block;
    {
        LPCH cur = GetEnvironmentStringsA();
        for (LPCH p = cur; *p; p += strlen(p) + 1)
            env_block += std::string(p) + '\0';
        FreeEnvironmentStringsA(cur);
        for (auto& [k, v] : env_extra)
            env_block += k + "=" + v + '\0';
        env_block += '\0';
    }

    STARTUPINFOA si_info{};
    si_info.cb         = sizeof(si_info);
    si_info.hStdInput  = si_rd;
    si_info.hStdOutput = so_wr;
    si_info.hStdError  = se_wr;
    si_info.dwFlags    = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(
        nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0,
        reinterpret_cast<LPVOID>(env_block.data()),
        nullptr, &si_info, &pi);

    // Close child-side handles in parent
    CloseHandle(si_rd);
    CloseHandle(so_wr);
    CloseHandle(se_wr);

    if (!ok) {
        CloseHandle(si_wr); CloseHandle(so_rd); CloseHandle(se_rd);
        return {-1, {}, "CreateProcess failed"};
    }

    // Write stdin, then close our write end so child sees EOF
    if (!stdin_data.empty()) {
        DWORD written{};
        WriteFile(si_wr, stdin_data.data(),
                  static_cast<DWORD>(stdin_data.size()), &written, nullptr);
    }
    CloseHandle(si_wr);

    auto read_pipe = [](HANDLE h) -> std::string {
        std::string buf; char tmp[4096]; DWORD n{};
        while (ReadFile(h, tmp, sizeof(tmp), &n, nullptr) && n > 0)
            buf.append(tmp, n);
        CloseHandle(h);
        return buf;
    };
    std::string out = read_pipe(so_rd);
    std::string err = read_pipe(se_rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code{};
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return {static_cast<int>(code), std::move(out), std::move(err)};
}

#else  // POSIX

ProcResult spawn_shell(std::string_view command,
                       std::string_view stdin_data,
                       const std::vector<std::pair<std::string,std::string>>& env_extra) {
    int si[2], so[2], se[2];
    pipe(si); pipe(so); pipe(se);

    pid_t pid = fork();
    if (pid == 0) {
        // Child: connect pipes
        dup2(si[0], STDIN_FILENO);
        dup2(so[1], STDOUT_FILENO);
        dup2(se[1], STDERR_FILENO);
        for (int fd : {si[0], si[1], so[0], so[1], se[0], se[1]}) close(fd);

        // Apply extra environment variables
        for (auto& [k, v] : env_extra)
            setenv(k.c_str(), v.c_str(), 1);

        // Determine how to invoke the command.
        // Mirrors Rust shell_command(): if the command string is an existing file,
        // run "sh <command>"; otherwise run "sh -lc <command>".
        std::string cmd_str{command};
        bool is_file = (command.starts_with("./")  ||
                        command.starts_with("../")  ||
                        std::filesystem::path(command).is_absolute()) &&
                       std::filesystem::exists(cmd_str);

        if (is_file)
            execl("/bin/sh", "sh", cmd_str.c_str(), nullptr);
        else
            execl("/bin/sh", "sh", "-lc", cmd_str.c_str(), nullptr);
        _exit(127);
    }

    // Parent: close child-side ends
    close(si[0]); close(so[1]); close(se[1]);

    // Write stdin then close so child sees EOF
    if (!stdin_data.empty())
        ::write(si[1], stdin_data.data(), stdin_data.size());
    close(si[1]);

    auto read_fd = [](int fd) -> std::string {
        std::string buf; char tmp[4096]; ssize_t n{};
        while ((n = ::read(fd, tmp, sizeof(tmp))) > 0) buf.append(tmp, n);
        close(fd);
        return buf;
    };
    std::string out = read_fd(so[0]);
    std::string err = read_fd(se[0]);

    int status{};
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {code, std::move(out), std::move(err)};
}

#endif  // _WIN32

}  // anonymous namespace

// ─── HookRunner::run_commands ─────────────────────────────────────────────────
// Mirrors Rust HookRunner::run_commands().

HookRunResult HookRunner::run_commands(
    HookEvent event,
    const std::vector<std::string>& commands,
    std::string_view tool_name,
    std::string_view tool_input,
    std::optional<std::string_view> tool_output,
    bool is_error) {

    if (commands.empty()) return HookRunResult::allow({});

    std::string payload =
        hook_payload(event, tool_name, tool_input, tool_output, is_error).dump();

    std::vector<std::string> messages;

    for (auto& command : commands) {
        auto outcome = run_command(command, event, tool_name, tool_input,
                                   tool_output, is_error, payload);
        switch (outcome.tag) {
            case CommandOutcome::Tag::Allow:
                if (outcome.message)
                    messages.push_back(std::move(*outcome.message));
                break;

            case CommandOutcome::Tag::Deny:
                if (outcome.message)
                    messages.push_back(std::move(*outcome.message));
                else
                    messages.push_back(std::format(
                        "{} hook denied tool `{}`",
                        hook_event_str(event), tool_name));
                return HookRunResult::deny(std::move(messages));

            case CommandOutcome::Tag::Failed:
                messages.push_back(std::move(outcome.failure_message));
                return HookRunResult::failed(std::move(messages));
        }
    }

    return HookRunResult::allow(std::move(messages));
}

// ─── HookRunner::run_command ──────────────────────────────────────────────────
// Mirrors Rust HookRunner::run_command().
// exit 0  → Allow
// exit 2  → Deny
// other   → Failed

HookRunner::CommandOutcome HookRunner::run_command(
    std::string_view command,
    HookEvent event,
    std::string_view tool_name,
    std::string_view tool_input,
    std::optional<std::string_view> tool_output,
    bool is_error,
    std::string_view payload) {

    std::vector<std::pair<std::string,std::string>> env_extra = {
        {"HOOK_EVENT",         hook_event_str(event)},
        {"HOOK_TOOL_NAME",     std::string(tool_name)},
        {"HOOK_TOOL_INPUT",    std::string(tool_input)},
        {"HOOK_TOOL_IS_ERROR", is_error ? "1" : "0"},
    };
    if (tool_output)
        env_extra.emplace_back("HOOK_TOOL_OUTPUT", std::string(*tool_output));

    try {
        auto [code, out_raw, err_raw] = spawn_shell(command, payload, env_extra);
        auto stdout_str = trim_trailing(out_raw);
        auto stderr_str = trim_trailing(err_raw);

        // stdout non-empty → carry as message
        std::optional<std::string> msg =
            stdout_str.empty()
                ? std::nullopt
                : std::make_optional(stdout_str);

        if (code == 0)
            return {CommandOutcome::Tag::Allow, std::move(msg), {}};

        if (code == 2)
            return {CommandOutcome::Tag::Deny, std::move(msg), {}};

        // Any other non-zero exit → failure
        std::string fail_msg = format_hook_warning(
            command, code,
            msg ? std::optional<std::string_view>(*msg) : std::nullopt,
            stderr_str);
        return {CommandOutcome::Tag::Failed, std::nullopt, std::move(fail_msg)};

    } catch (std::exception& ex) {
        return {
            CommandOutcome::Tag::Failed,
            std::nullopt,
            std::format("{} hook `{}` failed to start for `{}`: {}",
                        hook_event_str(event), command, tool_name, ex.what())
        };
    }
}

}  // namespace claw::plugins
