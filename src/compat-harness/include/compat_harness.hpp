#pragma once

//
// compat_harness.hpp
// C++20 port of crates/compat-harness/src/lib.rs
//
// Reads the upstream Claude Code TypeScript source tree and extracts a
// manifest of commands, tools and the bootstrap phase sequence.
//

#include "bootstrap.hpp"
#include "commands.hpp"
#include "tools.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <tl/expected.hpp>

namespace claw {

// ---------------------------------------------------------------------------
// UpstreamPaths
// ---------------------------------------------------------------------------

/// Resolves the set of canonical source-file paths inside the upstream
/// Claude Code repository tree.
class UpstreamPaths {
public:
    /// Construct directly from a known repo root.
    [[nodiscard]] static UpstreamPaths from_repo_root(std::filesystem::path repo_root);

    /// Derive from a workspace directory: walks up one level, then probes
    /// several candidate sibling directories for the upstream repo.
    [[nodiscard]] static UpstreamPaths from_workspace_dir(const std::filesystem::path& workspace_dir);

    // Path accessors ----------------------------------------------------------

    [[nodiscard]] std::filesystem::path commands_path() const;
    [[nodiscard]] std::filesystem::path tools_path()    const;
    [[nodiscard]] std::filesystem::path cli_path()      const;

    [[nodiscard]] bool operator==(const UpstreamPaths& other) const noexcept {
        return repo_root_ == other.repo_root_;
    }

private:
    explicit UpstreamPaths(std::filesystem::path repo_root)
        : repo_root_(std::move(repo_root)) {}

    std::filesystem::path repo_root_;
};

// ---------------------------------------------------------------------------
// ExtractedManifest
// ---------------------------------------------------------------------------

/// The full manifest extracted from an upstream source tree.
struct ExtractedManifest {
    CommandRegistry commands;
    ToolRegistry    tools;
    BootstrapPlan   bootstrap;
};

// ---------------------------------------------------------------------------
// Top-level extraction API
// ---------------------------------------------------------------------------

/// Read the three source files referenced by `paths` and return the combined
/// manifest.  Returns a std::error_code on I/O failure.
[[nodiscard]] tl::expected<ExtractedManifest, std::error_code>
extract_manifest(const UpstreamPaths& paths);

/// Parse `source` (the text of `commands.ts`) and return a CommandRegistry.
[[nodiscard]] CommandRegistry extract_commands(std::string_view source);

/// Parse `source` (the text of `tools.ts`) and return a ToolRegistry.
[[nodiscard]] ToolRegistry extract_tools(std::string_view source);

/// Parse `source` (the text of `cli.tsx`) and return a BootstrapPlan.
[[nodiscard]] BootstrapPlan extract_bootstrap_plan(std::string_view source);

} // namespace claw