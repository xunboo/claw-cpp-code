#pragma once
#include <string>
#include <vector>
#include <optional>
#include <tl/expected.hpp>

namespace claw::runtime {

enum class FilesystemIsolationMode {
    None,
    ReadOnlyRoot,
    Restricted,
};

struct SandboxConfig {
    FilesystemIsolationMode isolation_mode{FilesystemIsolationMode::None};
    std::vector<std::string> allowed_write_dirs;
    std::vector<std::string> allowed_network_hosts;
    bool allow_network{true};
};

enum class SandboxStatus {
    Active,
    Inactive,
    Unsupported,
};

// Detect if running inside a container (Docker/Kubernetes)
[[nodiscard]] bool detect_container_environment();

// Check if unshare user namespace works on this system (result cached via call_once)
[[nodiscard]] bool unshare_user_namespace_works();

// Build the command prefix to invoke a command inside a Linux user namespace sandbox
// Returns empty vector if sandboxing is not supported/configured
[[nodiscard]] std::vector<std::string> build_linux_sandbox_command(
    const SandboxConfig& config,
    const std::vector<std::string>& sandbox_dirs);

// Query current sandbox status
[[nodiscard]] SandboxStatus sandbox_status(const SandboxConfig& config);

} // namespace claw::runtime
