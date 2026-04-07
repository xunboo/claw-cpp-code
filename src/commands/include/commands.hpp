#pragma once
// C++20 translation of rust/crates/commands/src/lib.rs
//
// Key mapping decisions:
//   Rust Result<T,E>       -> tl::expected<T,E> (C++23) approximated via a
//                             hand-rolled Result<T,E> template below
//   Rust Option<T>         -> std::optional<T>
//   Rust &'static str      -> const char* / std::string_view
//   Rust enum w/ data      -> std::variant  (or individual struct-tagged unions)
//   Rust Vec<T>            -> std::vector<T>
//   Rust BTreeMap          -> std::map
//   #[must_use]            -> [[nodiscard]]

#include "plugin_types.hpp"
#include "runtime_types.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace claw::commands {

// ---------------------------------------------------------------------------
// Minimal Result<T,E> template (std::expected-like, usable in C++20)
// ---------------------------------------------------------------------------

template <typename T, typename E>
class Result {
public:
    // Construct an Ok value
    static Result ok(T val)  { Result r; r.ok_ = std::move(val); return r; }
    // Construct an Err value
    static Result err(E e)   { Result r; r.err_ = std::move(e);  return r; }

    [[nodiscard]] bool has_value() const noexcept { return ok_.has_value(); }
    [[nodiscard]] bool is_error()  const noexcept { return err_.has_value(); }

    [[nodiscard]] T&       value()       { return *ok_; }
    [[nodiscard]] const T& value() const { return *ok_; }

    [[nodiscard]] E&       error()       { return *err_; }
    [[nodiscard]] const E& error() const { return *err_; }

private:
    std::optional<T> ok_;
    std::optional<E> err_;
};

// Specialisation: Result<void, E>
template <typename E>
class Result<void, E> {
public:
    static Result ok()       { Result r; r.is_ok_ = true;           return r; }
    static Result err(E e)   { Result r; r.err_ = std::move(e);     return r; }

    [[nodiscard]] bool has_value() const noexcept { return is_ok_; }
    [[nodiscard]] bool is_error()  const noexcept { return err_.has_value(); }

    [[nodiscard]] E&       error()       { return *err_; }
    [[nodiscard]] const E& error() const { return *err_; }

private:
    bool             is_ok_{false};
    std::optional<E> err_;
};

// ---------------------------------------------------------------------------
// CommandSource
// ---------------------------------------------------------------------------

enum class CommandSource : std::uint8_t {
    Builtin,
    InternalOnly,
    FeatureGated,
};

// ---------------------------------------------------------------------------
// CommandManifestEntry
// ---------------------------------------------------------------------------

struct CommandManifestEntry {
    std::string   name;
    CommandSource source;

    bool operator==(const CommandManifestEntry&) const = default;
};

// ---------------------------------------------------------------------------
// CommandRegistry
// ---------------------------------------------------------------------------

class CommandRegistry {
public:
    CommandRegistry() = default;
    explicit CommandRegistry(std::vector<CommandManifestEntry> entries)
        : entries_(std::move(entries)) {}

    [[nodiscard]] std::span<const CommandManifestEntry> entries() const noexcept {
        return entries_;
    }

    bool operator==(const CommandRegistry&) const = default;

private:
    std::vector<CommandManifestEntry> entries_;
};

// ---------------------------------------------------------------------------
// SlashCommandSpec
// ---------------------------------------------------------------------------

struct SlashCommandSpec {
    const char*                     name;
    std::span<const char* const>    aliases;
    const char*                     summary;
    const char*                     argument_hint;  // nullptr means no hint
    bool                            resume_supported;
};

// ---------------------------------------------------------------------------
// SlashCommandParseError
// ---------------------------------------------------------------------------

class SlashCommandParseError : public std::exception {
public:
    explicit SlashCommandParseError(std::string msg) : message_(std::move(msg)) {}
    [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    bool operator==(const SlashCommandParseError& other) const noexcept {
        return message_ == other.message_;
    }

private:
    std::string message_;
};

// ---------------------------------------------------------------------------
// SlashCommand – Rust enum with optional data fields
// Each variant that carries data is represented as a struct tag inside a
// std::variant.  No-data variants are represented by empty structs.
// ---------------------------------------------------------------------------

namespace slash_command {
    // No-argument variants
    struct Help              {};
    struct Status            {};
    struct Sandbox           {};
    struct Compact           {};
    struct Commit            {};
    struct DebugToolCall     {};
    struct Cost              {};
    struct Memory            {};
    struct Init              {};
    struct Diff              {};
    struct Version           {};
    struct Doctor            {};
    struct Login             {};
    struct Logout            {};
    struct Vim               {};
    struct Upgrade           {};
    struct Stats             {};
    struct Share             {};
    struct Feedback          {};
    struct Files             {};
    struct Fast              {};
    struct Exit              {};
    struct Summary           {};
    struct Desktop           {};
    struct Brief             {};
    struct Advisor           {};
    struct Stickers          {};
    struct Insights          {};
    struct Thinkback         {};
    struct ReleaseNotes      {};
    struct SecurityReview    {};
    struct Keybindings       {};
    struct PrivacySettings   {};

    // Variants with optional/required data
    struct Bughunter         { std::optional<std::string> scope; };
    struct Pr                { std::optional<std::string> context; };
    struct Issue             { std::optional<std::string> context; };
    struct Ultraplan         { std::optional<std::string> task; };
    struct Teleport          { std::optional<std::string> target; };
    struct Model             { std::optional<std::string> model; };
    struct Permissions       { std::optional<std::string> mode; };
    struct Clear             { bool confirm{false}; };
    struct Resume            { std::optional<std::string> session_path; };
    struct Config            { std::optional<std::string> section; };
    struct Mcp               { std::optional<std::string> action; std::optional<std::string> target; };
    struct Export            { std::optional<std::string> path; };
    struct Session           { std::optional<std::string> action; std::optional<std::string> target; };
    struct Plugins           { std::optional<std::string> action; std::optional<std::string> target; };
    struct Agents            { std::optional<std::string> args; };
    struct Skills            { std::optional<std::string> args; };
    struct Plan              { std::optional<std::string> mode; };
    struct Review            { std::optional<std::string> scope; };
    struct Tasks             { std::optional<std::string> args; };
    struct Theme             { std::optional<std::string> name; };
    struct Voice             { std::optional<std::string> mode; };
    struct Usage             { std::optional<std::string> scope; };
    struct Rename            { std::optional<std::string> name; };
    struct Copy              { std::optional<std::string> target; };
    struct Hooks             { std::optional<std::string> args; };
    struct Context           { std::optional<std::string> action; };
    struct Color             { std::optional<std::string> scheme; };
    struct Effort            { std::optional<std::string> level; };
    struct Branch            { std::optional<std::string> name; };
    struct Rewind            { std::optional<std::string> steps; };
    struct Ide               { std::optional<std::string> target; };
    struct Tag               { std::optional<std::string> label; };
    struct OutputStyle       { std::optional<std::string> style; };
    struct AddDir            { std::optional<std::string> path; };
    struct Unknown           { std::string name; };
} // namespace slash_command

using SlashCommand = std::variant<
    slash_command::Help,
    slash_command::Status,
    slash_command::Sandbox,
    slash_command::Compact,
    slash_command::Commit,
    slash_command::DebugToolCall,
    slash_command::Cost,
    slash_command::Memory,
    slash_command::Init,
    slash_command::Diff,
    slash_command::Version,
    slash_command::Doctor,
    slash_command::Login,
    slash_command::Logout,
    slash_command::Vim,
    slash_command::Upgrade,
    slash_command::Stats,
    slash_command::Share,
    slash_command::Feedback,
    slash_command::Files,
    slash_command::Fast,
    slash_command::Exit,
    slash_command::Summary,
    slash_command::Desktop,
    slash_command::Brief,
    slash_command::Advisor,
    slash_command::Stickers,
    slash_command::Insights,
    slash_command::Thinkback,
    slash_command::ReleaseNotes,
    slash_command::SecurityReview,
    slash_command::Keybindings,
    slash_command::PrivacySettings,
    slash_command::Bughunter,
    slash_command::Pr,
    slash_command::Issue,
    slash_command::Ultraplan,
    slash_command::Teleport,
    slash_command::Model,
    slash_command::Permissions,
    slash_command::Clear,
    slash_command::Resume,
    slash_command::Config,
    slash_command::Mcp,
    slash_command::Export,
    slash_command::Session,
    slash_command::Plugins,
    slash_command::Agents,
    slash_command::Skills,
    slash_command::Plan,
    slash_command::Review,
    slash_command::Tasks,
    slash_command::Theme,
    slash_command::Voice,
    slash_command::Usage,
    slash_command::Rename,
    slash_command::Copy,
    slash_command::Hooks,
    slash_command::Context,
    slash_command::Color,
    slash_command::Effort,
    slash_command::Branch,
    slash_command::Rewind,
    slash_command::Ide,
    slash_command::Tag,
    slash_command::OutputStyle,
    slash_command::AddDir,
    slash_command::Unknown
>;

// ---------------------------------------------------------------------------
// SlashCommandResult
// ---------------------------------------------------------------------------

struct SlashCommandResult {
    std::string       message;
    runtime::Session  session;
};

// ---------------------------------------------------------------------------
// PluginsCommandResult
// ---------------------------------------------------------------------------

struct PluginsCommandResult {
    std::string message;
    bool        reload_runtime{false};
};

// ---------------------------------------------------------------------------
// Public API – free functions
// ---------------------------------------------------------------------------

// Parse a raw input string.
// Returns Ok(nullopt)   if the input is not a slash command (no leading '/').
// Returns Ok(some(cmd)) if parsed successfully.
// Returns Err(e)        on a parse error.
[[nodiscard]]
Result<std::optional<SlashCommand>, SlashCommandParseError>
parse_slash_command(std::string_view input);

// Identical to parse_slash_command – exposed for direct validation.
[[nodiscard]]
Result<std::optional<SlashCommand>, SlashCommandParseError>
validate_slash_command_input(std::string_view input);

// Static table of all known slash command specs.
[[nodiscard]] std::span<const SlashCommandSpec> slash_command_specs() noexcept;

// Specs whose resume_supported flag is true.
[[nodiscard]] std::vector<const SlashCommandSpec*> resume_supported_slash_commands();

// Render the /help text shown to the user.
[[nodiscard]] std::string render_slash_command_help();

// Render detail help for a single command (identified by name or alias).
[[nodiscard]] std::optional<std::string> render_slash_command_help_detail(std::string_view name);

// Fuzzy-match suggestions for a partial input string.
[[nodiscard]] std::vector<std::string> suggest_slash_commands(std::string_view input, std::size_t limit);

// Handle a slash command that can be resolved purely in-process
// (e.g. /help, /compact).  Returns nullopt for runtime-bound commands.
[[nodiscard]]
std::optional<SlashCommandResult>
handle_slash_command(std::string_view input,
                     const runtime::Session& session,
                     runtime::CompactionConfig compaction);

// Plugin command handler.
[[nodiscard]]
Result<PluginsCommandResult, plugins::PluginError>
handle_plugins_slash_command(std::optional<std::string_view> action,
                             std::optional<std::string_view> target,
                             plugins::PluginManager&          manager);

// Agents / Skills / MCP command handlers.
[[nodiscard]] Result<std::string, std::error_code>
handle_agents_slash_command(std::optional<std::string_view> args,
                            const std::filesystem::path&    cwd);

[[nodiscard]] Result<std::string, std::error_code>
handle_skills_slash_command(std::optional<std::string_view> args,
                            const std::filesystem::path&    cwd);

// Skill invocation dispatch -- mirrors Rust's SkillSlashDispatch.
enum class SkillSlashDispatch : std::uint8_t { Local, Invoke };

struct SkillSlashDispatchResult {
    SkillSlashDispatch kind{SkillSlashDispatch::Local};
    std::string        invoke_prompt;  // non-empty only when kind == Invoke
};

// Classify a /skills slash-command invocation into local (list/help/install)
// or invoke (direct skill dispatch).
[[nodiscard]] SkillSlashDispatchResult
classify_skills_slash_command(std::optional<std::string_view> args);

// Validate that a skill invocation refers to a real skill on disk.
// Returns Err(message) with available-skills help when the skill is unknown.
[[nodiscard]] Result<SkillSlashDispatchResult, std::string>
resolve_skill_invocation(const std::filesystem::path& cwd,
                         std::optional<std::string_view> args);

[[nodiscard]] Result<std::string, runtime::ConfigError>
handle_mcp_slash_command(std::optional<std::string_view> args,
                         const std::filesystem::path&    cwd);

// Plugin list rendering helper (also called from tests / CLI shells).
[[nodiscard]] std::string render_plugins_report(std::span<const plugins::PluginSummary> plugins);

} // namespace claw::commands
