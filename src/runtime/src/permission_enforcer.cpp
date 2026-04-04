// Permission enforcement layer that gates tool execution based on the
// active PermissionPolicy (rule set).
//
// Translated from Rust: crates/runtime/src/permission_enforcer.rs

#include "permission_enforcer.hpp"
#include <algorithm>
#include <string>
#include <string_view>

namespace claw::runtime {

// ---------------------------------------------------------------------------
// Read-only bash command heuristic
// ---------------------------------------------------------------------------

static constexpr std::string_view READ_ONLY_BASENAMES[] = {
    "cat", "head", "tail", "less", "more", "wc",
    "ls", "find", "grep", "rg",
    "awk", "sed", "echo", "printf",
    "which", "where", "whoami", "pwd",
    "env", "printenv",
    "date", "cal", "df", "du", "free", "uptime", "uname",
    "file", "stat", "diff", "sort", "uniq",
    "tr", "cut", "paste", "tee", "xargs",
    "test", "true", "false", "type",
    "readlink", "realpath", "basename", "dirname",
    "sha256sum", "md5sum", "b3sum",
    "xxd", "hexdump", "od", "strings",
    "tree", "jq", "yq",
    "python3", "python", "node", "ruby",
    "cargo", "rustc", "git", "gh",
};

static std::string_view extract_basename(std::string_view first_token) noexcept {
    auto pos = first_token.rfind('/');
    if (pos == std::string_view::npos) {
        pos = first_token.rfind('\\');
    }
    if (pos == std::string_view::npos) return first_token;
    return first_token.substr(pos + 1);
}

bool is_read_only_bash_command(std::string_view command) {
    while (!command.empty() && (command.front() == ' ' || command.front() == '\t')) {
        command.remove_prefix(1);
    }
    if (command.empty()) return false;

    if (command.find("-i ") != std::string_view::npos) return false;
    if (command.find("--in-place") != std::string_view::npos) return false;
    if (command.find(" > ") != std::string_view::npos) return false;
    if (command.find(" >> ") != std::string_view::npos) return false;

    auto space = command.find_first_of(" \t");
    std::string_view first_token = (space == std::string_view::npos)
        ? command
        : command.substr(0, space);

    std::string_view base = extract_basename(first_token);

    for (auto ro : READ_ONLY_BASENAMES) {
        if (base == ro) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// PermissionEnforcer
// ---------------------------------------------------------------------------

EnforcementResult PermissionEnforcer::check(std::string_view tool_name,
                                             std::string_view input) const {
    auto outcome = policy_.authorize(tool_name, input);
    if (outcome.is_allow()) {
        return EnforcementAllow{};
    }
    return EnforcementDeny{
        std::string(tool_name),
        std::string(permission_mode_as_str(policy_.active_mode())),
        std::string(permission_mode_as_str(policy_.required_mode_for(tool_name))),
        std::move(outcome.deny_reason)
    };
}

bool PermissionEnforcer::is_allowed(std::string_view tool_name,
                                     std::string_view input) const {
    return std::holds_alternative<EnforcementAllow>(check(tool_name, input));
}

PermissionMode PermissionEnforcer::active_mode() const noexcept {
    return policy_.active_mode();
}

EnforcementResult PermissionEnforcer::check_file_write(
    std::string_view path,
    std::string_view workspace_root) const
{
    // Build an input string that includes the path for rule matching
    std::string input = std::string(path);
    return check("write_file", input);
}

EnforcementResult PermissionEnforcer::check_bash(std::string_view command) const {
    if (is_read_only_bash_command(command)) {
        return EnforcementAllow{};
    }
    return check("bash", command);
}

} // namespace claw::runtime
