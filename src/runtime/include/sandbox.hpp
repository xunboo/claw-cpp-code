// sandbox.hpp - C++20 faithful conversion of sandbox.rs
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace claw::runtime {

enum class FilesystemIsolationMode {
  Off,
  WorkspaceOnly,
  AllowList,
  // Legacy alias: None maps to the same value as Off for backwards
  // compatibility
  None = Off,
};

inline std::string as_str(FilesystemIsolationMode mode) {
  switch (mode) {
  case FilesystemIsolationMode::Off:
    return "off";
  case FilesystemIsolationMode::WorkspaceOnly:
    return "workspace-only";
  case FilesystemIsolationMode::AllowList:
    return "allow-list";
  default:
    return "off";   
  }
}

// Ensure the default translates properly
inline FilesystemIsolationMode default_isolation_mode() {
  return FilesystemIsolationMode::WorkspaceOnly;
}

struct SandboxConfig {
  std::optional<bool> enabled;
  std::optional<bool> namespace_restrictions;
  std::optional<bool> network_isolation;
  std::optional<FilesystemIsolationMode> filesystem_mode;
  std::vector<std::string> allowed_mounts;

  // Legacy fields for backwards compatibility with craw-cpp-code's config.cpp
  FilesystemIsolationMode isolation_mode{FilesystemIsolationMode::None};
  std::vector<std::string> allowed_write_dirs;
  std::vector<std::string> allowed_network_hosts;
  bool allow_network{true};
};

struct SandboxRequest {
  bool enabled{false};
  bool namespace_restrictions{false};
  bool network_isolation{false};
  FilesystemIsolationMode filesystem_mode{FilesystemIsolationMode::Off};
  std::vector<std::string> allowed_mounts;
};

struct ContainerEnvironment {
  bool in_container{false};
  std::vector<std::string> markers;
};

struct SandboxStatus {
  bool enabled{false};
  SandboxRequest requested;
  bool supported{false};
  bool active{false};
  bool namespace_supported{false};
  bool namespace_active{false};
  bool network_supported{false};
  bool network_active{false};
  FilesystemIsolationMode filesystem_mode{FilesystemIsolationMode::Off};
  bool filesystem_active{false};
  std::vector<std::string> allowed_mounts;
  bool in_container{false};
  std::vector<std::string> container_markers;
  std::optional<std::string> fallback_reason;
};

struct SandboxDetectionInputs {
  std::vector<std::pair<std::string, std::string>> env_pairs;
  bool dockerenv_exists{false};
  bool containerenv_exists{false};
  std::optional<std::string> proc_1_cgroup;
};

struct LinuxSandboxCommand {
  std::string program;
  std::vector<std::string> args;
  std::vector<std::pair<std::string, std::string>> env;
};

[[nodiscard]] SandboxRequest resolve_request(
    const SandboxConfig &config,
    std::optional<bool> enabled_override = std::nullopt,
    std::optional<bool> namespace_override = std::nullopt,
    std::optional<bool> network_override = std::nullopt,
    std::optional<FilesystemIsolationMode> filesystem_mode_override =
        std::nullopt,
    std::optional<std::vector<std::string>> allowed_mounts_override =
        std::nullopt);

[[nodiscard]] ContainerEnvironment detect_container_environment();
[[nodiscard]] ContainerEnvironment
detect_container_environment_from(const SandboxDetectionInputs &inputs);

[[nodiscard]] SandboxStatus
resolve_sandbox_status(const SandboxConfig &config,
                       const std::filesystem::path &cwd);
[[nodiscard]] SandboxStatus
resolve_sandbox_status_for_request(const SandboxRequest &request,
                                   const std::filesystem::path &cwd);

[[nodiscard]] std::optional<LinuxSandboxCommand>
build_linux_sandbox_command(const std::string &command,
                            const std::filesystem::path &cwd,
                            const SandboxStatus &status);

// Compatibility stubs
[[nodiscard]] SandboxStatus sandbox_status(const SandboxConfig &config);
[[nodiscard]] std::vector<std::string>
build_linux_sandbox_command(const SandboxConfig &config,
                            const std::vector<std::string> &sandbox_dirs);
[[nodiscard]] bool unshare_user_namespace_works();

} // namespace claw::runtime
