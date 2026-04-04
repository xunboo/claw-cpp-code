#pragma once
// bash_validation.hpp
// C++20 port of rust/crates/runtime/src/bash_validation.rs
//
// Bash command validation pipeline:
//   - readOnlyValidation
//   - destructiveCommandWarning
//   - modeValidation
//   - sedValidation
//   - pathValidation
//   - commandSemantics

#include <filesystem>
#include <string>
#include <string_view>
#include <variant>

namespace claw::runtime {

// ---------------------------------------------------------------------------
// PermissionMode — mirrors Rust crate::permissions::PermissionMode
// ---------------------------------------------------------------------------

enum class PermissionMode {
    ReadOnly,          // No writes of any kind
    WorkspaceWrite,    // Writes limited to the workspace
    DangerFullAccess,  // Unrestricted
    Allow,             // Unrestricted (alias)
    Prompt,            // Ask before executing
};

// ---------------------------------------------------------------------------
// ValidationResult — mirrors Rust ValidationResult
// ---------------------------------------------------------------------------

struct ValidationAllow {};
struct ValidationBlock { std::string reason; };
struct ValidationWarn  { std::string message; };

/// Discriminated union: Allow | Block{reason} | Warn{message}
using ValidationResult = std::variant<ValidationAllow, ValidationBlock, ValidationWarn>;

// ---------------------------------------------------------------------------
// CommandIntent — mirrors Rust CommandIntent
// ---------------------------------------------------------------------------

enum class CommandIntent {
    ReadOnly,           // ls, cat, grep, find, …
    Write,              // cp, mv, mkdir, touch, tee, …
    Destructive,        // rm, shred, truncate, …
    Network,            // curl, wget, ssh, …
    ProcessManagement,  // kill, pkill, …
    PackageManagement,  // apt, brew, pip, npm, …
    SystemAdmin,        // sudo, chmod, chown, mount, …
    Unknown,
};

// ---------------------------------------------------------------------------
// Public API — one function per Rust pub fn
// ---------------------------------------------------------------------------

/// readOnlyValidation: block write-like commands when mode == ReadOnly.
[[nodiscard]] ValidationResult validate_read_only(std::string_view command,
                                                  PermissionMode mode);

/// destructiveCommandWarning: flag dangerous commands with a Warn result.
[[nodiscard]] ValidationResult check_destructive(std::string_view command);

/// modeValidation: enforce permission-mode constraints.
[[nodiscard]] ValidationResult validate_mode(std::string_view command,
                                             PermissionMode mode);

/// sedValidation: block sed -i in read-only mode.
[[nodiscard]] ValidationResult validate_sed(std::string_view command,
                                            PermissionMode mode);

/// pathValidation: detect suspicious traversal / home-dir patterns.
[[nodiscard]] ValidationResult validate_paths(std::string_view command,
                                              const std::filesystem::path& workspace);

/// commandSemantics: classify command intent.
[[nodiscard]] CommandIntent classify_command(std::string_view command);

/// Full pipeline: runs all checks in order, returns first non-Allow result.
[[nodiscard]] ValidationResult validate_command(std::string_view command,
                                                PermissionMode mode,
                                                const std::filesystem::path& workspace);

} // namespace claw::runtime
