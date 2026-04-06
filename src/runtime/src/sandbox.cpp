// sandbox.cpp - C++20 faithful conversion of sandbox.rs
#include "sandbox.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <sched.h>
#endif

namespace claw::runtime {

namespace fs = std::filesystem;

static bool extract_command_exists(const std::string& command) {
#ifdef _WIN32
    return false; // Not needed on windows for Linux Sandbox
#else
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;
    std::string path_str(path_env);
    
    std::size_t start = 0;
    while (start < path_str.length()) {
        std::size_t end = path_str.find(':', start);
        if (end == std::string::npos) end = path_str.length();
        std::string dir = path_str.substr(start, end - start);
        if (!dir.empty()) {
            if (fs::exists(fs::path(dir) / command)) {
                return true;
            }
        }
        start = end + 1;
    }
    return false;
#endif
}

bool unshare_user_namespace_works() {
#ifdef _WIN32
    return false;
#else
    static bool result = false;
    static std::once_flag flag;
    std::call_once(flag, []() {
        if (!extract_command_exists("unshare")) {
            result = false;
            return;
        }
        
        pid_t pid = ::fork();
        if (pid == 0) {
            // child
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            const char* argv[] = {"unshare", "--user", "--map-root-user", "true", nullptr};
            execvp("unshare", const_cast<char**>(argv));
            ::_exit(1);
        } else if (pid > 0) {
            int status;
            ::waitpid(pid, &status, 0);
            result = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    });
    return result;
#endif
}

SandboxRequest resolve_request(
    const SandboxConfig& config,
    std::optional<bool> enabled_override,
    std::optional<bool> namespace_override,
    std::optional<bool> network_override,
    std::optional<FilesystemIsolationMode> filesystem_mode_override,
    std::optional<std::vector<std::string>> allowed_mounts_override) {
    SandboxRequest req;
    req.enabled = enabled_override.value_or(config.enabled.value_or(true));
    req.namespace_restrictions = namespace_override.value_or(config.namespace_restrictions.value_or(true));
    req.network_isolation = network_override.value_or(config.network_isolation.value_or(false));
    
    if (filesystem_mode_override.has_value()) {
        req.filesystem_mode = *filesystem_mode_override;
    } else if (config.filesystem_mode.has_value()) {
        req.filesystem_mode = *config.filesystem_mode;
    } else {
        req.filesystem_mode = default_isolation_mode();
    }
    
    req.allowed_mounts = allowed_mounts_override.value_or(config.allowed_mounts);
    return req;
}

ContainerEnvironment detect_container_environment_from(const SandboxDetectionInputs& inputs) {
    std::vector<std::string> markers;
    if (inputs.dockerenv_exists) markers.push_back("/.dockerenv");
    if (inputs.containerenv_exists) markers.push_back("/run/.containerenv");
    
    for (const auto& [key, value] : inputs.env_pairs) {
        std::string normalized = key;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
        if ((normalized == "container" || normalized == "docker" || 
             normalized == "podman" || normalized == "kubernetes_service_host") && !value.empty()) {
            markers.push_back("env:" + key + "=" + value);
        }
    }
    
    if (inputs.proc_1_cgroup.has_value()) {
        std::string cgroup = *inputs.proc_1_cgroup;
        const char* needles[] = {"docker", "containerd", "kubepods", "podman", "libpod"};
        for (const char* needle : needles) {
            if (cgroup.find(needle) != std::string::npos) {
                markers.push_back(std::string("/proc/1/cgroup:") + needle);
            }
        }
    }
    
    std::sort(markers.begin(), markers.end());
    auto last = std::unique(markers.begin(), markers.end());
    markers.erase(last, markers.end());
    
    ContainerEnvironment env;
    env.in_container = !markers.empty();
    env.markers = markers;
    return env;
}

ContainerEnvironment detect_container_environment() {
    SandboxDetectionInputs inputs;
    
#ifdef _WIN32
    // env extraction on windows
    char* env_strings = GetEnvironmentStringsA();
    if (env_strings) {
        for (char* p = env_strings; *p; p += std::strlen(p) + 1) {
            std::string env_var(p);
            std::size_t eq_pos = env_var.find('=');
            if (eq_pos != std::string::npos && eq_pos > 0) {
                inputs.env_pairs.emplace_back(env_var.substr(0, eq_pos), env_var.substr(eq_pos + 1));
            }
        }
        FreeEnvironmentStringsA(env_strings);
    }
#else
    extern char** environ;
    if (environ) {
        for (char** env = environ; *env; ++env) {
            std::string env_var(*env);
            std::size_t eq_pos = env_var.find('=');
            if (eq_pos != std::string::npos && eq_pos > 0) {
                inputs.env_pairs.emplace_back(env_var.substr(0, eq_pos), env_var.substr(eq_pos + 1));
            }
        }
    }
#endif
    
    inputs.dockerenv_exists = fs::exists("/.dockerenv");
    inputs.containerenv_exists = fs::exists("/run/.containerenv");
    
    try {
        if (fs::exists("/proc/1/cgroup")) {
            std::ifstream file("/proc/1/cgroup");
            if (file) {
                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                inputs.proc_1_cgroup = content;
            }
        }
    } catch (...) {}
    
    return detect_container_environment_from(inputs);
}

static std::vector<std::string> normalize_mounts(const std::vector<std::string>& mounts, const fs::path& cwd) {
    std::vector<std::string> normalized;
    normalized.reserve(mounts.size());
    for (const auto& mount : mounts) {
        fs::path p(mount);
        if (p.is_absolute()) {
            normalized.push_back(p.string());
        } else {
            normalized.push_back((cwd / p).string());
        }
    }
    return normalized;
}

SandboxStatus resolve_sandbox_status_for_request(const SandboxRequest& request, const fs::path& cwd) {
    auto container = detect_container_environment();
    
#ifdef _WIN32
    bool is_linux = false;
#else
    bool is_linux = true;
#endif

    bool namespace_supported = is_linux && unshare_user_namespace_works();
    bool network_supported = namespace_supported;
    bool filesystem_active = request.enabled && request.filesystem_mode != FilesystemIsolationMode::Off;
    
    std::vector<std::string> fallback_reasons;
    
    if (request.enabled && request.namespace_restrictions && !namespace_supported) {
        fallback_reasons.push_back("namespace isolation unavailable (requires Linux with `unshare`)");
    }
    if (request.enabled && request.network_isolation && !network_supported) {
        fallback_reasons.push_back("network isolation unavailable (requires Linux with `unshare`)");
    }
    if (request.enabled && request.filesystem_mode == FilesystemIsolationMode::AllowList && request.allowed_mounts.empty()) {
        fallback_reasons.push_back("filesystem allow-list requested without configured mounts");
    }
    
    bool active = request.enabled && 
                  (!request.namespace_restrictions || namespace_supported) && 
                  (!request.network_isolation || network_supported);
                  
    auto allowed_mounts = normalize_mounts(request.allowed_mounts, cwd);
    
    SandboxStatus status;
    status.enabled = request.enabled;
    status.requested = request;
    status.supported = namespace_supported;
    status.active = active;
    status.namespace_supported = namespace_supported;
    status.namespace_active = request.enabled && request.namespace_restrictions && namespace_supported;
    status.network_supported = network_supported;
    status.network_active = request.enabled && request.network_isolation && network_supported;
    status.filesystem_mode = request.filesystem_mode;
    status.filesystem_active = filesystem_active;
    status.allowed_mounts = allowed_mounts;
    status.in_container = container.in_container;
    status.container_markers = container.markers;
    
    if (!fallback_reasons.empty()) {
        std::string joined;
        for (size_t i = 0; i < fallback_reasons.size(); ++i) {
            if (i > 0) joined += "; ";
            joined += fallback_reasons[i];
        }
        status.fallback_reason = joined;
    }
    
    return status;
}

SandboxStatus resolve_sandbox_status(const SandboxConfig& config, const fs::path& cwd) {
    auto request = resolve_request(config);
    return resolve_sandbox_status_for_request(request, cwd);
}

std::optional<LinuxSandboxCommand> build_linux_sandbox_command(
    const std::string& command,
    const fs::path& cwd,
    const SandboxStatus& status) {
    
#ifndef _WIN32
    if (!status.enabled || (!status.namespace_active && !status.network_active)) {
        return std::nullopt;
    }
    
    LinuxSandboxCommand cmd;
    cmd.program = "unshare";
    cmd.args = { "--user", "--map-root-user", "--mount", "--ipc", "--pid", "--uts", "--fork" };
    if (status.network_active) {
        cmd.args.push_back("--net");
    }
    cmd.args.push_back("sh");
    cmd.args.push_back("-lc");
    cmd.args.push_back(command);
    
    fs::path sandbox_home = cwd / ".sandbox-home";
    fs::path sandbox_tmp = cwd / ".sandbox-tmp";
    
    cmd.env.push_back({"HOME", sandbox_home.string()});
    cmd.env.push_back({"TMPDIR", sandbox_tmp.string()});
    cmd.env.push_back({"CLAWD_SANDBOX_FILESYSTEM_MODE", as_str(status.filesystem_mode)});
    
    std::string mounts_str;
    for (size_t i = 0; i < status.allowed_mounts.size(); ++i) {
        if (i > 0) mounts_str += ":";
        mounts_str += status.allowed_mounts[i];
    }
    cmd.env.push_back({"CLAWD_SANDBOX_ALLOWED_MOUNTS", mounts_str});
    
    const char* path_env = std::getenv("PATH");
    if (path_env) {
        cmd.env.push_back({"PATH", path_env});
    }
    
    return cmd;
#else
    return std::nullopt;
#endif
}

// Compatibility stubs
SandboxStatus sandbox_status(const SandboxConfig& config) {
    return resolve_sandbox_status(config, fs::current_path());
}

std::vector<std::string> build_linux_sandbox_command(
    const SandboxConfig& config,
    const std::vector<std::string>& sandbox_dirs) {
    
    auto request = resolve_request(config);
    request.allowed_mounts = sandbox_dirs;
    
    auto status = resolve_sandbox_status_for_request(request, fs::current_path());
    auto cmd = build_linux_sandbox_command("sh", fs::current_path(), status);
    if (!cmd) return {};
    
    std::vector<std::string> prefix;
    prefix.push_back(cmd->program);
    for (auto it = cmd->args.begin(); it != cmd->args.end(); ++it) {
        if (*it == "sh") break; // Only the prefix before the shell command execution
        prefix.push_back(*it);
    }
    return prefix;
}

} // namespace claw::runtime
