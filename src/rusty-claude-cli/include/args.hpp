#pragma once
// args.hpp -- C++20 port of args.rs
// CLI argument definitions: PermissionMode, OutputFormat, Subcommand, Cli

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace claw {

/// Maps to Rust's PermissionMode enum.
enum class PermissionMode {
    ReadOnly,
    WorkspaceWrite,
    DangerFullAccess,
};

/// Maps to Rust's OutputFormat enum.
enum class OutputFormat {
    Text,
    Json,
    Ndjson,
};

/// Maps to Rust's Command (subcommand) enum.
enum class SubcommandKind {
    None,
    DumpManifests,
    BootstrapPlan,
    Login,
    Logout,
    Prompt,
};

struct Subcommand {
    SubcommandKind kind{SubcommandKind::None};
    /// Populated only when kind == Prompt.
    std::vector<std::string> prompt_words;
};

/// Top-level parsed CLI structure -- mirrors Rust's Cli struct.
struct Cli {
    std::string model{"claude-opus-4-6"};
    PermissionMode permission_mode{PermissionMode::DangerFullAccess};
    std::optional<std::filesystem::path> config;
    OutputFormat output_format{OutputFormat::Text};
    Subcommand command;
};

/// Parse argc/argv into a Cli.
/// Throws std::runtime_error on unknown flags or missing values.
[[nodiscard]] Cli parse_cli(int argc, char* argv[]);

// ---- constexpr helpers ----

[[nodiscard]] constexpr std::string_view permission_mode_label(PermissionMode mode) noexcept {
    switch (mode) {
        case PermissionMode::ReadOnly:         return "read-only";
        case PermissionMode::WorkspaceWrite:   return "workspace-write";
        case PermissionMode::DangerFullAccess: return "danger-full-access";
    }
    return "danger-full-access";
}

[[nodiscard]] constexpr std::string_view output_format_label(OutputFormat fmt) noexcept {
    switch (fmt) {
        case OutputFormat::Text:   return "text";
        case OutputFormat::Json:   return "json";
        case OutputFormat::Ndjson: return "ndjson";
    }
    return "text";
}

/// Returns nullopt when the string is not a recognised permission-mode label.
[[nodiscard]] std::optional<PermissionMode> permission_mode_from_str(std::string_view s) noexcept;

/// Returns nullopt when the string is not a recognised output-format label.
[[nodiscard]] std::optional<OutputFormat> output_format_from_str(std::string_view s) noexcept;

} // namespace claw