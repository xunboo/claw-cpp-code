#pragma once
// init.hpp -- C++20 port of init.rs
// Repository initialisation: InitStatus, InitArtifact, InitReport, initialize_repo

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

namespace claw {

/// Mirrors Rust's InitStatus enum.
enum class InitStatus {
    Created,
    Updated,
    Skipped,
};

/// Returns the human-readable label for an InitStatus value.
[[nodiscard]] constexpr std::string_view init_status_label(InitStatus s) noexcept {
    switch (s) {
        case InitStatus::Created: return "created";
        case InitStatus::Updated: return "updated";
        case InitStatus::Skipped: return "skipped (already exists)";
    }
    return "skipped (already exists)";
}

/// Mirrors Rust's InitArtifact struct.
struct InitArtifact {
    std::string name;   // e.g. ".claude.json"
    InitStatus status;
};

/// Mirrors Rust's InitReport struct.
struct InitReport {
    std::filesystem::path project_root;
    std::vector<InitArtifact> artifacts;

    /// Render a human-readable summary.
    [[nodiscard]] std::string render() const;
};

/// Mirrors Rust's RepoDetection struct (internal, exposed for testing).
struct RepoDetection {
    bool rust_workspace{false};
    bool rust_root{false};
    bool python{false};
    bool package_json{false};
    bool typescript{false};
    bool nextjs{false};
    bool react{false};
    bool vite{false};
    bool nest{false};
    bool src_dir{false};
    bool tests_dir{false};
    bool rust_dir{false};
};

/// Scan the filesystem under cwd and populate a RepoDetection.
[[nodiscard]] RepoDetection detect_repo(const std::filesystem::path& cwd);

/// Build the CLAUDE.md template content for the given directory.
[[nodiscard]] std::string render_init_claude_md(const std::filesystem::path& cwd);

/// Initialise a project directory (create .claude/, .claude.json, .gitignore, CLAUDE.md).
/// Throws std::runtime_error on I/O failure.
[[nodiscard]] InitReport initialize_repo(const std::filesystem::path& cwd);

} // namespace claw