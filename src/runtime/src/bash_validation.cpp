// bash_validation.cpp
// Full, faithful C++20 port of rust/crates/runtime/src/bash_validation.rs
//
// Corresponds to upstream BashTool validation pipeline:
//   - readOnlyValidation
//   - destructiveCommandWarning
//   - modeValidation
//   - sedValidation
//   - pathValidation
//   - commandSemantics

#include "bash_validation.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace claw::runtime {

// ---------------------------------------------------------------------------
// Internal helpers — forward declarations
// ---------------------------------------------------------------------------

static std::string extract_first_command(std::string_view command);
static std::string_view extract_sudo_inner(std::string_view command);
static std::optional<std::size_t> find_end_of_value(std::string_view s);
static ValidationResult validate_git_read_only(std::string_view command);
static CommandIntent classify_by_first_command(std::string_view first,
                                               std::string_view command);
static CommandIntent classify_git_command(std::string_view command);
static bool command_targets_outside_workspace(std::string_view command);

// ---------------------------------------------------------------------------
// readOnlyValidation — constant tables
// ---------------------------------------------------------------------------

/// Commands that perform write operations (blocked in read-only mode).
static constexpr std::array WRITE_COMMANDS = {
    std::string_view{"cp"},    std::string_view{"mv"},
    std::string_view{"rm"},    std::string_view{"mkdir"},
    std::string_view{"rmdir"}, std::string_view{"touch"},
    std::string_view{"chmod"}, std::string_view{"chown"},
    std::string_view{"chgrp"}, std::string_view{"ln"},
    std::string_view{"install"},std::string_view{"tee"},
    std::string_view{"truncate"},std::string_view{"shred"},
    std::string_view{"mkfifo"},std::string_view{"mknod"},
    std::string_view{"dd"},
};

/// Commands that modify system state (blocked in read-only mode).
static constexpr std::array STATE_MODIFYING_COMMANDS = {
    std::string_view{"apt"},      std::string_view{"apt-get"},
    std::string_view{"yum"},      std::string_view{"dnf"},
    std::string_view{"pacman"},   std::string_view{"brew"},
    std::string_view{"pip"},      std::string_view{"pip3"},
    std::string_view{"npm"},      std::string_view{"yarn"},
    std::string_view{"pnpm"},     std::string_view{"bun"},
    std::string_view{"cargo"},    std::string_view{"gem"},
    std::string_view{"go"},       std::string_view{"rustup"},
    std::string_view{"docker"},   std::string_view{"systemctl"},
    std::string_view{"service"},  std::string_view{"mount"},
    std::string_view{"umount"},   std::string_view{"kill"},
    std::string_view{"pkill"},    std::string_view{"killall"},
    std::string_view{"reboot"},   std::string_view{"shutdown"},
    std::string_view{"halt"},     std::string_view{"poweroff"},
    std::string_view{"useradd"},  std::string_view{"userdel"},
    std::string_view{"usermod"},  std::string_view{"groupadd"},
    std::string_view{"groupdel"}, std::string_view{"crontab"},
    std::string_view{"at"},
};

/// Shell redirection operators that indicate writes.
static constexpr std::array WRITE_REDIRECTIONS = {
    std::string_view{">"}, std::string_view{">>"}, std::string_view{">&"},
};

/// Git subcommands that are read-only safe.
static constexpr std::array GIT_READ_ONLY_SUBCOMMANDS = {
    std::string_view{"status"},   std::string_view{"log"},
    std::string_view{"diff"},     std::string_view{"show"},
    std::string_view{"branch"},   std::string_view{"tag"},
    std::string_view{"stash"},    std::string_view{"remote"},
    std::string_view{"fetch"},    std::string_view{"ls-files"},
    std::string_view{"ls-tree"},  std::string_view{"cat-file"},
    std::string_view{"rev-parse"},std::string_view{"describe"},
    std::string_view{"shortlog"}, std::string_view{"blame"},
    std::string_view{"bisect"},   std::string_view{"reflog"},
    std::string_view{"config"},
};

// ---------------------------------------------------------------------------
// destructiveCommandWarning — constant tables
// ---------------------------------------------------------------------------

/// (substring_pattern, human_readable_warning) pairs.
static constexpr std::array<std::pair<std::string_view, std::string_view>, 10>
    DESTRUCTIVE_PATTERNS{{
        {"rm -rf /",
         "Recursive forced deletion at root \xe2\x80\x94 this will destroy the system"},
        {"rm -rf ~", "Recursive forced deletion of home directory"},
        {"rm -rf *",
         "Recursive forced deletion of all files in current directory"},
        {"rm -rf .", "Recursive forced deletion of current directory"},
        {"mkfs", "Filesystem creation will destroy existing data on the device"},
        {"dd if=", "Direct disk write \xe2\x80\x94 can overwrite partitions or devices"},
        {"> /dev/sd", "Writing to raw disk device"},
        {"chmod -R 777", "Recursively setting world-writable permissions"},
        {"chmod -R 000", "Recursively removing all permissions"},
        {":(){ :|:& };:", "Fork bomb \xe2\x80\x94 will crash the system"},
    }};

/// Commands that are always destructive regardless of arguments.
static constexpr std::array ALWAYS_DESTRUCTIVE_COMMANDS = {
    std::string_view{"shred"}, std::string_view{"wipefs"},
};

// ---------------------------------------------------------------------------
// commandSemantics — constant tables
// ---------------------------------------------------------------------------

static constexpr std::array SEMANTIC_READ_ONLY_COMMANDS = {
    std::string_view{"ls"},       std::string_view{"cat"},
    std::string_view{"head"},     std::string_view{"tail"},
    std::string_view{"less"},     std::string_view{"more"},
    std::string_view{"wc"},       std::string_view{"sort"},
    std::string_view{"uniq"},     std::string_view{"grep"},
    std::string_view{"egrep"},    std::string_view{"fgrep"},
    std::string_view{"find"},     std::string_view{"which"},
    std::string_view{"whereis"},  std::string_view{"whatis"},
    std::string_view{"man"},      std::string_view{"info"},
    std::string_view{"file"},     std::string_view{"stat"},
    std::string_view{"du"},       std::string_view{"df"},
    std::string_view{"free"},     std::string_view{"uptime"},
    std::string_view{"uname"},    std::string_view{"hostname"},
    std::string_view{"whoami"},   std::string_view{"id"},
    std::string_view{"groups"},   std::string_view{"env"},
    std::string_view{"printenv"}, std::string_view{"echo"},
    std::string_view{"printf"},   std::string_view{"date"},
    std::string_view{"cal"},      std::string_view{"bc"},
    std::string_view{"expr"},     std::string_view{"test"},
    std::string_view{"true"},     std::string_view{"false"},
    std::string_view{"pwd"},      std::string_view{"tree"},
    std::string_view{"diff"},     std::string_view{"cmp"},
    std::string_view{"md5sum"},   std::string_view{"sha256sum"},
    std::string_view{"sha1sum"},  std::string_view{"xxd"},
    std::string_view{"od"},       std::string_view{"hexdump"},
    std::string_view{"strings"},  std::string_view{"readlink"},
    std::string_view{"realpath"}, std::string_view{"basename"},
    std::string_view{"dirname"},  std::string_view{"seq"},
    std::string_view{"yes"},      std::string_view{"tput"},
    std::string_view{"column"},   std::string_view{"jq"},
    std::string_view{"yq"},       std::string_view{"xargs"},
    std::string_view{"tr"},       std::string_view{"cut"},
    std::string_view{"paste"},    std::string_view{"awk"},
    std::string_view{"sed"},
};

static constexpr std::array NETWORK_COMMANDS = {
    std::string_view{"curl"},      std::string_view{"wget"},
    std::string_view{"ssh"},       std::string_view{"scp"},
    std::string_view{"rsync"},     std::string_view{"ftp"},
    std::string_view{"sftp"},      std::string_view{"nc"},
    std::string_view{"ncat"},      std::string_view{"telnet"},
    std::string_view{"ping"},      std::string_view{"traceroute"},
    std::string_view{"dig"},       std::string_view{"nslookup"},
    std::string_view{"host"},      std::string_view{"whois"},
    std::string_view{"ifconfig"},  std::string_view{"ip"},
    std::string_view{"netstat"},   std::string_view{"ss"},
    std::string_view{"nmap"},
};

static constexpr std::array PROCESS_COMMANDS = {
    std::string_view{"kill"},   std::string_view{"pkill"},
    std::string_view{"killall"},std::string_view{"ps"},
    std::string_view{"top"},    std::string_view{"htop"},
    std::string_view{"bg"},     std::string_view{"fg"},
    std::string_view{"jobs"},   std::string_view{"nohup"},
    std::string_view{"disown"}, std::string_view{"wait"},
    std::string_view{"nice"},   std::string_view{"renice"},
};

static constexpr std::array PACKAGE_COMMANDS = {
    std::string_view{"apt"},     std::string_view{"apt-get"},
    std::string_view{"yum"},     std::string_view{"dnf"},
    std::string_view{"pacman"},  std::string_view{"brew"},
    std::string_view{"pip"},     std::string_view{"pip3"},
    std::string_view{"npm"},     std::string_view{"yarn"},
    std::string_view{"pnpm"},    std::string_view{"bun"},
    std::string_view{"cargo"},   std::string_view{"gem"},
    std::string_view{"go"},      std::string_view{"rustup"},
    std::string_view{"snap"},    std::string_view{"flatpak"},
};

static constexpr std::array SYSTEM_ADMIN_COMMANDS = {
    std::string_view{"sudo"},        std::string_view{"su"},
    std::string_view{"chroot"},      std::string_view{"mount"},
    std::string_view{"umount"},      std::string_view{"fdisk"},
    std::string_view{"parted"},      std::string_view{"lsblk"},
    std::string_view{"blkid"},       std::string_view{"systemctl"},
    std::string_view{"service"},     std::string_view{"journalctl"},
    std::string_view{"dmesg"},       std::string_view{"modprobe"},
    std::string_view{"insmod"},      std::string_view{"rmmod"},
    std::string_view{"iptables"},    std::string_view{"ufw"},
    std::string_view{"firewall-cmd"},std::string_view{"sysctl"},
    std::string_view{"crontab"},     std::string_view{"at"},
    std::string_view{"useradd"},     std::string_view{"userdel"},
    std::string_view{"usermod"},     std::string_view{"groupadd"},
    std::string_view{"groupdel"},    std::string_view{"passwd"},
    std::string_view{"visudo"},
};

// ---------------------------------------------------------------------------
// Helper: array-contains (avoids runtime std::find overhead for constexpr arrays)
// ---------------------------------------------------------------------------

template <typename Array>
[[nodiscard]] static bool array_contains(const Array& arr, std::string_view value) {
    return std::ranges::any_of(arr, [value](std::string_view sv) { return sv == value; });
}

// ---------------------------------------------------------------------------
// Internal helper implementations
// ---------------------------------------------------------------------------

/// Find the end of a KEY=value token (handles basic quoting).
/// Returns the index past the value's last character, or nullopt if the value
/// extends to the end of the string (i.e. no trailing space).
static std::optional<std::size_t> find_end_of_value(std::string_view s) {
    // Trim leading whitespace (mirrors Rust trim_start).
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    s = s.substr(start);

    if (s.empty())
        return std::nullopt;

    const unsigned char first = static_cast<unsigned char>(s[0]);
    if (first == '"' || first == '\'') {
        const unsigned char quote = first;
        std::size_t i = 1;
        while (i < s.size()) {
            if (static_cast<unsigned char>(s[i]) == quote &&
                (i == 0 || static_cast<unsigned char>(s[i - 1]) != '\\')) {
                ++i; // skip past closing quote
                while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
                    ++i;
                return (i < s.size()) ? std::optional<std::size_t>{start + i} : std::nullopt;
            }
            ++i;
        }
        return std::nullopt;
    } else {
        // Unquoted value: find next whitespace character.
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (std::isspace(static_cast<unsigned char>(s[i])))
                return start + i;
        }
        return std::nullopt;
    }
}

/// Extract the first bare command token, stripping leading KEY=value env pairs.
/// Mirrors Rust extract_first_command().
static std::string extract_first_command(std::string_view command) {
    // Trim leading whitespace.
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.front())))
        command.remove_prefix(1);

    std::string_view remaining = command;

    // Skip over leading KEY=value assignments.
    while (!remaining.empty()) {
        // Find '=' in the next token.
        auto eq_pos = remaining.find('=');
        if (eq_pos == std::string_view::npos)
            break;

        std::string_view before_eq = remaining.substr(0, eq_pos);

        // Valid env-var name: non-empty, all alphanumeric/underscore.
        bool valid_name = !before_eq.empty() &&
            std::ranges::all_of(before_eq, [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            });

        if (!valid_name)
            break;

        std::string_view after_eq = remaining.substr(eq_pos + 1);
        auto end_opt = find_end_of_value(after_eq);
        if (!end_opt.has_value()) {
            // Value reaches end of string — no actual command follows.
            return {};
        }
        remaining = after_eq.substr(*end_opt);

        // Trim whitespace after value.
        while (!remaining.empty() && std::isspace(static_cast<unsigned char>(remaining.front())))
            remaining.remove_prefix(1);
    }

    // The first whitespace-separated token is the command.
    std::size_t end = 0;
    while (end < remaining.size() && !std::isspace(static_cast<unsigned char>(remaining[end])))
        ++end;

    return std::string(remaining.substr(0, end));
}

/// Return the portion of `command` starting at the inner command that follows
/// "sudo" (skipping sudo flags).  Returns empty string_view if not found.
static std::string_view extract_sudo_inner(std::string_view command) {
    // Tokenise.
    std::vector<std::string_view> parts;
    {
        std::size_t i = 0;
        while (i < command.size()) {
            while (i < command.size() && std::isspace(static_cast<unsigned char>(command[i])))
                ++i;
            if (i >= command.size())
                break;
            std::size_t j = i;
            while (j < command.size() && !std::isspace(static_cast<unsigned char>(command[j])))
                ++j;
            parts.push_back(command.substr(i, j - i));
            i = j;
        }
    }

    // Find "sudo".
    auto sudo_it = std::ranges::find(parts, std::string_view{"sudo"});
    if (sudo_it == parts.end())
        return {};

    // Skip flags after sudo to find the inner command token.
    auto rest_begin = std::next(sudo_it);
    for (auto it = rest_begin; it != parts.end(); ++it) {
        if (!it->starts_with('-')) {
            // Found the inner command — return from its position to the end.
            // Locate the token in the original string.
            auto offset = command.find(*it);
            if (offset == std::string_view::npos)
                return {};
            return command.substr(offset);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// readOnlyValidation
// ---------------------------------------------------------------------------

static ValidationResult validate_git_read_only(std::string_view command) {
    // Tokenise.
    std::vector<std::string_view> parts;
    {
        std::size_t i = 0;
        while (i < command.size()) {
            while (i < command.size() && std::isspace(static_cast<unsigned char>(command[i])))
                ++i;
            if (i >= command.size())
                break;
            std::size_t j = i;
            while (j < command.size() && !std::isspace(static_cast<unsigned char>(command[j])))
                ++j;
            parts.push_back(command.substr(i, j - i));
            i = j;
        }
    }

    // Skip past "git" and any flags (e.g., "git -C /path").
    std::optional<std::string_view> subcommand;
    for (auto it = parts.begin() + 1; it != parts.end(); ++it) {
        if (!it->starts_with('-')) {
            subcommand = *it;
            break;
        }
    }

    if (!subcommand.has_value()) {
        // Bare "git" — allow.
        return ValidationResult{std::in_place_type<ValidationAllow>};
    }

    if (array_contains(GIT_READ_ONLY_SUBCOMMANDS, *subcommand)) {
        return ValidationResult{std::in_place_type<ValidationAllow>};
    }

    return ValidationResult{std::in_place_type<ValidationBlock>,
        std::format(
            "Git subcommand '{}' modifies repository state and is not allowed in read-only mode",
            *subcommand)};
}

ValidationResult validate_read_only(std::string_view command, PermissionMode mode) {
    if (mode != PermissionMode::ReadOnly) {
        return ValidationResult{std::in_place_type<ValidationAllow>};
    }

    const std::string first = extract_first_command(command);

    // Check for write commands.
    for (std::string_view write_cmd : WRITE_COMMANDS) {
        if (first == write_cmd) {
            return ValidationResult{std::in_place_type<ValidationBlock>,
                std::format(
                    "Command '{}' modifies the filesystem and is not allowed in read-only mode",
                    write_cmd)};
        }
    }

    // Check for state-modifying commands.
    for (std::string_view state_cmd : STATE_MODIFYING_COMMANDS) {
        if (first == state_cmd) {
            return ValidationResult{std::in_place_type<ValidationBlock>,
                std::format(
                    "Command '{}' modifies system state and is not allowed in read-only mode",
                    state_cmd)};
        }
    }

    // Check for sudo wrapping write/state commands.
    if (first == "sudo") {
        std::string_view inner = extract_sudo_inner(command);
        if (!inner.empty()) {
            auto inner_result = validate_read_only(inner, mode);
            if (!std::holds_alternative<ValidationAllow>(inner_result)) {
                return inner_result;
            }
        }
    }

    // Check for write redirections.
    for (std::string_view redir : WRITE_REDIRECTIONS) {
        if (command.find(redir) != std::string_view::npos) {
            return ValidationResult{std::in_place_type<ValidationBlock>,
                std::format(
                    "Command contains write redirection '{}' which is not allowed in read-only mode",
                    redir)};
        }
    }

    // Check for git commands that modify state.
    if (first == "git") {
        return validate_git_read_only(command);
    }

    return ValidationResult{std::in_place_type<ValidationAllow>};
}

// ---------------------------------------------------------------------------
// destructiveCommandWarning
// ---------------------------------------------------------------------------

ValidationResult check_destructive(std::string_view command) {
    // Check known destructive substring patterns.
    for (const auto& [pattern, warning] : DESTRUCTIVE_PATTERNS) {
        if (command.find(pattern) != std::string_view::npos) {
            return ValidationResult{std::in_place_type<ValidationWarn>,
                std::format("Destructive command detected: {}", warning)};
        }
    }

    // Check always-destructive commands.
    const std::string first = extract_first_command(command);
    for (std::string_view cmd : ALWAYS_DESTRUCTIVE_COMMANDS) {
        if (first == cmd) {
            return ValidationResult{std::in_place_type<ValidationWarn>,
                std::format(
                    "Command '{}' is inherently destructive and may cause data loss",
                    cmd)};
        }
    }

    // Check for "rm -rf" with broad targets (the most specific patterns were
    // already caught above; flag any remaining rm -rf).
    if (command.find("rm ") != std::string_view::npos &&
        command.find("-r") != std::string_view::npos &&
        command.find("-f") != std::string_view::npos) {
        return ValidationResult{std::in_place_type<ValidationWarn>,
            std::string{
                "Recursive forced deletion detected \xe2\x80\x94 verify the target path is correct"}};
    }

    return ValidationResult{std::in_place_type<ValidationAllow>};
}

// ---------------------------------------------------------------------------
// modeValidation
// ---------------------------------------------------------------------------

/// Heuristic: does the command reference absolute paths outside typical workspace dirs?
static bool command_targets_outside_workspace(std::string_view command) {
    static constexpr std::array SYSTEM_PATHS = {
        std::string_view{"/etc/"},  std::string_view{"/usr/"},
        std::string_view{"/var/"},  std::string_view{"/boot/"},
        std::string_view{"/sys/"},  std::string_view{"/proc/"},
        std::string_view{"/dev/"},  std::string_view{"/sbin/"},
        std::string_view{"/lib/"},  std::string_view{"/opt/"},
    };

    const std::string first = extract_first_command(command);
    const bool is_write_cmd =
        array_contains(WRITE_COMMANDS, std::string_view{first}) ||
        array_contains(STATE_MODIFYING_COMMANDS, std::string_view{first});

    if (!is_write_cmd)
        return false;

    for (std::string_view sys_path : SYSTEM_PATHS) {
        if (command.find(sys_path) != std::string_view::npos)
            return true;
    }
    return false;
}

ValidationResult validate_mode(std::string_view command, PermissionMode mode) {
    switch (mode) {
        case PermissionMode::ReadOnly:
            return validate_read_only(command, mode);

        case PermissionMode::WorkspaceWrite:
            if (command_targets_outside_workspace(command)) {
                return ValidationResult{std::in_place_type<ValidationWarn>,
                    std::string{
                        "Command appears to target files outside the workspace"
                        " \xe2\x80\x94 requires elevated permission"}};
            }
            return ValidationResult{std::in_place_type<ValidationAllow>};

        case PermissionMode::DangerFullAccess:
        case PermissionMode::Allow:
        case PermissionMode::Prompt:
            return ValidationResult{std::in_place_type<ValidationAllow>};
    }
    // Unreachable, but keeps the compiler happy.
    return ValidationResult{std::in_place_type<ValidationAllow>};
}

// ---------------------------------------------------------------------------
// sedValidation
// ---------------------------------------------------------------------------

ValidationResult validate_sed(std::string_view command, PermissionMode mode) {
    const std::string first = extract_first_command(command);
    if (first != "sed") {
        return ValidationResult{std::in_place_type<ValidationAllow>};
    }

    // In read-only mode, block sed -i (in-place editing).
    if (mode == PermissionMode::ReadOnly && command.find(" -i") != std::string_view::npos) {
        return ValidationResult{std::in_place_type<ValidationBlock>,
            std::string{"sed -i (in-place editing) is not allowed in read-only mode"}};
    }

    return ValidationResult{std::in_place_type<ValidationAllow>};
}

// ---------------------------------------------------------------------------
// pathValidation
// ---------------------------------------------------------------------------

ValidationResult validate_paths(std::string_view command,
                                const std::filesystem::path& workspace) {
    if (command.find("../") != std::string_view::npos) {
        const std::string workspace_str = workspace.string();
        if (command.find(workspace_str) == std::string_view::npos) {
            return ValidationResult{std::in_place_type<ValidationWarn>,
                std::string{
                    "Command contains directory traversal pattern '../'"
                    " \xe2\x80\x94 verify the target path resolves within the workspace"}};
        }
    }

    if (command.find("~/") != std::string_view::npos ||
        command.find("$HOME") != std::string_view::npos) {
        return ValidationResult{std::in_place_type<ValidationWarn>,
            std::string{
                "Command references home directory"
                " \xe2\x80\x94 verify it stays within the workspace scope"}};
    }

    return ValidationResult{std::in_place_type<ValidationAllow>};
}

// ---------------------------------------------------------------------------
// commandSemantics
// ---------------------------------------------------------------------------

static CommandIntent classify_git_command(std::string_view command) {
    // Tokenise.
    std::vector<std::string_view> parts;
    {
        std::size_t i = 0;
        while (i < command.size()) {
            while (i < command.size() && std::isspace(static_cast<unsigned char>(command[i])))
                ++i;
            if (i >= command.size())
                break;
            std::size_t j = i;
            while (j < command.size() && !std::isspace(static_cast<unsigned char>(command[j])))
                ++j;
            parts.push_back(command.substr(i, j - i));
            i = j;
        }
    }

    // Skip past "git" and any flags.
    std::optional<std::string_view> subcommand;
    for (auto it = parts.begin() + 1; it != parts.end(); ++it) {
        if (!it->starts_with('-')) {
            subcommand = *it;
            break;
        }
    }

    if (!subcommand.has_value())
        return CommandIntent::Write; // bare git falls to Write as per Rust

    return array_contains(GIT_READ_ONLY_SUBCOMMANDS, *subcommand)
               ? CommandIntent::ReadOnly
               : CommandIntent::Write;
}

static CommandIntent classify_by_first_command(std::string_view first,
                                               std::string_view command) {
    if (array_contains(SEMANTIC_READ_ONLY_COMMANDS, first)) {
        // sed -i is a write operation even though sed is read-only by default.
        if (first == "sed" && command.find(" -i") != std::string_view::npos)
            return CommandIntent::Write;
        return CommandIntent::ReadOnly;
    }

    if (array_contains(ALWAYS_DESTRUCTIVE_COMMANDS, first) || first == "rm")
        return CommandIntent::Destructive;

    if (array_contains(WRITE_COMMANDS, first))
        return CommandIntent::Write;

    if (array_contains(NETWORK_COMMANDS, first))
        return CommandIntent::Network;

    if (array_contains(PROCESS_COMMANDS, first))
        return CommandIntent::ProcessManagement;

    if (array_contains(PACKAGE_COMMANDS, first))
        return CommandIntent::PackageManagement;

    if (array_contains(SYSTEM_ADMIN_COMMANDS, first))
        return CommandIntent::SystemAdmin;

    if (first == "git")
        return classify_git_command(command);

    return CommandIntent::Unknown;
}

CommandIntent classify_command(std::string_view command) {
    const std::string first = extract_first_command(command);
    return classify_by_first_command(first, command);
}

// ---------------------------------------------------------------------------
// Full validation pipeline
// ---------------------------------------------------------------------------

ValidationResult validate_command(std::string_view command,
                                  PermissionMode mode,
                                  const std::filesystem::path& workspace) {
    // 1. Mode-level validation (includes read-only checks).
    {
        auto result = validate_mode(command, mode);
        if (!std::holds_alternative<ValidationAllow>(result))
            return result;
    }

    // 2. Sed-specific validation.
    {
        auto result = validate_sed(command, mode);
        if (!std::holds_alternative<ValidationAllow>(result))
            return result;
    }

    // 3. Destructive command warnings.
    {
        auto result = check_destructive(command);
        if (!std::holds_alternative<ValidationAllow>(result))
            return result;
    }

    // 4. Path validation.
    return validate_paths(command, workspace);
}

} // namespace claw::runtime
