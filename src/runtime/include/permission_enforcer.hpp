#pragma once
#include "permissions.hpp"
#include <string>
#include <string_view>
#include <variant>

// ---------------------------------------------------------------------------
// Permission enforcement layer — C++20 translation of
// crates/runtime/src/permission_enforcer.rs
//
// Wraps a PermissionPolicy and provides convenience methods for checking
// tool permissions (file writes, bash commands, generic tools).
// ---------------------------------------------------------------------------

namespace claw::runtime {

// ---------------------------------------------------------------------------
// EnforcementResult — matches Rust enum EnforcementResult
// ---------------------------------------------------------------------------
struct EnforcementAllow {};

struct EnforcementDeny {
    std::string tool;
    std::string active_mode;
    std::string required_mode;
    std::string reason;
};

using EnforcementResult = std::variant<EnforcementAllow, EnforcementDeny>;

// Read-only bash command heuristics (commands that don't modify state)
[[nodiscard]] bool is_read_only_bash_command(std::string_view command);

// ---------------------------------------------------------------------------
// PermissionEnforcer — wraps PermissionPolicy
// ---------------------------------------------------------------------------
class PermissionEnforcer {
public:
    explicit PermissionEnforcer(PermissionPolicy policy)
        : policy_(std::move(policy)) {}

    // Check whether a tool can be executed under the current permission policy.
    [[nodiscard]] EnforcementResult check(std::string_view tool_name,
                                          std::string_view input) const;

    [[nodiscard]] bool is_allowed(std::string_view tool_name,
                                  std::string_view input) const;

    [[nodiscard]] PermissionMode active_mode() const noexcept;

    [[nodiscard]] EnforcementResult check_file_write(
        std::string_view path,
        std::string_view workspace_root) const;

    [[nodiscard]] EnforcementResult check_bash(std::string_view command) const;

private:
    PermissionPolicy policy_;
};

} // namespace claw::runtime
