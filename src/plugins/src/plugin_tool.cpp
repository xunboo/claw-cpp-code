#include "plugin_tool.hpp"

#include <format>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace claw::plugins {

PluginTool::PluginTool(std::string plugin_id,
                       std::string plugin_name,
                       PluginToolDefinition definition,
                       std::string cmd,
                       std::vector<std::string> args_,
                       PluginToolPermission required_permission,
                       std::optional<std::filesystem::path> root_)
    : plugin_id_(std::move(plugin_id))
    , plugin_name_(std::move(plugin_name))
    , definition_(std::move(definition))
    , required_permission_(required_permission)
{
    command = std::move(cmd);
    args    = std::move(args_);
    root    = std::move(root_);
}

// ─── Process execution helper ─────────────────────────────────────────────────
//
// Spawns the tool command with piped stdin/stdout/stderr and the supplied
// environment extras.  Mirrors Rust PluginTool::execute() behaviour exactly.

namespace {

struct ProcResult {
    int exit_code{-1};
    std::string stdout_str;
    std::string stderr_str;
};

// Trim trailing whitespace (mirrors Rust str::trim())
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.pop_back();
    return s;
}

#ifdef _WIN32
ProcResult run_process(const std::string& cmd,
                       const std::vector<std::string>& extra_args,
                       const std::string& stdin_data,
                       const std::optional<std::filesystem::path>& cwd,
                       const std::vector<std::pair<std::string,std::string>>& env_extra) {

    // Create inheritable pipes
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE si_rd{}, si_wr{}, so_rd{}, so_wr{}, se_rd{}, se_wr{};
    CreatePipe(&si_rd, &si_wr, &sa, 0);
    CreatePipe(&so_rd, &so_wr, &sa, 0);
    CreatePipe(&se_rd, &se_wr, &sa, 0);
    // Parent-side handles must not be inherited
    SetHandleInformation(si_wr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(so_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(se_rd, HANDLE_FLAG_INHERIT, 0);

    // Build command line: cmd /C "<cmd>" [args...]
    std::string cmdline = "cmd /C \"" + cmd + "\"";
    for (auto& a : extra_args) cmdline += " \"" + a + "\"";

    // Build environment block (inherit current + extras)
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

    std::string cwd_str = cwd ? cwd->string() : "";

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(
        nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0,
        reinterpret_cast<LPVOID>(env_block.data()),
        cwd_str.empty() ? nullptr : cwd_str.c_str(),
        &si_info, &pi);

    // Close child-side pipe ends in parent immediately after spawn
    CloseHandle(si_rd);
    CloseHandle(so_wr);
    CloseHandle(se_wr);

    if (!ok) {
        CloseHandle(si_wr);
        CloseHandle(so_rd);
        CloseHandle(se_rd);
        return {-1, {}, "CreateProcess failed"};
    }

    // Write stdin data then close our write end so the child sees EOF
    if (!stdin_data.empty()) {
        DWORD written{};
        WriteFile(si_wr, stdin_data.data(), static_cast<DWORD>(stdin_data.size()),
                  &written, nullptr);
    }
    CloseHandle(si_wr);

    // Read stdout and stderr
    auto read_pipe = [](HANDLE h) -> std::string {
        std::string out; char buf[4096]; DWORD n{};
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
        CloseHandle(h);
        return out;
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

ProcResult run_process(const std::string& cmd,
                       const std::vector<std::string>& extra_args,
                       const std::string& stdin_data,
                       const std::optional<std::filesystem::path>& cwd,
                       const std::vector<std::pair<std::string,std::string>>& env_extra) {

    int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
    pipe(stdin_pipe);
    pipe(stdout_pipe);
    pipe(stderr_pipe);

    pid_t pid = fork();
    if (pid == 0) {
        // Child: wire up pipes
        dup2(stdin_pipe[0],  STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        for (int fd : {stdin_pipe[0], stdin_pipe[1],
                       stdout_pipe[0], stdout_pipe[1],
                       stderr_pipe[0], stderr_pipe[1]})
            close(fd);

        // Set extra environment variables
        for (auto& [k, v] : env_extra)
            setenv(k.c_str(), v.c_str(), 1);

        // Change working directory if requested
        if (cwd) chdir(cwd->c_str());

        // Determine how to invoke: file path → sh <path>, otherwise sh -lc <cmd>
        bool is_file = std::filesystem::exists(cmd);

        // Build argv: sh [args...] cmd [extra_args...]
        std::vector<const char*> argv;
        argv.push_back("sh");
        if (!is_file) argv.push_back("-lc");
        argv.push_back(cmd.c_str());
        for (auto& a : extra_args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp("sh", const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent: close child-side ends
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Write stdin then close so the child gets EOF
    if (!stdin_data.empty())
        ::write(stdin_pipe[1], stdin_data.data(), stdin_data.size());
    close(stdin_pipe[1]);

    auto read_fd = [](int fd) -> std::string {
        std::string out; char buf[4096]; ssize_t n{};
        while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, n);
        close(fd);
        return out;
    };
    std::string out = read_fd(stdout_pipe[0]);
    std::string err = read_fd(stderr_pipe[0]);

    int status{};
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {code, std::move(out), std::move(err)};
}

#endif  // _WIN32

}  // anonymous namespace

// ─── PluginTool::execute ──────────────────────────────────────────────────────
// Mirrors Rust PluginTool::execute(): serialises input to JSON, spawns the
// command with piped stdin/stdout/stderr, returns trimmed stdout on success or
// a PluginError::CommandFailed on non-zero exit.

tl::expected<std::string, PluginError>
PluginTool::execute(const nlohmann::json& input) const {
    const std::string input_json = input.dump();

    std::vector<std::pair<std::string,std::string>> env_extra = {
        {"CLAWD_PLUGIN_ID",   plugin_id_},
        {"CLAWD_PLUGIN_NAME", plugin_name_},
        {"CLAWD_TOOL_NAME",   definition_.name},
        {"CLAWD_TOOL_INPUT",  input_json},
    };
    if (root)
        env_extra.emplace_back("CLAWD_PLUGIN_ROOT", root->string());

    auto result = run_process(command, args, input_json, root, env_extra);
    std::string out = trim(result.stdout_str);
    std::string err = trim(result.stderr_str);

    if (result.exit_code == 0)
        return out;

    return tl::unexpected(PluginError::command_failed(std::format(
        "plugin tool `{}` from `{}` failed for `{}`: {}",
        definition_.name, plugin_id_, command,
        err.empty()
            ? std::format("exit status {}", result.exit_code)
            : err)));
}

}  // namespace claw::plugins
