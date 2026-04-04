#include "sandbox.hpp"
#include <filesystem>
#include <fstream>
#include <mutex>

#ifdef _WIN32
// Windows: sandboxing via Linux namespaces is not available.

namespace claw::runtime {

bool detect_container_environment() {
    // Containers are a Linux concept; always false on Windows
    return false;
}

bool unshare_user_namespace_works() {
    // Linux user-namespace unshare not available on Windows
    return false;
}

std::vector<std::string> build_linux_sandbox_command(const SandboxConfig& config,
                                                      const std::vector<std::string>& /*sandbox_dirs*/) {
    (void)config;
    return {};
}

SandboxStatus sandbox_status(const SandboxConfig& config) {
    if (config.isolation_mode == FilesystemIsolationMode::None) return SandboxStatus::Inactive;
    return SandboxStatus::Unsupported;
}

} // namespace claw::runtime

#else
// POSIX
#include <unistd.h>
#include <sys/wait.h>
#include <sched.h>

namespace claw::runtime {

bool detect_container_environment() {
    // Check /.dockerenv
    if (std::filesystem::exists("/.dockerenv")) return true;
    // Check /proc/1/cgroup for docker/k8s
    std::ifstream cgroup("/proc/1/cgroup");
    if (cgroup) {
        std::string line;
        while (std::getline(cgroup, line)) {
            if (line.find("docker") != std::string::npos ||
                line.find("kubepods") != std::string::npos ||
                line.find("containerd") != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool unshare_user_namespace_works() {
    static bool result = false;
    static std::once_flag flag;
    std::call_once(flag, []() {
        pid_t pid = ::fork();
        if (pid == 0) {
#ifdef CLONE_NEWUSER
            if (::unshare(CLONE_NEWUSER) == 0) ::_exit(0);
#endif
            ::_exit(1);
        }
        if (pid > 0) {
            int status;
            ::waitpid(pid, &status, 0);
            result = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    });
    return result;
}

std::vector<std::string> build_linux_sandbox_command(const SandboxConfig& config,
                                                      const std::vector<std::string>& sandbox_dirs) {
    if (config.isolation_mode == FilesystemIsolationMode::None) return {};
    if (!unshare_user_namespace_works()) return {};

    std::vector<std::string> prefix;
    prefix.push_back("unshare");
    prefix.push_back("--user");
    prefix.push_back("--map-root-user");

    if (config.isolation_mode == FilesystemIsolationMode::ReadOnlyRoot) {
        prefix.push_back("--mount");
        prefix.push_back("--");
    }

    return prefix;
}

SandboxStatus sandbox_status(const SandboxConfig& config) {
    if (config.isolation_mode == FilesystemIsolationMode::None) return SandboxStatus::Inactive;
    if (!unshare_user_namespace_works()) return SandboxStatus::Unsupported;
    return SandboxStatus::Active;
}

} // namespace claw::runtime
#endif
