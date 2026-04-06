#include "plugin.hpp"

#include <format>
#include <span>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace claw::plugins {

// ─── Path validation helpers ──────────────────────────────────────────────────

namespace {

// Returns true when entry is a "literal" shell command (not a relative/absolute
// file path), matching Rust's is_literal_command().
bool is_literal_command(std::string_view entry) noexcept {
    return !entry.starts_with("./") && !entry.starts_with("../") &&
           !std::filesystem::path(entry).is_absolute();
}

tl::expected<void, PluginError> validate_command_path(
    const std::filesystem::path& root,
    std::string_view entry,
    std::string_view kind) {

    if (is_literal_command(entry)) return {};

    std::filesystem::path path = std::filesystem::path(entry).is_absolute()
        ? std::filesystem::path(entry)
        : root / entry;

    if (!std::filesystem::exists(path))
        return tl::unexpected(PluginError::invalid_manifest(
            std::format("{} path `{}` does not exist", kind, path.string())));

    if (!std::filesystem::is_regular_file(path))
        return tl::unexpected(PluginError::invalid_manifest(
            std::format("{} path `{}` must point to a file", kind, path.string())));

    return {};
}

tl::expected<void, PluginError> validate_hook_paths(
    const std::optional<std::filesystem::path>& root,
    const PluginHooks& hooks) {

    if (!root) return {};
    for (auto& e : hooks.pre_tool_use)
        if (auto r = validate_command_path(*root, e, "hook"); !r) return r;
    for (auto& e : hooks.post_tool_use)
        if (auto r = validate_command_path(*root, e, "hook"); !r) return r;
    for (auto& e : hooks.post_tool_use_failure)
        if (auto r = validate_command_path(*root, e, "hook"); !r) return r;
    return {};
}

tl::expected<void, PluginError> validate_lifecycle_paths(
    const std::optional<std::filesystem::path>& root,
    const PluginLifecycle& lifecycle) {

    if (!root) return {};
    for (auto& e : lifecycle.init)
        if (auto r = validate_command_path(*root, e, "lifecycle command"); !r) return r;
    for (auto& e : lifecycle.shutdown)
        if (auto r = validate_command_path(*root, e, "lifecycle command"); !r) return r;
    return {};
}

tl::expected<void, PluginError> validate_tool_paths(
    const std::optional<std::filesystem::path>& root,
    std::span<const PluginTool> tools) {

    if (!root) return {};
    for (auto& tool : tools)
        if (auto r = validate_command_path(*root, tool.command, "tool"); !r) return r;
    return {};
}

// ─── Lifecycle command runner ─────────────────────────────────────────────────
// Mirrors Rust run_lifecycle_commands(): captures stdout+stderr via pipes so
// failure messages are reported correctly.

struct ProcResult { int code; std::string out; std::string err; };

#ifdef _WIN32
ProcResult run_lifecycle_proc(const std::string& command,
                              const std::optional<std::filesystem::path>& cwd) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE so_rd{}, so_wr{}, se_rd{}, se_wr{};
    CreatePipe(&so_rd, &so_wr, &sa, 0);
    CreatePipe(&se_rd, &se_wr, &sa, 0);
    SetHandleInformation(so_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(se_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput  = so_wr;
    si.hStdError   = se_wr;
    si.dwFlags     = STARTF_USESTDHANDLES;

    // cmd /C "<command>" — mirrors Rust shell_command() on Windows.
    // The command is a shell command string, so we pass it to cmd.exe for interpretation.
    std::string cmdline = std::format("cmd /C \"{}\"", command);
    std::string cwd_str = cwd ? cwd->string() : "";

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr,
                             cwd_str.empty() ? nullptr : cwd_str.c_str(), &si, &pi);
    CloseHandle(so_wr);
    CloseHandle(se_wr);

    if (!ok) {
        CloseHandle(so_rd); CloseHandle(se_rd);
        return {-1, {}, "CreateProcess failed"};
    }

    auto read_pipe = [](HANDLE h) -> std::string {
        std::string buf; char tmp[4096]; DWORD n{};
        while (ReadFile(h, tmp, sizeof(tmp), &n, nullptr) && n > 0) buf.append(tmp, n);
        CloseHandle(h); return buf;
    };
    std::string out = read_pipe(so_rd);
    std::string err = read_pipe(se_rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code{}; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return {static_cast<int>(code), std::move(out), std::move(err)};
}
#else
ProcResult run_lifecycle_proc(const std::string& command,
                              const std::optional<std::filesystem::path>& cwd) {
    int so[2], se[2];
    pipe(so); pipe(se);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(so[1], STDOUT_FILENO);
        dup2(se[1], STDERR_FILENO);
        for (int fd : {so[0], so[1], se[0], se[1]}) close(fd);
        if (cwd) chdir(cwd->c_str());
        bool is_file = std::filesystem::exists(command);
        if (is_file) execl("/bin/sh", "sh", command.c_str(), nullptr);
        else         execl("/bin/sh", "sh", "-lc", command.c_str(), nullptr);
        _exit(127);
    }
    close(so[1]); close(se[1]);
    auto read_fd = [](int fd) -> std::string {
        std::string buf; char tmp[4096]; ssize_t n{};
        while ((n = ::read(fd, tmp, sizeof(tmp))) > 0) buf.append(tmp, n);
        close(fd); return buf;
    };
    std::string out = read_fd(so[0]);
    std::string err = read_fd(se[0]);
    int status{}; waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {code, std::move(out), std::move(err)};
}
#endif

// Trim trailing whitespace (matching Rust .trim())
std::string trim_str(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.pop_back();
    return s;
}

tl::expected<void, PluginError> run_lifecycle_commands(
    const PluginMetadata& metadata,
    const PluginLifecycle& lifecycle,
    std::string_view phase,
    const std::vector<std::string>& commands) {

    if (lifecycle.is_empty() || commands.empty()) return {};

    for (auto& command : commands) {
        auto result = run_lifecycle_proc(command, metadata.root);
        if (result.code != 0) {
            std::string stderr_msg = trim_str(result.err);
            return tl::unexpected(PluginError::command_failed(std::format(
                "plugin `{}` {} failed for `{}`: {}",
                metadata.id, phase, command,
                stderr_msg.empty()
                    ? std::format("exit status {}", result.code)
                    : stderr_msg)));
        }
    }
    return {};
}

}  // anonymous namespace

// ─── BundledPlugin ────────────────────────────────────────────────────────────

tl::expected<void, PluginError> BundledPlugin::validate() const {
    if (auto r = validate_hook_paths(metadata_.root, hooks_); !r) return r;
    if (auto r = validate_lifecycle_paths(metadata_.root, lifecycle_); !r) return r;
    return validate_tool_paths(metadata_.root, std::span{tools_});
}

tl::expected<void, PluginError> BundledPlugin::initialize() const {
    return run_lifecycle_commands(metadata_, lifecycle_, "init", lifecycle_.init);
}

tl::expected<void, PluginError> BundledPlugin::shutdown() const {
    return run_lifecycle_commands(metadata_, lifecycle_, "shutdown", lifecycle_.shutdown);
}

// ─── ExternalPlugin ───────────────────────────────────────────────────────────

tl::expected<void, PluginError> ExternalPlugin::validate() const {
    if (auto r = validate_hook_paths(metadata_.root, hooks_); !r) return r;
    if (auto r = validate_lifecycle_paths(metadata_.root, lifecycle_); !r) return r;
    return validate_tool_paths(metadata_.root, std::span{tools_});
}

tl::expected<void, PluginError> ExternalPlugin::initialize() const {
    return run_lifecycle_commands(metadata_, lifecycle_, "init", lifecycle_.init);
}

tl::expected<void, PluginError> ExternalPlugin::shutdown() const {
    return run_lifecycle_commands(metadata_, lifecycle_, "shutdown", lifecycle_.shutdown);
}

// ─── PluginDefinition dispatch helpers ────────────────────────────────────────
// Mirror Rust `impl Plugin for PluginDefinition`

const PluginMetadata& plugin_metadata(const PluginDefinition& d) noexcept {
    return std::visit([](auto& p) -> const PluginMetadata& { return p.metadata_; }, d);
}

const PluginHooks& plugin_hooks(const PluginDefinition& d) noexcept {
    return std::visit([](auto& p) -> const PluginHooks& { return p.hooks_; }, d);
}

const PluginLifecycle& plugin_lifecycle(const PluginDefinition& d) noexcept {
    return std::visit([](auto& p) -> const PluginLifecycle& { return p.lifecycle_; }, d);
}

std::span<const PluginTool> plugin_tools(const PluginDefinition& d) noexcept {
    return std::visit([](auto& p) -> std::span<const PluginTool> {
        return std::span{p.tools_};
    }, d);
}

tl::expected<void, PluginError> plugin_validate(const PluginDefinition& d) {
    return std::visit([](auto& p) { return p.validate(); }, d);
}

tl::expected<void, PluginError> plugin_initialize(const PluginDefinition& d) {
    return std::visit([](auto& p) { return p.initialize(); }, d);
}

tl::expected<void, PluginError> plugin_shutdown(const PluginDefinition& d) {
    return std::visit([](auto& p) { return p.shutdown(); }, d);
}

// ─── PluginLoadFailure ────────────────────────────────────────────────────────

PluginLoadFailure::PluginLoadFailure(std::filesystem::path root,
                                     PluginKind k,
                                     std::string src,
                                     PluginError err)
    : plugin_root(std::move(root))
    , kind(k)
    , source(std::move(src))
    , error(std::make_unique<PluginError>(std::move(err)))
{}

std::string PluginLoadFailure::to_string() const {
    return std::format("failed to load {} plugin from `{}` (source: {}): {}",
        plugin_kind_str(kind), plugin_root.string(), source, error->what());
}

// ─── RegisteredPlugin ─────────────────────────────────────────────────────────

RegisteredPlugin::RegisteredPlugin(PluginDefinition def, bool enabled)
    : definition(std::move(def)), enabled_(enabled) {}

const PluginMetadata& RegisteredPlugin::metadata() const noexcept {
    return plugin_metadata(definition);
}

const PluginHooks& RegisteredPlugin::hooks() const noexcept {
    return plugin_hooks(definition);
}

std::span<const PluginTool> RegisteredPlugin::tools() const noexcept {
    return plugin_tools(definition);
}

tl::expected<void, PluginError> RegisteredPlugin::validate() const {
    return plugin_validate(definition);
}

tl::expected<void, PluginError> RegisteredPlugin::initialize() const {
    return plugin_initialize(definition);
}

tl::expected<void, PluginError> RegisteredPlugin::shutdown() const {
    return plugin_shutdown(definition);
}

PluginSummary RegisteredPlugin::summary() const {
    return PluginSummary{metadata(), enabled_};
}

// ─── builtin_plugins ─────────────────────────────────────────────────────────
// Mirrors Rust builtin_plugins() — returns the single example-builtin entry.

std::vector<PluginDefinition> builtin_plugins() {
    BuiltinPlugin p;
    p.metadata_ = PluginMetadata{
        .id              = std::string("example-builtin@") + BUILTIN_MARKETPLACE,
        .name            = "example-builtin",
        .version         = "0.1.0",
        .description     = "Example built-in plugin scaffold for the C++ plugin system",
        .kind            = PluginKind::Builtin,
        .source          = BUILTIN_MARKETPLACE,
        .default_enabled = false,
        .root            = std::nullopt,
    };
    std::vector<PluginDefinition> result;
    result.emplace_back(std::move(p));
    return result;
}

}  // namespace claw::plugins
