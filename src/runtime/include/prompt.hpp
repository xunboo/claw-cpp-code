#pragma once
// C++20 conversion of prompt.rs
// Mirrors every public type and function from the Rust source.

#include "config.hpp"

#include <tl/expected.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace claw::runtime {

// ---------------------------------------------------------------------------
// Constants (pub const in Rust)
// ---------------------------------------------------------------------------

/// Marker separating static prompt scaffolding from dynamic runtime context.
extern const std::string_view SYSTEM_PROMPT_DYNAMIC_BOUNDARY; // "__SYSTEM_PROMPT_DYNAMIC_BOUNDARY__"
/// Human-readable default frontier model name embedded into generated prompts.
extern const std::string_view FRONTIER_MODEL_NAME;            // "Claude Opus 4.6"

// ---------------------------------------------------------------------------
// PromptBuildError  (mirrors Rust enum PromptBuildError { Io, Config })
// ---------------------------------------------------------------------------

/// Errors raised while assembling the final system prompt.
struct PromptBuildError {
    enum class Kind { Io, Config };
    Kind        kind{Kind::Io};
    std::string message;

    [[nodiscard]] static PromptBuildError from_io(std::string msg);
    [[nodiscard]] static PromptBuildError from_config(std::string msg);
    [[nodiscard]] std::string             to_string() const;
};

// ---------------------------------------------------------------------------
// ContextFile  (mirrors Rust struct ContextFile)
// ---------------------------------------------------------------------------

/// Contents of an instruction file included in prompt construction.
struct ContextFile {
    std::filesystem::path path;
    std::string           content;
};

// ---------------------------------------------------------------------------
// ProjectContext  (mirrors Rust struct ProjectContext)
// ---------------------------------------------------------------------------

/// Project-local context injected into the rendered system prompt.
struct ProjectContext {
    std::filesystem::path    cwd;
    std::string              current_date;
    std::optional<std::string> git_status;
    std::optional<std::string> git_diff;
    std::vector<ContextFile>   instruction_files;

    // Mirrors ProjectContext::discover
    [[nodiscard]] static tl::expected<ProjectContext, PromptBuildError>
    discover(const std::filesystem::path& cwd, std::string current_date);

    // Mirrors ProjectContext::discover_with_git
    [[nodiscard]] static tl::expected<ProjectContext, PromptBuildError>
    discover_with_git(const std::filesystem::path& cwd, std::string current_date);
};

// ---------------------------------------------------------------------------
// SystemPromptBuilder  (mirrors Rust struct SystemPromptBuilder)
// ---------------------------------------------------------------------------

/// Builder for the runtime system prompt and dynamic environment sections.
class SystemPromptBuilder {
public:
    SystemPromptBuilder() = default;

    // Builder-style setters (return *this for chaining, matching Rust #[must_use] builder pattern)
    SystemPromptBuilder& with_output_style(std::string name, std::string prompt);
    SystemPromptBuilder& with_os(std::string os_name, std::string os_version);
    SystemPromptBuilder& with_project_context(ProjectContext ctx);
    SystemPromptBuilder& with_runtime_config(RuntimeConfig config);
    SystemPromptBuilder& append_section(std::string section);

    // Mirrors Rust build() -> Vec<String>
    [[nodiscard]] std::vector<std::string> build() const;

    // Mirrors Rust render() -> String  (build().join("\n\n"))
    [[nodiscard]] std::string render() const;

private:
    std::optional<std::string>   output_style_name_;
    std::optional<std::string>   output_style_prompt_;
    std::optional<std::string>   os_name_;
    std::optional<std::string>   os_version_;
    std::optional<ProjectContext> project_context_;
    std::optional<RuntimeConfig>  config_;
    std::vector<std::string>      append_sections_;

    // Mirrors Rust fn environment_section(&self) -> String
    [[nodiscard]] std::string environment_section() const;
};

// ---------------------------------------------------------------------------
// Free functions (mirrors Rust pub fn / private fn with public tests)
// ---------------------------------------------------------------------------

/// Formats each item as an indented bullet for prompt sections.
[[nodiscard]] std::vector<std::string> prepend_bullets(std::vector<std::string> items);

/// Loads config and project context, then renders the system prompt text.
[[nodiscard]] tl::expected<std::vector<std::string>, PromptBuildError>
load_system_prompt(const std::filesystem::path& cwd,
                   std::string                  current_date,
                   std::string                  os_name,
                   std::string                  os_version);

} // namespace claw::runtime
