// C++20 translation of rust/crates/commands/src/lib.rs
//
// Each Rust function maps 1-to-1 to a C++ function.  Helper functions that
// were private in Rust are defined as file-static (anonymous-namespace) here.

#include "commands.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace claw::commands {

// ===========================================================================
// Alias table for SlashCommandSpec static arrays
// ===========================================================================

// The Rust code uses &'static [&'static str] for aliases.  In C++20 we store
// them as null-terminated const char* arrays whose addresses are stable.

// Individual alias lists (one per command that has aliases)
static constexpr const char* ALIAS_PLUGIN[]      = {"plugins", "marketplace"};
static constexpr const char* ALIAS_APPROVE[]     = {"yes", "y"};
static constexpr const char* ALIAS_DENY[]        = {"no", "n"};
static constexpr const char* ALIAS_WORKSPACE[]   = {"cwd"};
static constexpr const char* ALIAS_SKILL[]       = {"skill"};
static constexpr const char* ALIAS_EMPTY[]       = {nullptr};  // Shared empty alias list

// ---------------------------------------------------------------------------
// The static table  – mirrors SLASH_COMMAND_SPECS in Rust.
// Fields: name, aliases (span), summary, argument_hint (nullptr = none),
//         resume_supported.
// ---------------------------------------------------------------------------
static constexpr SlashCommandSpec SLASH_COMMAND_SPECS[] = {
    {"help",            {ALIAS_EMPTY,     0},  "Show available slash commands",                                                nullptr,                                               true},
    {"status",          {ALIAS_EMPTY,     0},  "Show current session status",                                                 nullptr,                                               true},
    {"sandbox",         {ALIAS_EMPTY,     0},  "Show sandbox isolation status",                                               nullptr,                                               true},
    {"compact",         {ALIAS_EMPTY,     0},  "Compact local session history",                                               nullptr,                                               true},
    {"model",           {ALIAS_EMPTY,     0},  "Show or switch the active model",                                             "[model]",                                             false},
    {"permissions",     {ALIAS_EMPTY,     0},  "Show or switch the active permission mode",                                   "[read-only|workspace-write|danger-full-access]",      false},
    {"clear",           {ALIAS_EMPTY,     0},  "Start a fresh local session",                                                 "[--confirm]",                                         true},
    {"cost",            {ALIAS_EMPTY,     0},  "Show cumulative token usage for this session",                                nullptr,                                               true},
    {"resume",          {ALIAS_EMPTY,     0},  "Load a saved session into the REPL",                                          "<session-path>",                                      false},
    {"config",          {ALIAS_EMPTY,     0},  "Inspect Claude config files or merged sections",                              "[env|hooks|model|plugins]",                           true},
    {"mcp",             {ALIAS_EMPTY,     0},  "Inspect configured MCP servers",                                              "[list|show <server>|help]",                           true},
    {"memory",          {ALIAS_EMPTY,     0},  "Inspect loaded Claude instruction memory files",                              nullptr,                                               true},
    {"init",            {ALIAS_EMPTY,     0},  "Create a starter CLAUDE.md for this repo",                                   nullptr,                                               true},
    {"diff",            {ALIAS_EMPTY,     0},  "Show git diff for current workspace changes",                                 nullptr,                                               true},
    {"version",         {ALIAS_EMPTY,     0},  "Show CLI version and build information",                                      nullptr,                                               true},
    {"bughunter",       {ALIAS_EMPTY,     0},  "Inspect the codebase for likely bugs",                                        "[scope]",                                             false},
    {"commit",          {ALIAS_EMPTY,     0},  "Generate a commit message and create a git commit",                           nullptr,                                               false},
    {"pr",              {ALIAS_EMPTY,     0},  "Draft or create a pull request from the conversation",                        "[context]",                                           false},
    {"issue",           {ALIAS_EMPTY,     0},  "Draft or create a GitHub issue from the conversation",                        "[context]",                                           false},
    {"ultraplan",       {ALIAS_EMPTY,     0},  "Run a deep planning prompt with multi-step reasoning",                        "[task]",                                              false},
    {"teleport",        {ALIAS_EMPTY,     0},  "Jump to a file or symbol by searching the workspace",                         "<symbol-or-path>",                                    false},
    {"debug-tool-call", {ALIAS_EMPTY,     0},  "Replay the last tool call with debug details",                                nullptr,                                               false},
    {"export",          {ALIAS_EMPTY,     0},  "Export the current conversation to a file",                                   "[file]",                                              true},
    {"session",         {ALIAS_EMPTY,     0},  "List, switch, or fork managed local sessions",                                "[list|switch <session-id>|fork [branch-name]]",       false},
    {"plugin",          {ALIAS_PLUGIN,    2},  "Manage Claw Code plugins",                                                    "[list|install <path>|enable <name>|disable <name>|uninstall <id>|update <id>]", false},
    {"agents",          {ALIAS_EMPTY,     0},  "List configured agents",                                                      "[list|help]",                                         true},
    {"skills",          {ALIAS_SKILL,     1},  "List, install, or invoke available skills",                                   "[list|install <path>|help|<skill> [args]]",           true},
    {"doctor",          {ALIAS_EMPTY,     0},  "Diagnose setup issues and environment health",                                nullptr,                                               true},
    {"login",           {ALIAS_EMPTY,     0},  "Log in to the service",                                                       nullptr,                                               false},
    {"logout",          {ALIAS_EMPTY,     0},  "Log out of the current session",                                              nullptr,                                               false},
    {"plan",            {ALIAS_EMPTY,     0},  "Toggle or inspect planning mode",                                             "[on|off]",                                            true},
    {"review",          {ALIAS_EMPTY,     0},  "Run a code review on current changes",                                        "[scope]",                                             false},
    {"tasks",           {ALIAS_EMPTY,     0},  "List and manage background tasks",                                            "[list|get <id>|stop <id>]",                           true},
    {"theme",           {ALIAS_EMPTY,     0},  "Switch the terminal color theme",                                             "[theme-name]",                                        true},
    {"vim",             {ALIAS_EMPTY,     0},  "Toggle vim keybinding mode",                                                  nullptr,                                               true},
    {"voice",           {ALIAS_EMPTY,     0},  "Toggle voice input mode",                                                     "[on|off]",                                            false},
    {"upgrade",         {ALIAS_EMPTY,     0},  "Check for and install CLI updates",                                           nullptr,                                               false},
    {"usage",           {ALIAS_EMPTY,     0},  "Show detailed API usage statistics",                                          nullptr,                                               true},
    {"stats",           {ALIAS_EMPTY,     0},  "Show workspace and session statistics",                                       nullptr,                                               true},
    {"rename",          {ALIAS_EMPTY,     0},  "Rename the current session",                                                  "<name>",                                              false},
    {"copy",            {ALIAS_EMPTY,     0},  "Copy conversation or output to clipboard",                                    "[last|all]",                                          true},
    {"share",           {ALIAS_EMPTY,     0},  "Share the current conversation",                                              nullptr,                                               false},
    {"feedback",        {ALIAS_EMPTY,     0},  "Submit feedback about the current session",                                   nullptr,                                               false},
    {"hooks",           {ALIAS_EMPTY,     0},  "List and manage lifecycle hooks",                                             "[list|run <hook>]",                                   true},
    {"files",           {ALIAS_EMPTY,     0},  "List files in the current context window",                                    nullptr,                                               true},
    {"context",         {ALIAS_EMPTY,     0},  "Inspect or manage the conversation context",                                  "[show|clear]",                                        true},
    {"color",           {ALIAS_EMPTY,     0},  "Configure terminal color settings",                                           "[scheme]",                                            true},
    {"effort",          {ALIAS_EMPTY,     0},  "Set the effort level for responses",                                          "[low|medium|high]",                                   true},
    {"fast",            {ALIAS_EMPTY,     0},  "Toggle fast/concise response mode",                                           nullptr,                                               true},
    {"exit",            {ALIAS_EMPTY,     0},  "Exit the REPL session",                                                       nullptr,                                               false},
    {"branch",          {ALIAS_EMPTY,     0},  "Create or switch git branches",                                               "[name]",                                              false},
    {"rewind",          {ALIAS_EMPTY,     0},  "Rewind the conversation to a previous state",                                 "[steps]",                                             false},
    {"summary",         {ALIAS_EMPTY,     0},  "Generate a summary of the conversation",                                      nullptr,                                               true},
    {"desktop",         {ALIAS_EMPTY,     0},  "Open or manage the desktop app integration",                                  nullptr,                                               false},
    {"ide",             {ALIAS_EMPTY,     0},  "Open or configure IDE integration",                                           "[vscode|cursor]",                                     false},
    {"tag",             {ALIAS_EMPTY,     0},  "Tag the current conversation point",                                          "[label]",                                             true},
    {"brief",           {ALIAS_EMPTY,     0},  "Toggle brief output mode",                                                    nullptr,                                               true},
    {"advisor",         {ALIAS_EMPTY,     0},  "Toggle advisor mode for guidance-only responses",                             nullptr,                                               true},
    {"stickers",        {ALIAS_EMPTY,     0},  "Browse and manage sticker packs",                                             nullptr,                                               true},
    {"insights",        {ALIAS_EMPTY,     0},  "Show AI-generated insights about the session",                                nullptr,                                               true},
    {"thinkback",       {ALIAS_EMPTY,     0},  "Replay the thinking process of the last response",                            nullptr,                                               true},
    {"release-notes",   {ALIAS_EMPTY,     0},  "Generate release notes from recent changes",                                  nullptr,                                               false},
    {"security-review", {ALIAS_EMPTY,     0},  "Run a security review on the codebase",                                       "[scope]",                                             false},
    {"keybindings",     {ALIAS_EMPTY,     0},  "Show or configure keyboard shortcuts",                                        nullptr,                                               true},
    {"privacy-settings",{ALIAS_EMPTY,     0},  "View or modify privacy settings",                                             nullptr,                                               true},
    {"output-style",    {ALIAS_EMPTY,     0},  "Switch output formatting style",                                              "[style]",                                             true},
    {"add-dir",         {ALIAS_EMPTY,     0},  "Add an additional directory to the context",                                  "<path>",                                              false},
    {"allowed-tools",   {ALIAS_EMPTY,     0},  "Show or modify the allowed tools list",                                       "[add|remove|list] [tool]",                            true},
    {"api-key",         {ALIAS_EMPTY,     0},  "Show or set the Anthropic API key",                                           "[key]",                                               false},
    {"approve",         {ALIAS_APPROVE,   2},  "Approve a pending tool execution",                                            nullptr,                                               false},
    {"deny",            {ALIAS_DENY,      2},  "Deny a pending tool execution",                                               nullptr,                                               false},
    {"undo",            {ALIAS_EMPTY,     0},  "Undo the last file write or edit",                                            nullptr,                                               false},
    {"stop",            {ALIAS_EMPTY,     0},  "Stop the current generation",                                                 nullptr,                                               false},
    {"retry",           {ALIAS_EMPTY,     0},  "Retry the last failed message",                                               nullptr,                                               false},
    {"paste",           {ALIAS_EMPTY,     0},  "Paste clipboard content as input",                                            nullptr,                                               false},
    {"screenshot",      {ALIAS_EMPTY,     0},  "Take a screenshot and add to conversation",                                   nullptr,                                               false},
    {"image",           {ALIAS_EMPTY,     0},  "Add an image file to the conversation",                                       "<path>",                                              false},
    {"terminal-setup",  {ALIAS_EMPTY,     0},  "Configure terminal integration settings",                                     nullptr,                                               true},
    {"search",          {ALIAS_EMPTY,     0},  "Search files in the workspace",                                               "<query>",                                             false},
    {"listen",          {ALIAS_EMPTY,     0},  "Listen for voice input",                                                      nullptr,                                               false},
    {"speak",           {ALIAS_EMPTY,     0},  "Read the last response aloud",                                                nullptr,                                               false},
    {"language",        {ALIAS_EMPTY,     0},  "Set the interface language",                                                  "[language]",                                          true},
    {"profile",         {ALIAS_EMPTY,     0},  "Show or switch user profile",                                                 "[name]",                                              false},
    {"max-tokens",      {ALIAS_EMPTY,     0},  "Show or set the max output tokens",                                           "[count]",                                             true},
    {"temperature",     {ALIAS_EMPTY,     0},  "Show or set the sampling temperature",                                        "[value]",                                             true},
    {"system-prompt",   {ALIAS_EMPTY,     0},  "Show the active system prompt",                                               nullptr,                                               true},
    {"tool-details",    {ALIAS_EMPTY,     0},  "Show detailed info about a specific tool",                                    "<tool-name>",                                         true},
    {"format",          {ALIAS_EMPTY,     0},  "Format the last response in a different style",                               "[markdown|plain|json]",                               false},
    {"pin",             {ALIAS_EMPTY,     0},  "Pin a message to persist across compaction",                                  "[message-index]",                                     false},
    {"unpin",           {ALIAS_EMPTY,     0},  "Unpin a previously pinned message",                                           "[message-index]",                                     false},
    {"bookmarks",       {ALIAS_EMPTY,     0},  "List or manage conversation bookmarks",                                       "[add|remove|list]",                                   true},
    {"workspace",       {ALIAS_WORKSPACE, 1},  "Show or change the working directory",                                        "[path]",                                              true},
    {"history",         {ALIAS_EMPTY,     0},  "Show conversation history summary",                                           "[count]",                                             true},
    {"tokens",          {ALIAS_EMPTY,     0},  "Show token count for the current conversation",                               nullptr,                                               true},
    {"cache",           {ALIAS_EMPTY,     0},  "Show prompt cache statistics",                                                nullptr,                                               true},
    {"providers",       {ALIAS_EMPTY,     0},  "List available model providers",                                              nullptr,                                               true},
    {"notifications",   {ALIAS_EMPTY,     0},  "Show or configure notification settings",                                     "[on|off|status]",                                     true},
    {"changelog",       {ALIAS_EMPTY,     0},  "Show recent changes to the codebase",                                         "[count]",                                             true},
    {"test",            {ALIAS_EMPTY,     0},  "Run tests for the current project",                                           "[filter]",                                            false},
    {"lint",            {ALIAS_EMPTY,     0},  "Run linting for the current project",                                         "[filter]",                                            false},
    {"build",           {ALIAS_EMPTY,     0},  "Build the current project",                                                   "[target]",                                            false},
    {"run",             {ALIAS_EMPTY,     0},  "Run a command in the project context",                                        "<command>",                                           false},
    {"git",             {ALIAS_EMPTY,     0},  "Run a git command in the workspace",                                          "<subcommand>",                                        false},
    {"stash",           {ALIAS_EMPTY,     0},  "Stash or unstash workspace changes",                                          "[pop|list|apply]",                                    false},
    {"blame",           {ALIAS_EMPTY,     0},  "Show git blame for a file",                                                   "<file> [line]",                                       true},
    {"log",             {ALIAS_EMPTY,     0},  "Show git log for the workspace",                                              "[count]",                                             true},
    {"cron",            {ALIAS_EMPTY,     0},  "Manage scheduled tasks",                                                      "[list|add|remove]",                                   true},
    {"team",            {ALIAS_EMPTY,     0},  "Manage agent teams",                                                          "[list|create|delete]",                                true},
    {"benchmark",       {ALIAS_EMPTY,     0},  "Run performance benchmarks",                                                  "[suite]",                                             false},
    {"migrate",         {ALIAS_EMPTY,     0},  "Run pending data migrations",                                                  nullptr,                                               false},
    {"reset",           {ALIAS_EMPTY,     0},  "Reset configuration to defaults",                                             "[section]",                                           false},
    {"telemetry",       {ALIAS_EMPTY,     0},  "Show or configure telemetry settings",                                        "[on|off|status]",                                     true},
    {"env",             {ALIAS_EMPTY,     0},  "Show environment variables visible to tools",                                  nullptr,                                               true},
    {"project",         {ALIAS_EMPTY,     0},  "Show project detection info",                                                  nullptr,                                               true},
    {"templates",       {ALIAS_EMPTY,     0},  "List or apply prompt templates",                                              "[list|apply <name>]",                                 false},
    {"explain",         {ALIAS_EMPTY,     0},  "Explain a file or code snippet",                                              "<path> [line-range]",                                 false},
    {"refactor",        {ALIAS_EMPTY,     0},  "Suggest refactoring for a file or function",                                  "<path> [scope]",                                      false},
    {"docs",            {ALIAS_EMPTY,     0},  "Generate or show documentation",                                              "[path]",                                              false},
    {"fix",             {ALIAS_EMPTY,     0},  "Fix errors in a file or project",                                             "[path]",                                              false},
    {"perf",            {ALIAS_EMPTY,     0},  "Analyze performance of a function or file",                                   "<path>",                                              false},
    {"chat",            {ALIAS_EMPTY,     0},  "Switch to free-form chat mode",                                               nullptr,                                               false},
    {"focus",           {ALIAS_EMPTY,     0},  "Focus context on specific files or directories",                              "<path> [path...]",                                    false},
    {"unfocus",         {ALIAS_EMPTY,     0},  "Remove focus from files or directories",                                      "[path...]",                                           false},
    {"web",             {ALIAS_EMPTY,     0},  "Fetch and summarize a web page",                                              "<url>",                                               false},
    {"map",             {ALIAS_EMPTY,     0},  "Show a visual map of the codebase structure",                                 "[depth]",                                             true},
    {"symbols",         {ALIAS_EMPTY,     0},  "List symbols (functions, classes, etc.) in a file",                           "<path>",                                              true},
    {"references",      {ALIAS_EMPTY,     0},  "Find all references to a symbol",                                             "<symbol>",                                            false},
    {"definition",      {ALIAS_EMPTY,     0},  "Go to the definition of a symbol",                                            "<symbol>",                                            false},
    {"hover",           {ALIAS_EMPTY,     0},  "Show hover information for a symbol",                                         "<symbol>",                                            true},
    {"diagnostics",     {ALIAS_EMPTY,     0},  "Show LSP diagnostics for a file",                                             "[path]",                                              true},
    {"autofix",         {ALIAS_EMPTY,     0},  "Auto-fix all fixable diagnostics",                                            "[path]",                                              false},
    {"multi",           {ALIAS_EMPTY,     0},  "Execute multiple slash commands in sequence",                                 "<commands>",                                          false},
    {"macro",           {ALIAS_EMPTY,     0},  "Record or replay command macros",                                             "[record|stop|play <name>]",                           false},
    {"alias",           {ALIAS_EMPTY,     0},  "Create a command alias",                                                      "<name> <command>",                                    true},
    {"parallel",        {ALIAS_EMPTY,     0},  "Run commands in parallel subagents",                                          "<count> <prompt>",                                    false},
    {"agent",           {ALIAS_EMPTY,     0},  "Manage sub-agents and spawned sessions",                                      "[list|spawn|kill]",                                   true},
    {"subagent",        {ALIAS_EMPTY,     0},  "Control active subagent execution",                                           "[list|steer <target> <msg>|kill <id>]",               true},
    {"reasoning",       {ALIAS_EMPTY,     0},  "Toggle extended reasoning mode",                                              "[on|off|stream]",                                     true},
    {"budget",          {ALIAS_EMPTY,     0},  "Show or set token budget limits",                                             "[show|set <limit>]",                                  true},
    {"rate-limit",      {ALIAS_EMPTY,     0},  "Configure API rate limiting",                                                 "[status|set <rpm>]",                                  true},
    {"metrics",         {ALIAS_EMPTY,     0},  "Show performance and usage metrics",                                          nullptr,                                               true},
};

// ===========================================================================
// Internal helpers
// ===========================================================================

namespace {

// Trim leading and trailing ASCII whitespace
std::string_view trim_sv(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))  sv.remove_suffix(1);
    return sv;
}

// ASCII lowercase comparison
bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// Split a string_view by whitespace into tokens
std::vector<std::string_view> split_whitespace(std::string_view sv) {
    std::vector<std::string_view> tokens;
    std::size_t start = sv.find_first_not_of(" \t\r\n");
    while (start != std::string_view::npos) {
        std::size_t end = sv.find_first_of(" \t\r\n", start);
        if (end == std::string_view::npos) {
            tokens.push_back(sv.substr(start));
            break;
        }
        tokens.push_back(sv.substr(start, end - start));
        start = sv.find_first_not_of(" \t\r\n", end);
    }
    return tokens;
}

// The text that follows "/<command>" on the command line, trimmed.
// Returns nullopt when the remainder is empty.
std::optional<std::string> remainder_after_command(std::string_view input,
                                                   std::string_view command)
{
    std::string prefix = std::string("/") + std::string(command);
    auto trimmed = trim_sv(input);
    if (trimmed.substr(0, prefix.size()) != prefix) return std::nullopt;
    auto rest = trim_sv(trimmed.substr(prefix.size()));
    if (rest.empty()) return std::nullopt;
    return std::string(rest);
}

// Find a SlashCommandSpec by name or alias (case-insensitive)
const SlashCommandSpec* find_slash_command_spec(std::string_view name) {
    for (const auto& spec : SLASH_COMMAND_SPECS) {
        if (iequal(spec.name, name)) return &spec;
        for (const char* alias : spec.aliases)
            if (iequal(alias, name)) return &spec;
    }
    return nullptr;
}

// Name of the first space-delimited word (used for error messages)
std::string_view command_root_name(std::string_view command) {
    auto pos = command.find(' ');
    return (pos == std::string_view::npos) ? command : command.substr(0, pos);
}

// Build the usage string for a spec
std::string slash_command_usage(const SlashCommandSpec& spec) {
    if (spec.argument_hint)
        return std::format("/{} {}", spec.name, spec.argument_hint);
    return std::format("/{}", spec.name);
}

// ---------------------------------------------------------------------------
// Category lookup (matches Rust slash_command_category function)
// ---------------------------------------------------------------------------
const char* slash_command_category(std::string_view name) {
    // Session & visibility
    for (auto n : {"help","status","sandbox","model","permissions","cost","resume",
                   "session","version","login","logout","usage","stats","rename",
                   "privacy-settings"})
        if (name == n) return "Session & visibility";
    // Workspace & git
    for (auto n : {"compact","clear","config","memory","init","diff","commit","pr","issue",
                   "export","plugin","branch","add-dir","files","hooks","release-notes"})
        if (name == n) return "Workspace & git";
    // Discovery & debugging
    for (auto n : {"agents","skills","teleport","debug-tool-call","mcp","context","tasks",
                   "doctor","ide","desktop"})
        if (name == n) return "Discovery & debugging";
    // Analysis & automation
    for (auto n : {"bughunter","ultraplan","review","security-review","advisor","insights"})
        if (name == n) return "Analysis & automation";
    // Appearance & input
    for (auto n : {"theme","vim","voice","color","effort","fast","brief","output-style",
                   "keybindings","stickers"})
        if (name == n) return "Appearance & input";
    // Communication & control
    for (auto n : {"copy","share","feedback","summary","tag","thinkback","plan","exit",
                   "upgrade","rewind"})
        if (name == n) return "Communication & control";
    return "Other";
}

// Detailed lines for a single command
std::vector<std::string> slash_command_detail_lines(const SlashCommandSpec& spec) {
    std::vector<std::string> lines;
    lines.push_back(std::format("/{}", spec.name));
    lines.push_back(std::format("  Summary          {}", spec.summary));
    lines.push_back(std::format("  Usage            {}", slash_command_usage(spec)));
    lines.push_back(std::format("  Category         {}", slash_command_category(spec.name)));

    if (!spec.aliases.empty()) {
        std::string alias_str;
        bool first = true;
        for (const char* a : spec.aliases) {
            if (!first) alias_str += ", ";
            alias_str += '/';
            alias_str += a;
            first = false;
        }
        lines.push_back(std::format("  Aliases          {}", alias_str));
    }
    if (spec.resume_supported)
        lines.push_back("  Resume           Supported with --resume SESSION.jsonl");
    return lines;
}

// Build error message with optional detail block
SlashCommandParseError command_error(std::string_view message,
                                     std::string_view command,
                                     std::string_view usage)
{
    std::string detail;
    if (const auto* spec = find_slash_command_spec(command_root_name(command))) {
        auto dlines = slash_command_detail_lines(*spec);
        detail = "\n\n";
        for (std::size_t i = 0; i < dlines.size(); ++i) {
            if (i > 0) detail += '\n';
            detail += dlines[i];
        }
    }
    return SlashCommandParseError(
        std::format("{}\n  Usage            {}{}", message, usage, detail));
}

SlashCommandParseError usage_error(std::string_view command, std::string_view argument_hint) {
    std::string usage_str = std::format("/{} {}", command, argument_hint);
    // Trim trailing whitespace (when argument_hint is empty)
    while (!usage_str.empty() && usage_str.back() == ' ') usage_str.pop_back();
    return command_error(
        std::format("Usage: {}", usage_str),
        command_root_name(command),
        usage_str);
}

// validate_no_args
Result<void, SlashCommandParseError>
validate_no_args(std::string_view command,
                 std::span<const std::string_view> args)
{
    if (args.empty()) return Result<void, SlashCommandParseError>::ok();
    return Result<void, SlashCommandParseError>::err(
        command_error(
            std::format("Unexpected arguments for /{}.", command),
            command,
            std::format("/{}", command)));
}

// optional_single_arg
Result<std::optional<std::string>, SlashCommandParseError>
optional_single_arg(std::string_view command,
                    std::span<const std::string_view> args,
                    std::string_view argument_hint)
{
    using R = Result<std::optional<std::string>, SlashCommandParseError>;
    if (args.empty())  return R::ok(std::nullopt);
    if (args.size() == 1) return R::ok(std::string(args[0]));
    return R::err(usage_error(command, argument_hint));
}

// require_remainder
Result<std::string, SlashCommandParseError>
require_remainder(std::string_view command,
                  std::optional<std::string> remainder,
                  std::string_view argument_hint)
{
    using R = Result<std::string, SlashCommandParseError>;
    if (remainder.has_value()) return R::ok(std::move(*remainder));
    return R::err(usage_error(command, argument_hint));
}

// parse_permissions_mode
Result<std::optional<std::string>, SlashCommandParseError>
parse_permissions_mode(std::span<const std::string_view> args)
{
    using R = Result<std::optional<std::string>, SlashCommandParseError>;
    auto res = optional_single_arg(
        "permissions", args, "[read-only|workspace-write|danger-full-access]");
    if (res.is_error()) return R::err(res.error());
    if (!res.value().has_value()) return R::ok(std::nullopt);
    const std::string& mode = *res.value();
    if (mode == "read-only" || mode == "workspace-write" || mode == "danger-full-access")
        return R::ok(mode);
    return R::err(command_error(
        std::format(
            "Unsupported /permissions mode '{}'. "
            "Use read-only, workspace-write, or danger-full-access.", mode),
        "permissions",
        "/permissions [read-only|workspace-write|danger-full-access]"));
}

// parse_clear_args
Result<bool, SlashCommandParseError>
parse_clear_args(std::span<const std::string_view> args)
{
    using R = Result<bool, SlashCommandParseError>;
    if (args.empty()) return R::ok(false);
    if (args.size() == 1 && args[0] == "--confirm") return R::ok(true);
    if (args.size() == 1)
        return R::err(command_error(
            std::format("Unsupported /clear argument '{}'. Use /clear or /clear --confirm.",
                        args[0]),
            "clear",
            "/clear [--confirm]"));
    return R::err(usage_error("clear", "[--confirm]"));
}

// parse_config_section
Result<std::optional<std::string>, SlashCommandParseError>
parse_config_section(std::span<const std::string_view> args)
{
    using R = Result<std::optional<std::string>, SlashCommandParseError>;
    auto res = optional_single_arg("config", args, "[env|hooks|model|plugins]");
    if (res.is_error()) return R::err(res.error());
    if (!res.value().has_value()) return R::ok(std::nullopt);
    const std::string& sec = *res.value();
    if (sec == "env" || sec == "hooks" || sec == "model" || sec == "plugins")
        return R::ok(sec);
    return R::err(command_error(
        std::format("Unsupported /config section '{}'. Use env, hooks, model, or plugins.", sec),
        "config",
        "/config [env|hooks|model|plugins]"));
}

// parse_session_command
Result<SlashCommand, SlashCommandParseError>
parse_session_command(std::span<const std::string_view> args)
{
    using R = Result<SlashCommand, SlashCommandParseError>;
    if (args.empty())
        return R::ok(slash_command::Session{});
    if (args[0] == "list") {
        if (args.size() == 1)
            return R::ok(slash_command::Session{std::string("list"), std::nullopt});
        return R::err(usage_error("session", "[list|switch <session-id>|fork [branch-name]]"));
    }
    if (args[0] == "switch") {
        if (args.size() == 1)
            return R::err(usage_error("session switch", "<session-id>"));
        if (args.size() == 2)
            return R::ok(slash_command::Session{std::string("switch"), std::string(args[1])});
        return R::err(command_error(
            "Unexpected arguments for /session switch.",
            "session",
            "/session switch <session-id>"));
    }
    if (args[0] == "fork") {
        if (args.size() == 1)
            return R::ok(slash_command::Session{std::string("fork"), std::nullopt});
        if (args.size() == 2)
            return R::ok(slash_command::Session{std::string("fork"), std::string(args[1])});
        return R::err(command_error(
            "Unexpected arguments for /session fork.",
            "session",
            "/session fork [branch-name]"));
    }
    return R::err(command_error(
        std::format(
            "Unknown /session action '{}'. Use list, switch <session-id>, or fork [branch-name].",
            args[0]),
        "session",
        "/session [list|switch <session-id>|fork [branch-name]]"));
}

// parse_mcp_command
Result<SlashCommand, SlashCommandParseError>
parse_mcp_command(std::span<const std::string_view> args)
{
    using R = Result<SlashCommand, SlashCommandParseError>;
    if (args.empty())
        return R::ok(slash_command::Mcp{});
    if (args[0] == "list") {
        if (args.size() == 1) return R::ok(slash_command::Mcp{std::string("list"), std::nullopt});
        return R::err(usage_error("mcp list", ""));
    }
    if (args[0] == "show") {
        if (args.size() == 1) return R::err(usage_error("mcp show", "<server>"));
        if (args.size() == 2)
            return R::ok(slash_command::Mcp{std::string("show"), std::string(args[1])});
        return R::err(command_error(
            "Unexpected arguments for /mcp show.", "mcp", "/mcp show <server>"));
    }
    if (args[0] == "help" || args[0] == "-h" || args[0] == "--help")
        return R::ok(slash_command::Mcp{std::string("help"), std::nullopt});
    return R::err(command_error(
        std::format("Unknown /mcp action '{}'. Use list, show <server>, or help.", args[0]),
        "mcp",
        "/mcp [list|show <server>|help]"));
}

// parse_plugin_command
Result<SlashCommand, SlashCommandParseError>
parse_plugin_command(std::span<const std::string_view> args)
{
    using R = Result<SlashCommand, SlashCommandParseError>;
    if (args.empty())
        return R::ok(slash_command::Plugins{});

    auto make_plugin = [](std::string action, std::optional<std::string> target) -> SlashCommand {
        return slash_command::Plugins{std::move(action), std::move(target)};
    };

    if (args[0] == "list") {
        if (args.size() == 1) return R::ok(make_plugin("list", std::nullopt));
        return R::err(usage_error("plugin list", ""));
    }
    if (args[0] == "install") {
        if (args.size() == 1) return R::err(usage_error("plugin install", "<path>"));
        // All remaining args join with spaces (matches Rust: target.join(" "))
        std::string target;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (i > 1) target += ' ';
            target += args[i];
        }
        return R::ok(make_plugin("install", target));
    }
    if (args[0] == "enable") {
        if (args.size() == 1) return R::err(usage_error("plugin enable", "<name>"));
        if (args.size() == 2) return R::ok(make_plugin("enable", std::string(args[1])));
        return R::err(command_error(
            "Unexpected arguments for /plugin enable.", "plugin", "/plugin enable <name>"));
    }
    if (args[0] == "disable") {
        if (args.size() == 1) return R::err(usage_error("plugin disable", "<name>"));
        if (args.size() == 2) return R::ok(make_plugin("disable", std::string(args[1])));
        return R::err(command_error(
            "Unexpected arguments for /plugin disable.", "plugin", "/plugin disable <name>"));
    }
    if (args[0] == "uninstall") {
        if (args.size() == 1) return R::err(usage_error("plugin uninstall", "<id>"));
        if (args.size() == 2) return R::ok(make_plugin("uninstall", std::string(args[1])));
        return R::err(command_error(
            "Unexpected arguments for /plugin uninstall.", "plugin", "/plugin uninstall <id>"));
    }
    if (args[0] == "update") {
        if (args.size() == 1) return R::err(usage_error("plugin update", "<id>"));
        if (args.size() == 2) return R::ok(make_plugin("update", std::string(args[1])));
        return R::err(command_error(
            "Unexpected arguments for /plugin update.", "plugin", "/plugin update <id>"));
    }
    return R::err(command_error(
        std::format(
            "Unknown /plugin action '{}'. Use list, install <path>, enable <name>, "
            "disable <name>, uninstall <id>, or update <id>.", args[0]),
        "plugin",
        "/plugin [list|install <path>|enable <name>|disable <name>|uninstall <id>|update <id>]"));
}

// normalize_optional_args: return a trimmed non-empty string_view or nullopt
std::optional<std::string_view> normalize_optional_args(std::optional<std::string_view> args) {
    if (!args.has_value()) return std::nullopt;
    auto t = trim_sv(*args);
    if (t.empty()) return std::nullopt;
    return t;
}

std::optional<std::string_view> normalize_optional_args(std::string_view args) {
    auto t = trim_sv(args);
    if (t.empty()) return std::nullopt;
    return t;
}

// parse_list_or_help_args
Result<std::optional<std::string>, SlashCommandParseError>
parse_list_or_help_args(std::string_view command, std::optional<std::string> args)
{
    using R = Result<std::optional<std::string>, SlashCommandParseError>;
    auto norm = normalize_optional_args(args ? std::optional<std::string_view>(*args)
                                             : std::optional<std::string_view>{});
    if (!norm.has_value() || *norm == "list" || *norm == "help" ||
        *norm == "-h" || *norm == "--help")
        return R::ok(std::move(args));
    return R::err(command_error(
        std::format("Unexpected arguments for /{}: {}. "
                    "Use /{}, /{} list, or /{} help.",
                    command, *norm, command, command, command),
        command,
        std::format("/{} [list|help]", command)));
}

// parse_skills_args
Result<std::optional<std::string>, SlashCommandParseError>
parse_skills_args(std::optional<std::string_view> raw_args)
{
    using R = Result<std::optional<std::string>, SlashCommandParseError>;
    auto norm = normalize_optional_args(raw_args);
    if (!norm.has_value()) return R::ok(std::nullopt);
    std::string_view args = *norm;
    if (args == "list" || args == "help" || args == "-h" || args == "--help")
        return R::ok(std::string(args));
    if (args == "install")
        return R::err(command_error("Usage: /skills install <path>",
                                    "skills", "/skills install <path>"));
    if (args.starts_with("install")) {
        auto target = trim_sv(args.substr(std::string_view("install").size()));
        if (!target.empty())
            return R::ok(std::format("install {}", target));
    }
    // Accept any other args -- the dispatch layer uses classify_skills_slash_command
    // to determine whether this is a direct skill invocation.
    return R::ok(std::string(args));
}

// ---------------------------------------------------------------------------
// Levenshtein distance (direct port of the Rust implementation)
// ---------------------------------------------------------------------------
std::size_t levenshtein_distance(std::string_view left, std::string_view right) {
    if (left == right)   return 0;
    if (left.empty())    return right.size();
    if (right.empty())   return left.size();

    std::vector<std::size_t> previous(right.size() + 1);
    std::iota(previous.begin(), previous.end(), 0u);
    std::vector<std::size_t> current(right.size() + 1, 0u);

    for (std::size_t li = 0; li < left.size(); ++li) {
        current[0] = li + 1;
        for (std::size_t ri = 0; ri < right.size(); ++ri) {
            std::size_t sub_cost = (left[li] != right[ri]) ? 1u : 0u;
            current[ri + 1] = std::min({current[ri] + 1,
                                         previous[ri + 1] + 1,
                                         previous[ri] + sub_cost});
        }
        std::swap(previous, current);
    }
    return previous[right.size()];
}

// Format a single help line for the /help listing
std::string format_slash_command_help_line(const SlashCommandSpec& spec) {
    std::string name = slash_command_usage(spec);
    std::string alias_suffix;
    if (!spec.aliases.empty()) {
        alias_suffix = " (aliases: ";
        bool first = true;
        for (const char* a : spec.aliases) {
            if (!first) alias_suffix += ", ";
            alias_suffix += '/';
            alias_suffix += a;
            first = false;
        }
        alias_suffix += ')';
    }
    const char* resume = spec.resume_supported ? " [resume]" : "";
    // Left-pad name to 66 chars, then summary + aliases + resume
    std::string padded_name = name;
    if (padded_name.size() < 66)
        padded_name += std::string(66 - padded_name.size(), ' ');
    return std::format("  {} {}{}{}", padded_name, spec.summary, alias_suffix, resume);
}

// ---- Definition sources (mirrors Rust DefinitionSource) -------------------
enum class DefinitionSource : std::uint8_t {
    ProjectClaw,
    ProjectOmc,
    ProjectAgents,
    ProjectCodex,
    ProjectClaude,
    UserClawHome,
    UserCodexHome,
    UserClaw,
    UserOmc,
    UserCodex,
    UserClaude,
};

const char* definition_source_label(DefinitionSource s) {
    switch (s) {
    case DefinitionSource::ProjectClaw:    return "Project (.claw)";
    case DefinitionSource::ProjectOmc:     return "Project (.omc)";
    case DefinitionSource::ProjectAgents:  return "Project (.agents)";
    case DefinitionSource::ProjectCodex:   return "Project (.codex)";
    case DefinitionSource::ProjectClaude:  return "Project (.claude)";
    case DefinitionSource::UserClawHome:   return "User ($CLAW_CONFIG_HOME)";
    case DefinitionSource::UserCodexHome:  return "User ($CODEX_HOME)";
    case DefinitionSource::UserClaw:       return "User (~/.claw)";
    case DefinitionSource::UserOmc:        return "User (~/.omc)";
    case DefinitionSource::UserCodex:      return "User (~/.codex)";
    case DefinitionSource::UserClaude:     return "User (~/.claude)";
    }
    return "Unknown";
}

// ---- AgentSummary ---------------------------------------------------------
struct AgentSummary {
    std::string                         name;
    std::optional<std::string>          description;
    std::optional<std::string>          model;
    std::optional<std::string>          reasoning_effort;
    DefinitionSource                    source;
    std::optional<DefinitionSource>     shadowed_by;
};

// ---- SkillOrigin ----------------------------------------------------------
enum class SkillOrigin : std::uint8_t { SkillsDir, LegacyCommandsDir };

const char* skill_origin_detail_label(SkillOrigin o) {
    return o == SkillOrigin::LegacyCommandsDir ? "legacy /commands" : nullptr;
}

// ---- SkillSummary ---------------------------------------------------------
struct SkillSummary {
    std::string                         name;
    std::optional<std::string>          description;
    DefinitionSource                    source;
    std::optional<DefinitionSource>     shadowed_by;
    SkillOrigin                         origin;
};

// ---- SkillRoot ------------------------------------------------------------
struct SkillRoot {
    DefinitionSource          source;
    std::filesystem::path     path;
    SkillOrigin               origin;
};

// ---- InstalledSkill -------------------------------------------------------
struct InstalledSkill {
    std::string               invocation_name;
    std::optional<std::string> display_name;
    std::filesystem::path     source;
    std::filesystem::path     registry_root;
    std::filesystem::path     installed_path;
};

// ---- SkillInstallSource ---------------------------------------------------
struct SkillInstallSourceDir      { std::filesystem::path root; std::filesystem::path prompt_path; };
struct SkillInstallSourceMarkdown { std::filesystem::path path; };
using SkillInstallSource = std::variant<SkillInstallSourceDir, SkillInstallSourceMarkdown>;

std::filesystem::path skill_install_source_prompt_path(const SkillInstallSource& src) {
    return std::visit([](const auto& s) -> std::filesystem::path {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, SkillInstallSourceDir>)      return s.prompt_path;
        if constexpr (std::is_same_v<T, SkillInstallSourceMarkdown>) return s.path;
    }, src);
}

std::optional<std::string> skill_install_source_fallback_name(const SkillInstallSource& src) {
    return std::visit([](const auto& s) -> std::optional<std::string> {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, SkillInstallSourceDir>) {
            auto fn = s.root.filename();
            return fn.empty() ? std::nullopt : std::optional<std::string>(fn.string());
        }
        if constexpr (std::is_same_v<T, SkillInstallSourceMarkdown>) {
            auto stem = s.path.stem();
            return stem.empty() ? std::nullopt : std::optional<std::string>(stem.string());
        }
    }, src);
}

std::filesystem::path skill_install_source_report_path(const SkillInstallSource& src) {
    return std::visit([](const auto& s) -> std::filesystem::path {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, SkillInstallSourceDir>)      return s.root;
        if constexpr (std::is_same_v<T, SkillInstallSourceMarkdown>) return s.path;
    }, src);
}

// ---- push_unique_root / push_unique_skill_root ----------------------------
void push_unique_root(std::vector<std::pair<DefinitionSource, std::filesystem::path>>& roots,
                       DefinitionSource source,
                       std::filesystem::path path)
{
    if (std::filesystem::is_directory(path) &&
        std::none_of(roots.begin(), roots.end(),
                     [&](const auto& e){ return e.second == path; }))
        roots.emplace_back(source, std::move(path));
}

void push_unique_skill_root(std::vector<SkillRoot>& roots,
                             DefinitionSource source,
                             std::filesystem::path path,
                             SkillOrigin origin)
{
    if (std::filesystem::is_directory(path) &&
        std::none_of(roots.begin(), roots.end(),
                     [&](const auto& e){ return e.path == path; }))
        roots.push_back(SkillRoot{source, std::move(path), origin});
}

// ---- discover_definition_roots (agents) -----------------------------------
std::vector<std::pair<DefinitionSource, std::filesystem::path>>
discover_definition_roots(const std::filesystem::path& cwd, std::string_view leaf)
{
    std::vector<std::pair<DefinitionSource, std::filesystem::path>> roots;
    for (auto p = cwd; ; p = p.parent_path()) {
        push_unique_root(roots, DefinitionSource::ProjectClaw,    p / ".claw"   / leaf);
        push_unique_root(roots, DefinitionSource::ProjectOmc,     p / ".omc"    / leaf);
        push_unique_root(roots, DefinitionSource::ProjectAgents,  p / ".agents" / leaf);
        push_unique_root(roots, DefinitionSource::ProjectCodex,   p / ".codex"  / leaf);
        push_unique_root(roots, DefinitionSource::ProjectClaude,  p / ".claude" / leaf);
        if (p == p.parent_path()) break;
    }
    if (const char* claw_home = std::getenv("CLAW_CONFIG_HOME"))
        push_unique_root(roots, DefinitionSource::UserClawHome,
                         std::filesystem::path(claw_home) / leaf);
    if (const char* ch = std::getenv("CODEX_HOME"))
        push_unique_root(roots, DefinitionSource::UserCodexHome,
                         std::filesystem::path(ch) / leaf);
    if (const char* home = std::getenv("HOME")) {
        auto h = std::filesystem::path(home);
        push_unique_root(roots, DefinitionSource::UserClaw,    h / ".claw"   / leaf);
        push_unique_root(roots, DefinitionSource::UserOmc,     h / ".omc"    / leaf);
        push_unique_root(roots, DefinitionSource::UserCodex,   h / ".codex"  / leaf);
        push_unique_root(roots, DefinitionSource::UserClaude,  h / ".claude" / leaf);
    }
    return roots;
}

// ---- discover_skill_roots -------------------------------------------------
std::vector<SkillRoot> discover_skill_roots(const std::filesystem::path& cwd) {
    std::vector<SkillRoot> roots;
    for (auto p = cwd; ; p = p.parent_path()) {
        push_unique_skill_root(roots, DefinitionSource::ProjectClaw,
                               p / ".claw"   / "skills",   SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::ProjectOmc,
                               p / ".omc"    / "skills",   SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::ProjectAgents,
                               p / ".agents" / "skills",   SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::ProjectCodex,
                               p / ".codex"  / "skills",   SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::ProjectClaude,
                               p / ".claude" / "skills",   SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::ProjectClaw,
                               p / ".claw"   / "commands", SkillOrigin::LegacyCommandsDir);
        push_unique_skill_root(roots, DefinitionSource::ProjectOmc,
                               p / ".omc"    / "commands", SkillOrigin::LegacyCommandsDir);
        push_unique_skill_root(roots, DefinitionSource::ProjectAgents,
                               p / ".agents" / "commands", SkillOrigin::LegacyCommandsDir);
        push_unique_skill_root(roots, DefinitionSource::ProjectCodex,
                               p / ".codex"  / "commands", SkillOrigin::LegacyCommandsDir);
        push_unique_skill_root(roots, DefinitionSource::ProjectClaude,
                               p / ".claude" / "commands", SkillOrigin::LegacyCommandsDir);
        if (p == p.parent_path()) break;
    }
    if (const char* claw_home = std::getenv("CLAW_CONFIG_HOME")) {
        auto ch = std::filesystem::path(claw_home);
        push_unique_skill_root(roots, DefinitionSource::UserClawHome,
                               ch / "skills",   SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::UserClawHome,
                               ch / "commands", SkillOrigin::LegacyCommandsDir);
    }
    if (const char* ch = std::getenv("CODEX_HOME")) {
        auto codex = std::filesystem::path(ch);
        push_unique_skill_root(roots, DefinitionSource::UserCodexHome,
                               codex / "skills",   SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::UserCodexHome,
                               codex / "commands", SkillOrigin::LegacyCommandsDir);
    }
    if (const char* home = std::getenv("HOME")) {
        auto h = std::filesystem::path(home);
        push_unique_skill_root(roots, DefinitionSource::UserClaw,
                               h / ".claw"   / "skills",   SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::UserOmc,
                               h / ".omc"    / "skills",   SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::UserOmc,
                               h / ".omc"    / "commands", SkillOrigin::LegacyCommandsDir);
        push_unique_skill_root(roots, DefinitionSource::UserCodex,
                               h / ".codex"  / "skills",   SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::UserCodex,
                               h / ".codex"  / "commands", SkillOrigin::LegacyCommandsDir);
        push_unique_skill_root(roots, DefinitionSource::UserClaude,
                               h / ".claude" / "skills",   SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::UserClaude,
                               h / ".claude" / "skills" / "omc-learned", SkillOrigin::SkillsDir);
        push_unique_skill_root(roots, DefinitionSource::UserClaude,
                               h / ".claude" / "commands", SkillOrigin::LegacyCommandsDir);
    }
    return roots;
}

// ---- parse_toml_string ---------------------------------------------------
// Minimal TOML string parser – looks for `key = "value"` lines.
std::optional<std::string> parse_toml_string(std::string_view contents, std::string_view key) {
    std::string prefix = std::string(key) + " =";
    std::istringstream ss{std::string(contents)};
    std::string line;
    while (std::getline(ss, line)) {
        std::string_view lv = trim_sv(line);
        if (!lv.empty() && lv.front() == '#') continue;
        if (!lv.starts_with(prefix)) continue;
        auto value = trim_sv(lv.substr(prefix.size()));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            auto inner = value.substr(1, value.size() - 2);
            if (!inner.empty()) return std::string(inner);
        }
    }
    return std::nullopt;
}

// ---- parse_skill_frontmatter / unquote_frontmatter_value -----------------
std::string unquote_frontmatter_value(std::string_view value) {
    auto t = trim_sv(value);
    if (t.size() >= 2) {
        if ((t.front() == '"'  && t.back() == '"')  ||
            (t.front() == '\'' && t.back() == '\''))
            return std::string(trim_sv(t.substr(1, t.size() - 2)));
    }
    return std::string(t);
}

std::pair<std::optional<std::string>, std::optional<std::string>>
parse_skill_frontmatter(std::string_view contents)
{
    std::istringstream ss{std::string(contents)};
    std::string line;
    // First line must be "---"
    if (!std::getline(ss, line) || trim_sv(line) != "---")
        return {std::nullopt, std::nullopt};

    std::optional<std::string> name, description;
    while (std::getline(ss, line)) {
        auto t = trim_sv(line);
        if (t == "---") break;
        if (t.starts_with("name:")) {
            auto v = unquote_frontmatter_value(t.substr(5));
            if (!v.empty()) name = std::move(v);
        } else if (t.starts_with("description:")) {
            auto v = unquote_frontmatter_value(t.substr(12));
            if (!v.empty()) description = std::move(v);
        }
    }
    return {std::move(name), std::move(description)};
}

// ---- load_agents_from_roots -----------------------------------------------
std::vector<AgentSummary>
load_agents_from_roots(
    const std::vector<std::pair<DefinitionSource, std::filesystem::path>>& roots)
{
    std::vector<AgentSummary> agents;
    std::map<std::string, DefinitionSource> active_sources;

    for (const auto& [source, root] : roots) {
        std::vector<AgentSummary> root_agents;
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.path().extension() != ".toml") continue;
            std::ifstream f(entry.path());
            if (!f.is_open()) continue;
            std::string contents{std::istreambuf_iterator<char>(f), {}};
            std::string fallback_name = entry.path().stem().string();
            root_agents.push_back(AgentSummary{
                parse_toml_string(contents, "name").value_or(std::move(fallback_name)),
                parse_toml_string(contents, "description"),
                parse_toml_string(contents, "model"),
                parse_toml_string(contents, "model_reasoning_effort"),
                source,
                std::nullopt,
            });
        }
        std::sort(root_agents.begin(), root_agents.end(),
                  [](const auto& a, const auto& b){ return a.name < b.name; });

        for (auto& agent : root_agents) {
            std::string key;
            for (char c : agent.name)
                key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            auto it = active_sources.find(key);
            if (it != active_sources.end())
                agent.shadowed_by = it->second;
            else
                active_sources.emplace(key, agent.source);
            agents.push_back(std::move(agent));
        }
    }
    return agents;
}

// ---- load_skills_from_roots -----------------------------------------------
std::vector<SkillSummary>
load_skills_from_roots(const std::vector<SkillRoot>& roots)
{
    std::vector<SkillSummary> skills;
    std::map<std::string, DefinitionSource> active_sources;

    for (const auto& root : roots) {
        std::vector<SkillSummary> root_skills;
        for (const auto& entry : std::filesystem::directory_iterator(root.path)) {
            if (root.origin == SkillOrigin::SkillsDir) {
                if (!entry.is_directory()) continue;
                auto skill_path = entry.path() / "SKILL.md";
                if (!std::filesystem::is_regular_file(skill_path)) continue;
                std::ifstream f(skill_path);
                if (!f.is_open()) continue;
                std::string contents{std::istreambuf_iterator<char>(f), {}};
                auto [name, desc] = parse_skill_frontmatter(contents);
                root_skills.push_back(SkillSummary{
                    name.value_or(entry.path().filename().string()),
                    std::move(desc),
                    root.source,
                    std::nullopt,
                    root.origin,
                });
            } else {
                // LegacyCommandsDir
                std::filesystem::path md_path;
                if (entry.is_directory()) {
                    auto sp = entry.path() / "SKILL.md";
                    if (!std::filesystem::is_regular_file(sp)) continue;
                    md_path = sp;
                } else {
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c){ return std::tolower(c); });
                    if (ext != ".md") continue;
                    md_path = entry.path();
                }
                std::ifstream f(md_path);
                if (!f.is_open()) continue;
                std::string contents{std::istreambuf_iterator<char>(f), {}};
                std::string fallback = md_path.stem().string();
                auto [name, desc] = parse_skill_frontmatter(contents);
                root_skills.push_back(SkillSummary{
                    name.value_or(std::move(fallback)),
                    std::move(desc),
                    root.source,
                    std::nullopt,
                    root.origin,
                });
            }
        }
        std::sort(root_skills.begin(), root_skills.end(),
                  [](const auto& a, const auto& b){ return a.name < b.name; });

        for (auto& skill : root_skills) {
            std::string key;
            for (char c : skill.name)
                key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            auto it = active_sources.find(key);
            if (it != active_sources.end())
                skill.shadowed_by = it->second;
            else
                active_sources.emplace(key, skill.source);
            skills.push_back(std::move(skill));
        }
    }
    return skills;
}

// ---- agent_detail ---------------------------------------------------------
// Mirrors Rust's agent_detail() helper: joins name, description, model,
// reasoning_effort with " · " separators.
std::string agent_detail(const AgentSummary& agent) {
    std::vector<std::string> parts;
    parts.push_back(agent.name);
    if (agent.description) parts.push_back(*agent.description);
    if (agent.model)       parts.push_back(*agent.model);
    if (agent.reasoning_effort) parts.push_back(*agent.reasoning_effort);
    std::string detail;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) detail += " \xC2\xB7 "; // UTF-8 middle dot U+00B7
        detail += parts[i];
    }
    return detail;
}

// ---- render helpers -------------------------------------------------------

std::string render_agents_report(const std::vector<AgentSummary>& agents) {
    if (agents.empty()) return "No agents found.";
    std::size_t total_active = std::ranges::count_if(agents,
                               [](const auto& a){ return !a.shadowed_by.has_value(); });
    std::vector<std::string> lines = {
        "Agents",
        std::format("  {} active agents", total_active),
        "",
    };

    for (auto src : {DefinitionSource::ProjectClaw, DefinitionSource::ProjectOmc,
                     DefinitionSource::ProjectAgents, DefinitionSource::ProjectCodex,
                     DefinitionSource::ProjectClaude, DefinitionSource::UserClawHome,
                     DefinitionSource::UserCodexHome, DefinitionSource::UserClaw,
                     DefinitionSource::UserOmc, DefinitionSource::UserCodex,
                     DefinitionSource::UserClaude})
    {
        std::vector<const AgentSummary*> group;
        for (const auto& a : agents)
            if (a.source == src) group.push_back(&a);
        if (group.empty()) continue;
        lines.push_back(std::format("{}:", definition_source_label(src)));
        for (const auto* a : group) {
            std::string detail = agent_detail(*a);
            if (a->shadowed_by)
                lines.push_back(std::format("  (shadowed by {}) {}",
                                            definition_source_label(*a->shadowed_by), detail));
            else
                lines.push_back(std::format("  {}", detail));
        }
        lines.push_back("");
    }

    // Trim trailing empty lines (mirrors Rust's .trim_end())
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

std::string render_skills_report(const std::vector<SkillSummary>& skills) {
    if (skills.empty()) return "No skills found.";
    std::size_t total_active = std::ranges::count_if(skills,
                               [](const auto& s){ return !s.shadowed_by.has_value(); });
    std::vector<std::string> lines = {
        "Skills",
        std::format("  {} available skills", total_active),
        "",
    };

    for (auto src : {DefinitionSource::ProjectClaw, DefinitionSource::ProjectOmc,
                     DefinitionSource::ProjectAgents, DefinitionSource::ProjectCodex,
                     DefinitionSource::ProjectClaude, DefinitionSource::UserClawHome,
                     DefinitionSource::UserCodexHome, DefinitionSource::UserClaw,
                     DefinitionSource::UserOmc, DefinitionSource::UserCodex,
                     DefinitionSource::UserClaude})
    {
        std::vector<const SkillSummary*> group;
        for (const auto& s : skills)
            if (s.source == src) group.push_back(&s);
        if (group.empty()) continue;
        lines.push_back(std::format("{}:", definition_source_label(src)));
        for (const auto* skill : group) {
            std::vector<std::string> parts = {skill->name};
            if (skill->description) parts.push_back(*skill->description);
            if (const char* lbl = skill_origin_detail_label(skill->origin))
                parts.push_back(lbl);
            std::string detail;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) detail += " \xC2\xB7 "; // UTF-8 middle dot
                detail += parts[i];
            }
            if (skill->shadowed_by)
                lines.push_back(std::format("  (shadowed by {}) {}",
                                            definition_source_label(*skill->shadowed_by), detail));
            else
                lines.push_back(std::format("  {}", detail));
        }
        lines.push_back("");
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

std::string render_skill_install_report(const InstalledSkill& skill) {
    std::vector<std::string> lines = {
        "Skills",
        std::format("  Result           installed {}", skill.invocation_name),
        std::format("  Invoke as        ${}", skill.invocation_name),
    };
    if (skill.display_name)
        lines.push_back(std::format("  Display name     {}", *skill.display_name));
    lines.push_back(std::format("  Source           {}", skill.source.string()));
    lines.push_back(std::format("  Registry         {}", skill.registry_root.string()));
    lines.push_back(std::format("  Installed path   {}", skill.installed_path.string()));
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

std::string render_agents_usage(std::optional<std::string_view> unexpected) {
    std::vector<std::string> lines = {
        "Agents",
        "  Usage            /agents [list|help]",
        "  Direct CLI       claw agents",
        "  Sources          .claw/agents, ~/.claw/agents, $CLAW_CONFIG_HOME/agents",
    };
    if (unexpected) lines.push_back(std::format("  Unexpected       {}", *unexpected));
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

std::string render_skills_usage(std::optional<std::string_view> unexpected) {
    std::vector<std::string> lines = {
        "Skills",
        "  Usage            /skills [list|install <path>|help|<skill> [args]]",
        "  Alias            /skill",
        "  Direct CLI       claw skills [list|install <path>|help|<skill> [args]]",
        "  Invoke           /skills help overview -> $help overview",
        "  Install root     $CLAW_CONFIG_HOME/skills or ~/.claw/skills",
        "  Sources          .claw/skills, .omc/skills, .agents/skills, .codex/skills, .claude/skills, ~/.claw/skills, ~/.omc/skills, ~/.claude/skills/omc-learned, ~/.codex/skills, ~/.claude/skills, legacy /commands",
    };
    if (unexpected) lines.push_back(std::format("  Unexpected       {}", *unexpected));
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

std::string render_mcp_usage(std::optional<std::string_view> unexpected) {
    std::vector<std::string> lines = {
        "MCP",
        "  Usage            /mcp [list|show <server>|help]",
        "  Direct CLI       claw mcp [list|show <server>|help]",
        "  Sources          .claw/settings.json, .claw/settings.local.json",
    };
    if (unexpected) lines.push_back(std::format("  Unexpected       {}", *unexpected));
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

// ---- MCP rendering helpers ------------------------------------------------
const char* config_source_label(runtime::ConfigSource s) {
    switch (s) {
    case runtime::ConfigSource::User:    return "user";
    case runtime::ConfigSource::Project: return "project";
    case runtime::ConfigSource::Local:   return "local";
    }
    return "unknown";
}

const char* mcp_transport_label(const runtime::TaggedMcpServerConfig& cfg) {
    switch (cfg.kind) {
    case runtime::McpTransportKind::Stdio:        return "stdio";
    case runtime::McpTransportKind::Sse:          return "sse";
    case runtime::McpTransportKind::Http:         return "http";
    case runtime::McpTransportKind::Ws:           return "ws";
    case runtime::McpTransportKind::Sdk:          return "sdk";
    case runtime::McpTransportKind::ManagedProxy: return "managed-proxy";
    }
    return "unknown";
}

std::string format_optional_list(const std::vector<std::string>& values) {
    if (values.empty()) return "<none>";
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out += ' ';
        out += values[i];
    }
    return out;
}

std::string format_optional_keys(std::vector<std::string> keys) {
    if (keys.empty()) return "<none>";
    std::sort(keys.begin(), keys.end());
    std::string out;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) out += ", ";
        out += keys[i];
    }
    return out;
}

std::string format_mcp_oauth(const runtime::McpOAuthConfig* oauth) {
    if (!oauth) return "<none>";
    std::vector<std::string> parts;
    if (oauth->client_id) parts.push_back(std::format("client_id={}", *oauth->client_id));
    if (oauth->callback_port)
        parts.push_back(std::format("callback_port={}", *oauth->callback_port));
    if (oauth->auth_server_metadata_url)
        parts.push_back(std::format("metadata_url={}", *oauth->auth_server_metadata_url));
    if (oauth->xaa) parts.push_back(std::format("xaa={}", *oauth->xaa));
    if (parts.empty()) return "enabled";
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += ", ";
        out += parts[i];
    }
    return out;
}

std::string mcp_server_summary(const runtime::TaggedMcpServerConfig& cfg) {
    switch (cfg.kind) {
    case runtime::McpTransportKind::Stdio:
        if (cfg.stdio) {
            if (cfg.stdio->args.empty()) return cfg.stdio->command;
            return cfg.stdio->command + ' ' + format_optional_list(cfg.stdio->args);
        }
        break;
    case runtime::McpTransportKind::Sse:
    case runtime::McpTransportKind::Http:
        if (cfg.remote) return cfg.remote->url;
        break;
    case runtime::McpTransportKind::Ws:
        if (cfg.ws) return cfg.ws->url;
        break;
    case runtime::McpTransportKind::Sdk:
        if (cfg.sdk) return cfg.sdk->name;
        break;
    case runtime::McpTransportKind::ManagedProxy:
        if (cfg.managed_proxy)
            return std::format("{} ({})", cfg.managed_proxy->id, cfg.managed_proxy->url);
        break;
    }
    return "<unknown>";
}

std::string render_mcp_summary_report(
    const std::filesystem::path& cwd,
    const std::map<std::string, runtime::ScopedMcpServerConfig>& servers)
{
    std::vector<std::string> lines = {
        "MCP",
        std::format("  Working directory {}", cwd.string()),
        std::format("  Configured servers {}", servers.size()),
    };
    if (servers.empty()) {
        lines.push_back("  No MCP servers configured.");
        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) out += '\n';
            out += lines[i];
        }
        return out;
    }
    lines.push_back("");
    for (const auto& [name, server] : servers) {
        lines.push_back(std::format(
            "  {:<16} {:<13} {:<7} {}",
            name,
            mcp_transport_label(server.config),
            config_source_label(server.scope),
            mcp_server_summary(server.config)));
    }
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

std::string render_mcp_server_report(
    const std::filesystem::path& cwd,
    std::string_view server_name,
    const runtime::ScopedMcpServerConfig* server)
{
    if (!server)
        return std::format(
            "MCP\n  Working directory {}\n  Result            server `{}` is not configured",
            cwd.string(), server_name);

    std::vector<std::string> lines = {
        "MCP",
        std::format("  Working directory {}", cwd.string()),
        std::format("  Name              {}", server_name),
        std::format("  Scope             {}", config_source_label(server->scope)),
        std::format("  Transport         {}", mcp_transport_label(server->config)),
    };

    switch (server->config.kind) {
    case runtime::McpTransportKind::Stdio:
        if (server->config.stdio) {
            auto& c = *server->config.stdio;
            std::vector<std::string> env_keys;
            for (const auto& [k, _] : c.env) env_keys.push_back(k);
            lines.push_back(std::format("  Command           {}", c.command));
            lines.push_back(std::format("  Args              {}", format_optional_list(c.args)));
            lines.push_back(std::format("  Env keys          {}", format_optional_keys(env_keys)));
            lines.push_back(std::format("  Tool timeout      {}",
                c.tool_call_timeout_ms
                    ? std::format("{} ms", *c.tool_call_timeout_ms)
                    : std::string("<default>")));
        }
        break;
    case runtime::McpTransportKind::Sse:
    case runtime::McpTransportKind::Http:
        if (server->config.remote) {
            auto& c = *server->config.remote;
            std::vector<std::string> hkeys;
            for (const auto& [k, _] : c.headers) hkeys.push_back(k);
            lines.push_back(std::format("  URL               {}", c.url));
            lines.push_back(std::format("  Header keys       {}", format_optional_keys(hkeys)));
            lines.push_back(std::format("  Header helper     {}",
                c.headers_helper.value_or("<none>")));
            lines.push_back(std::format("  OAuth             {}",
                format_mcp_oauth(c.oauth ? &*c.oauth : nullptr)));
        }
        break;
    case runtime::McpTransportKind::Ws:
        if (server->config.ws) {
            auto& c = *server->config.ws;
            std::vector<std::string> hkeys;
            for (const auto& [k, _] : c.headers) hkeys.push_back(k);
            lines.push_back(std::format("  URL               {}", c.url));
            lines.push_back(std::format("  Header keys       {}", format_optional_keys(hkeys)));
            lines.push_back(std::format("  Header helper     {}",
                c.headers_helper.value_or("<none>")));
        }
        break;
    case runtime::McpTransportKind::Sdk:
        if (server->config.sdk)
            lines.push_back(std::format("  SDK name          {}", server->config.sdk->name));
        break;
    case runtime::McpTransportKind::ManagedProxy:
        if (server->config.managed_proxy) {
            lines.push_back(std::format("  URL               {}", server->config.managed_proxy->url));
            lines.push_back(std::format("  Proxy id          {}", server->config.managed_proxy->id));
        }
        break;
    }

    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

// ---- render_mcp_report_for ------------------------------------------------
Result<std::string, runtime::ConfigError>
render_mcp_report_for(const runtime::ConfigLoader& loader,
                       const std::filesystem::path& cwd,
                       std::optional<std::string_view> args)
{
    using R = Result<std::string, runtime::ConfigError>;
    auto norm = normalize_optional_args(args);

    if (!norm.has_value() || *norm == "list") {
        try {
            auto cfg = loader.load();
            return R::ok(render_mcp_summary_report(cwd, cfg.mcp().servers()));
        } catch (const runtime::ConfigError& e) {
            return R::err(e);
        }
    }
    if (*norm == "-h" || *norm == "--help" || *norm == "help")
        return R::ok(render_mcp_usage(std::nullopt));
    if (*norm == "show")
        return R::ok(render_mcp_usage(std::string_view("show")));

    if (norm->starts_with("show")) {
        auto parts = split_whitespace(*norm);
        if (parts.size() < 2) return R::ok(render_mcp_usage(std::string_view("show")));
        if (parts.size() > 2) return R::ok(render_mcp_usage(*norm));
        auto server_name = parts[1];
        try {
            auto cfg = loader.load();
            return R::ok(render_mcp_server_report(cwd, server_name, cfg.mcp().get(server_name)));
        } catch (const runtime::ConfigError& e) {
            return R::err(e);
        }
    }
    return R::ok(render_mcp_usage(*norm));
}

// ---- copy_directory_contents ----------------------------------------------
void copy_directory_contents(const std::filesystem::path& source,
                              const std::filesystem::path& destination)
{
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
        auto dest_path = destination / entry.path().filename();
        if (entry.is_directory()) {
            std::filesystem::create_directories(dest_path);
            copy_directory_contents(entry.path(), dest_path);
        } else {
            std::filesystem::copy_file(entry.path(), dest_path,
                std::filesystem::copy_options::overwrite_existing);
        }
    }
}

// ---- sanitize_skill_invocation_name --------------------------------------
std::optional<std::string> sanitize_skill_invocation_name(std::string_view candidate) {
    auto trimmed = trim_sv(candidate);
    while (!trimmed.empty() && (trimmed.front() == '/' || trimmed.front() == '$'))
        trimmed.remove_prefix(1);
    if (trimmed.empty()) return std::nullopt;

    std::string sanitized;
    bool last_was_separator = false;
    for (unsigned char ch : trimmed) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            sanitized.push_back(static_cast<char>(std::tolower(ch)));
            last_was_separator = false;
        } else if ((std::isspace(ch) || ch == '/' || ch == '\\') &&
                   !last_was_separator && !sanitized.empty()) {
            sanitized.push_back('-');
            last_was_separator = true;
        }
    }
    while (!sanitized.empty() &&
           (sanitized.front() == '-' || sanitized.front() == '_' || sanitized.front() == '.'))
        sanitized.erase(sanitized.begin());
    while (!sanitized.empty() &&
           (sanitized.back()  == '-' || sanitized.back()  == '_' || sanitized.back()  == '.'))
        sanitized.pop_back();
    if (sanitized.empty()) return std::nullopt;
    return sanitized;
}

// ---- default_skill_install_root ------------------------------------------
std::filesystem::path default_skill_install_root() {
    if (const char* ch = std::getenv("CLAW_CONFIG_HOME"))
        return std::filesystem::path(ch) / "skills";
    if (const char* codex = std::getenv("CODEX_HOME"))
        return std::filesystem::path(codex) / "skills";
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".claw" / "skills";
    throw std::system_error(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "unable to resolve a skills install root; set CLAW_CONFIG_HOME or HOME");
}

// ---- resolve_skill_install_source ----------------------------------------
SkillInstallSource resolve_skill_install_source(std::string_view source_str,
                                                 const std::filesystem::path& cwd)
{
    std::filesystem::path candidate(source_str);
    if (!candidate.is_absolute()) candidate = cwd / candidate;
    auto source = std::filesystem::canonical(candidate);
    if (std::filesystem::is_directory(source)) {
        auto prompt_path = source / "SKILL.md";
        if (!std::filesystem::is_regular_file(prompt_path))
            throw std::system_error(
                std::make_error_code(std::errc::invalid_argument),
                std::format("skill directory '{}' must contain SKILL.md", source.string()));
        return SkillInstallSourceDir{source, prompt_path};
    }
    std::string ext = source.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (ext == ".md") return SkillInstallSourceMarkdown{source};
    throw std::system_error(
        std::make_error_code(std::errc::invalid_argument),
        std::format("skill source '{}' must be a directory with SKILL.md or a markdown file",
                    source.string()));
}

// ---- derive_skill_install_name -------------------------------------------
std::string derive_skill_install_name(const SkillInstallSource& source,
                                       std::optional<std::string_view> declared_name)
{
    // Try declared_name first, then fallback_name
    if (declared_name.has_value()) {
        if (auto name = sanitize_skill_invocation_name(*declared_name))
            return *name;
    }
    auto fallback = skill_install_source_fallback_name(source);
    if (fallback.has_value()) {
        if (auto name = sanitize_skill_invocation_name(*fallback))
            return *name;
    }
    throw std::system_error(
        std::make_error_code(std::errc::invalid_argument),
        std::format("unable to derive an installable invocation name from '{}'",
                    skill_install_source_report_path(source).string()));
}

// ---- install_skill_into --------------------------------------------------
InstalledSkill install_skill_into(std::string_view source_str,
                                   const std::filesystem::path& cwd,
                                   const std::filesystem::path& registry_root)
{
    auto source = resolve_skill_install_source(source_str, cwd);
    auto prompt_path = skill_install_source_prompt_path(source);
    std::ifstream f(prompt_path);
    if (!f.is_open())
        throw std::system_error(
            std::make_error_code(std::errc::no_such_file_or_directory),
            prompt_path.string());
    std::string contents{std::istreambuf_iterator<char>(f), {}};
    auto [display_name, _desc] = parse_skill_frontmatter(contents);
    auto invocation_name = derive_skill_install_name(
        source,
        display_name ? std::optional<std::string_view>(*display_name) : std::nullopt);
    auto installed_path = registry_root / invocation_name;

    if (std::filesystem::exists(installed_path))
        throw std::system_error(
            std::make_error_code(std::errc::file_exists),
            std::format("skill '{}' is already installed at {}",
                        invocation_name, installed_path.string()));

    std::filesystem::create_directories(installed_path);
    try {
        if (std::holds_alternative<SkillInstallSourceDir>(source)) {
            copy_directory_contents(std::get<SkillInstallSourceDir>(source).root, installed_path);
        } else {
            std::filesystem::copy_file(std::get<SkillInstallSourceMarkdown>(source).path,
                                       installed_path / "SKILL.md");
        }
    } catch (...) {
        std::filesystem::remove_all(installed_path);
        throw;
    }
    return InstalledSkill{
        std::move(invocation_name),
        std::move(display_name),
        skill_install_source_report_path(source),
        registry_root,
        installed_path,
    };
}

InstalledSkill install_skill(std::string_view source, const std::filesystem::path& cwd) {
    return install_skill_into(source, cwd, default_skill_install_root());
}

// ---- render_plugin_install_report ----------------------------------------
std::string render_plugin_install_report(std::string_view plugin_id,
                                          const plugins::PluginSummary* plugin)
{
    const char* name    = plugin ? plugin->metadata.name.c_str()    : plugin_id.data();
    const char* version = plugin ? plugin->metadata.version.c_str() : "unknown";
    bool enabled        = plugin && plugin->enabled;
    return std::format(
        "Plugins\n  Result           installed {}\n  Name             {}\n"
        "  Version          {}\n  Status           {}",
        plugin_id, name, version, enabled ? "enabled" : "disabled");
}

// ---- resolve_plugin_target -----------------------------------------------
plugins::PluginSummary resolve_plugin_target(plugins::PluginManager& manager,
                                              std::string_view target)
{
    auto all = manager.list_installed_plugins();
    std::vector<plugins::PluginSummary> matches;
    for (auto& p : all)
        if (p.metadata.id == target || p.metadata.name == target)
            matches.push_back(p);
    if (matches.size() == 1) return matches[0];
    if (matches.empty())
        throw plugins::PluginError::not_found(
            std::format("plugin `{}` is not installed or discoverable", target));
    throw plugins::PluginError::invalid_manifest(
        std::format("plugin name `{}` is ambiguous; use the full plugin id", target));
}

} // anonymous namespace

// ===========================================================================
// Public API implementations
// ===========================================================================

std::span<const SlashCommandSpec> slash_command_specs() noexcept {
    return {SLASH_COMMAND_SPECS, std::size(SLASH_COMMAND_SPECS)};
}

std::vector<const SlashCommandSpec*> resume_supported_slash_commands() {
    std::vector<const SlashCommandSpec*> out;
    for (const auto& spec : SLASH_COMMAND_SPECS)
        if (spec.resume_supported) out.push_back(&spec);
    return out;
}

std::optional<std::string> render_slash_command_help_detail(std::string_view name) {
    const auto* spec = find_slash_command_spec(name);
    if (!spec) return std::nullopt;
    auto lines = slash_command_detail_lines(*spec);
    std::string result;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result += '\n';
        result += lines[i];
    }
    return result;
}

std::vector<std::string> suggest_slash_commands(std::string_view input, std::size_t limit) {
    // Strip leading '/' and lowercase
    auto trimmed = trim_sv(input);
    while (!trimmed.empty() && trimmed.front() == '/') trimmed.remove_prefix(1);
    std::string query;
    query.reserve(trimmed.size());
    for (char c : trimmed) query.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (query.empty() || limit == 0) return {};

    // Tuple: (prefix_rank, distance, name_len, name)
    using Entry = std::tuple<int, std::size_t, std::size_t, std::string_view>;
    std::vector<Entry> suggestions;

    for (const auto& spec : SLASH_COMMAND_SPECS) {
        // Build list of names: primary + aliases
        std::vector<std::string> candidates;
        {
            std::string lower_name;
            for (char c : std::string_view(spec.name))
                lower_name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            candidates.push_back(std::move(lower_name));
        }
        for (const char* alias : spec.aliases) {
            std::string lower_alias;
            for (char c : std::string_view(alias))
                lower_alias.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            candidates.push_back(std::move(lower_alias));
        }

        // Find best (prefix_rank, distance) across all candidates
        std::optional<std::pair<int, std::size_t>> best;
        for (const auto& cand : candidates) {
            int prefix_rank = 2;
            if (cand.starts_with(query) || query.starts_with(cand))      prefix_rank = 0;
            else if (cand.find(query) != std::string::npos ||
                     query.find(cand) != std::string::npos)               prefix_rank = 1;
            std::size_t dist = levenshtein_distance(cand, query);
            auto cur = std::make_pair(prefix_rank, dist);
            if (!best.has_value() || cur < *best) best = cur;
        }

        if (best.has_value()) {
            auto [pr, dist] = *best;
            if (pr <= 1 || dist <= 2)
                suggestions.emplace_back(pr, dist, std::string_view(spec.name).size(), spec.name);
        }
    }

    std::sort(suggestions.begin(), suggestions.end());
    std::vector<std::string> result;
    for (std::size_t i = 0; i < suggestions.size() && i < limit; ++i)
        result.push_back(std::format("/{}", std::get<3>(suggestions[i])));
    return result;
}

std::string render_slash_command_help() {
    std::vector<std::string> lines = {
        "Slash commands",
        "  Start here        /status, /diff, /agents, /skills, /commit",
        "  [resume]          also works with --resume SESSION.jsonl",
        "",
    };

    constexpr const char* categories[] = {
        "Session & visibility",
        "Workspace & git",
        "Discovery & debugging",
        "Analysis & automation",
    };

    for (const char* cat : categories) {
        lines.push_back(cat);
        for (const auto& spec : SLASH_COMMAND_SPECS)
            if (std::string_view(slash_command_category(spec.name)) == cat)
                lines.push_back(format_slash_command_help_line(spec));
        lines.push_back("");
    }

    // Trim trailing empty lines (mirrors Rust's rev/skip_while/rev pattern)
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();

    std::string result;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result += '\n';
        result += lines[i];
    }
    return result;
}

// ---------------------------------------------------------------------------
// validate_slash_command_input  (the core parser)
// ---------------------------------------------------------------------------
Result<std::optional<SlashCommand>, SlashCommandParseError>
validate_slash_command_input(std::string_view raw_input)
{
    using R = Result<std::optional<SlashCommand>, SlashCommandParseError>;

    auto trimmed = trim_sv(raw_input);
    if (trimmed.empty() || trimmed.front() != '/')
        return R::ok(std::nullopt);

    // Strip the leading '/'
    auto without_slash = trimmed.substr(1);
    auto tokens = split_whitespace(without_slash);

    if (tokens.empty() || tokens[0].empty())
        return R::err(SlashCommandParseError(
            "Slash command name is missing. Use /help to list available slash commands."));

    auto command = tokens[0];
    std::vector<std::string_view> args(tokens.begin() + 1, tokens.end());
    auto remainder = remainder_after_command(trimmed, command);

    // -----------------------------------------------------------------------
    // Big match – one branch per command name/alias
    // -----------------------------------------------------------------------

    // Helper lambda for no-arg commands
    auto no_arg = [&](SlashCommand cmd) -> R {
        auto r = validate_no_args(command, args);
        if (r.is_error()) return R::err(r.error());
        return R::ok(std::move(cmd));
    };

    // --- No-arg commands ----
    if (command == "help")             return no_arg(slash_command::Help{});
    if (command == "status")           return no_arg(slash_command::Status{});
    if (command == "sandbox")          return no_arg(slash_command::Sandbox{});
    if (command == "compact")          return no_arg(slash_command::Compact{});
    if (command == "commit")           return no_arg(slash_command::Commit{});
    if (command == "debug-tool-call")  return no_arg(slash_command::DebugToolCall{});
    if (command == "cost")             return no_arg(slash_command::Cost{});
    if (command == "memory")           return no_arg(slash_command::Memory{});
    if (command == "init")             return no_arg(slash_command::Init{});
    if (command == "diff")             return no_arg(slash_command::Diff{});
    if (command == "version")          return no_arg(slash_command::Version{});
    if (command == "doctor")           return no_arg(slash_command::Doctor{});
    if (command == "login")            return no_arg(slash_command::Login{});
    if (command == "logout")           return no_arg(slash_command::Logout{});
    if (command == "vim")              return no_arg(slash_command::Vim{});
    if (command == "upgrade")          return no_arg(slash_command::Upgrade{});
    if (command == "stats")            return no_arg(slash_command::Stats{});
    if (command == "share")            return no_arg(slash_command::Share{});
    if (command == "feedback")         return no_arg(slash_command::Feedback{});
    if (command == "files")            return no_arg(slash_command::Files{});
    if (command == "fast")             return no_arg(slash_command::Fast{});
    if (command == "exit")             return no_arg(slash_command::Exit{});
    if (command == "summary")          return no_arg(slash_command::Summary{});
    if (command == "desktop")          return no_arg(slash_command::Desktop{});
    if (command == "brief")            return no_arg(slash_command::Brief{});
    if (command == "advisor")          return no_arg(slash_command::Advisor{});
    if (command == "stickers")         return no_arg(slash_command::Stickers{});
    if (command == "insights")         return no_arg(slash_command::Insights{});
    if (command == "thinkback")        return no_arg(slash_command::Thinkback{});
    if (command == "release-notes")    return no_arg(slash_command::ReleaseNotes{});
    if (command == "security-review")  return no_arg(slash_command::SecurityReview{});
    if (command == "keybindings")      return no_arg(slash_command::Keybindings{});
    if (command == "privacy-settings") return no_arg(slash_command::PrivacySettings{});

    // --- Commands with optional/required args ---
    if (command == "bughunter")
        return R::ok(slash_command::Bughunter{remainder});

    if (command == "pr")
        return R::ok(slash_command::Pr{remainder});

    if (command == "issue")
        return R::ok(slash_command::Issue{remainder});

    if (command == "ultraplan")
        return R::ok(slash_command::Ultraplan{remainder});

    if (command == "teleport") {
        auto r = require_remainder("teleport", remainder, "<symbol-or-path>");
        if (r.is_error()) return R::err(r.error());
        return R::ok(slash_command::Teleport{std::move(r.value())});
    }

    if (command == "model") {
        auto r = optional_single_arg("model", args, "[model]");
        if (r.is_error()) return R::err(r.error());
        return R::ok(slash_command::Model{std::move(r.value())});
    }

    if (command == "permissions") {
        auto r = parse_permissions_mode(args);
        if (r.is_error()) return R::err(r.error());
        return R::ok(slash_command::Permissions{std::move(r.value())});
    }

    if (command == "clear") {
        auto r = parse_clear_args(args);
        if (r.is_error()) return R::err(r.error());
        return R::ok(slash_command::Clear{r.value()});
    }

    if (command == "resume") {
        auto r = require_remainder("resume", remainder, "<session-path>");
        if (r.is_error()) return R::err(r.error());
        return R::ok(slash_command::Resume{std::move(r.value())});
    }

    if (command == "config") {
        auto r = parse_config_section(args);
        if (r.is_error()) return R::err(r.error());
        return R::ok(slash_command::Config{std::move(r.value())});
    }

    if (command == "mcp") {
        auto r = parse_mcp_command(args);
        if (r.is_error()) return R::err(r.error());
        return R::ok(std::move(r.value()));
    }

    if (command == "export")
        return R::ok(slash_command::Export{remainder});

    if (command == "session") {
        auto r = parse_session_command(args);
        if (r.is_error()) return R::err(r.error());
        return R::ok(std::move(r.value()));
    }

    if (command == "plugin" || command == "plugins" || command == "marketplace") {
        auto r = parse_plugin_command(args);
        if (r.is_error()) return R::err(r.error());
        return R::ok(std::move(r.value()));
    }

    if (command == "agents") {
        auto r = parse_list_or_help_args(
            command,
            remainder ? std::optional<std::string>(*remainder) : std::nullopt);
        if (r.is_error()) return R::err(r.error());
        return R::ok(slash_command::Agents{std::move(r.value())});
    }

    if (command == "skills" || command == "skill") {
        auto r = parse_skills_args(remainder ? std::optional<std::string_view>(*remainder)
                                             : std::optional<std::string_view>{});
        if (r.is_error()) return R::err(r.error());
        return R::ok(slash_command::Skills{std::move(r.value())});
    }

    // Simple "remainder" commands
    if (command == "plan")         return R::ok(slash_command::Plan{remainder});
    if (command == "review")       return R::ok(slash_command::Review{remainder});
    if (command == "tasks")        return R::ok(slash_command::Tasks{remainder});
    if (command == "theme")        return R::ok(slash_command::Theme{remainder});
    if (command == "voice")        return R::ok(slash_command::Voice{remainder});
    if (command == "usage")        return R::ok(slash_command::Usage{remainder});
    if (command == "rename")       return R::ok(slash_command::Rename{remainder});
    if (command == "copy")         return R::ok(slash_command::Copy{remainder});
    if (command == "hooks")        return R::ok(slash_command::Hooks{remainder});
    if (command == "context")      return R::ok(slash_command::Context{remainder});
    if (command == "color")        return R::ok(slash_command::Color{remainder});
    if (command == "effort")       return R::ok(slash_command::Effort{remainder});
    if (command == "branch")       return R::ok(slash_command::Branch{remainder});
    if (command == "rewind")       return R::ok(slash_command::Rewind{remainder});
    if (command == "ide")          return R::ok(slash_command::Ide{remainder});
    if (command == "tag")          return R::ok(slash_command::Tag{remainder});
    if (command == "output-style") return R::ok(slash_command::OutputStyle{remainder});
    if (command == "add-dir")      return R::ok(slash_command::AddDir{remainder});

    // Unknown
    return R::ok(slash_command::Unknown{std::string(command)});
}

Result<std::optional<SlashCommand>, SlashCommandParseError>
parse_slash_command(std::string_view input) {
    return validate_slash_command_input(input);
}

// ---------------------------------------------------------------------------
// handle_slash_command
// ---------------------------------------------------------------------------
std::optional<SlashCommandResult>
handle_slash_command(std::string_view input,
                     const runtime::Session& session,
                     runtime::CompactionConfig compaction)
{
    auto parse_result = validate_slash_command_input(input);
    if (parse_result.is_error()) {
        return SlashCommandResult{parse_result.error().message(), session};
    }
    if (!parse_result.value().has_value()) {
        return std::nullopt;
    }

    const SlashCommand& command = *parse_result.value();

    return std::visit([&](const auto& cmd) -> std::optional<SlashCommandResult> {
        using T = std::decay_t<decltype(cmd)>;

        if constexpr (std::is_same_v<T, slash_command::Compact>) {
            auto result = runtime::compact_session(session, compaction);
            std::string msg;
            if (result.removed_message_count == 0)
                msg = "Compaction skipped: session is below the compaction threshold.";
            else
                msg = std::format("Compacted {} messages into a resumable system summary.",
                                  result.removed_message_count);
            return SlashCommandResult{std::move(msg), std::move(result.compacted_session)};
        }
        else if constexpr (std::is_same_v<T, slash_command::Help>) {
            return SlashCommandResult{render_slash_command_help(), session};
        }
        else {
            // All runtime-bound commands return nullopt (handled by the caller)
            return std::nullopt;
        }
    }, command);
}

// ---------------------------------------------------------------------------
// handle_plugins_slash_command
// ---------------------------------------------------------------------------
Result<PluginsCommandResult, plugins::PluginError>
handle_plugins_slash_command(std::optional<std::string_view> action,
                              std::optional<std::string_view> target,
                              plugins::PluginManager&          manager)
{
    using R = Result<PluginsCommandResult, plugins::PluginError>;
    try {
        if (!action.has_value() || *action == "list") {
            auto installed = manager.list_installed_plugins();
            return R::ok(PluginsCommandResult{render_plugins_report(installed), false});
        }
        if (*action == "install") {
            if (!target.has_value())
                return R::ok(PluginsCommandResult{"Usage: /plugins install <path>", false});
            auto install = manager.install(*target);
            auto all = manager.list_installed_plugins();
            const plugins::PluginSummary* found = nullptr;
            for (const auto& p : all)
                if (p.metadata.id == install.plugin_id) { found = &p; break; }
            return R::ok(PluginsCommandResult{
                render_plugin_install_report(install.plugin_id, found), true});
        }
        if (*action == "enable") {
            if (!target.has_value())
                return R::ok(PluginsCommandResult{"Usage: /plugins enable <name>", false});
            auto plugin = resolve_plugin_target(manager, *target);
            manager.enable(plugin.metadata.id);
            return R::ok(PluginsCommandResult{
                std::format(
                    "Plugins\n  Result           enabled {}\n  Name             {}\n"
                    "  Version          {}\n  Status           enabled",
                    plugin.metadata.id, plugin.metadata.name, plugin.metadata.version),
                true});
        }
        if (*action == "disable") {
            if (!target.has_value())
                return R::ok(PluginsCommandResult{"Usage: /plugins disable <name>", false});
            auto plugin = resolve_plugin_target(manager, *target);
            manager.disable(plugin.metadata.id);
            return R::ok(PluginsCommandResult{
                std::format(
                    "Plugins\n  Result           disabled {}\n  Name             {}\n"
                    "  Version          {}\n  Status           disabled",
                    plugin.metadata.id, plugin.metadata.name, plugin.metadata.version),
                true});
        }
        if (*action == "uninstall") {
            if (!target.has_value())
                return R::ok(PluginsCommandResult{"Usage: /plugins uninstall <plugin-id>", false});
            manager.uninstall(*target);
            return R::ok(PluginsCommandResult{
                std::format("Plugins\n  Result           uninstalled {}", *target), true});
        }
        if (*action == "update") {
            if (!target.has_value())
                return R::ok(PluginsCommandResult{"Usage: /plugins update <plugin-id>", false});
            auto update = manager.update(*target);
            auto all = manager.list_installed_plugins();
            const plugins::PluginSummary* found = nullptr;
            for (const auto& p : all)
                if (p.metadata.id == update.plugin_id) { found = &p; break; }
            std::string found_name = found ? found->metadata.name : update.plugin_id;
            const char* status = found ? (found->enabled ? "enabled" : "disabled") : "unknown";
            return R::ok(PluginsCommandResult{
                std::format(
                    "Plugins\n  Result           updated {}\n  Name             {}\n"
                    "  Old version      {}\n  New version      {}\n  Status           {}",
                    update.plugin_id, found_name,
                    update.old_version, update.new_version, status),
                true});
        }
        // Unknown action
        return R::ok(PluginsCommandResult{
            std::format("Unknown /plugins action '{}'. "
                        "Use list, install, enable, disable, uninstall, or update.", *action),
            false});
    } catch (const plugins::PluginError& e) {
        return R::err(e);
    }
}

// ---------------------------------------------------------------------------
// render_plugins_report
// ---------------------------------------------------------------------------
std::string render_plugins_report(std::span<const plugins::PluginSummary> plugin_list) {
    std::vector<std::string> lines = {"Plugins"};
    if (plugin_list.empty()) {
        lines.push_back("  No plugins installed.");
        return lines[0] + '\n' + lines[1];
    }
    for (const auto& plugin : plugin_list) {
        const char* enabled = plugin.enabled ? "enabled" : "disabled";
        lines.push_back(std::format("  {:<20} v{:<10} {}",
                                    plugin.metadata.name, plugin.metadata.version, enabled));
    }
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

// ===========================================================================
// Public agent / skill / mcp handlers
// ===========================================================================

Result<std::string, std::error_code>
handle_agents_slash_command(std::optional<std::string_view> args,
                             const std::filesystem::path&    cwd)
{
    using R = Result<std::string, std::error_code>;
    auto norm = normalize_optional_args(args);
    if (!norm.has_value() || *norm == "list") {
        try {
            auto roots = discover_definition_roots(cwd, "agents");
            auto agents = load_agents_from_roots(roots);
            return R::ok(render_agents_report(agents));
        } catch (const std::system_error& e) {
            return R::err(e.code());
        }
    }
    if (*norm == "-h" || *norm == "--help" || *norm == "help")
        return R::ok(render_agents_usage(std::nullopt));
    return R::ok(render_agents_usage(*norm));
}

Result<std::string, std::error_code>
handle_skills_slash_command(std::optional<std::string_view> args,
                             const std::filesystem::path&    cwd)
{
    using R = Result<std::string, std::error_code>;
    auto norm = normalize_optional_args(args);
    if (!norm.has_value() || *norm == "list") {
        try {
            auto roots = discover_skill_roots(cwd);
            auto skills = load_skills_from_roots(roots);
            return R::ok(render_skills_report(skills));
        } catch (const std::system_error& e) {
            return R::err(e.code());
        }
    }
    if (*norm == "install")
        return R::ok(render_skills_usage(std::string_view("install")));
    if (norm->starts_with("install ")) {
        auto target = trim_sv(norm->substr(std::string_view("install ").size()));
        if (target.empty()) return R::ok(render_skills_usage(std::string_view("install")));
        try {
            auto skill = install_skill(target, cwd);
            return R::ok(render_skill_install_report(skill));
        } catch (const std::system_error& e) {
            return R::err(e.code());
        }
    }
    if (*norm == "-h" || *norm == "--help" || *norm == "help")
        return R::ok(render_skills_usage(std::nullopt));
    return R::ok(render_skills_usage(*norm));
}

// ---- classify_skills_slash_command / resolve_skill_invocation ----

SkillSlashDispatchResult
classify_skills_slash_command(std::optional<std::string_view> args) {
    auto norm = normalize_optional_args(args);
    if (!norm.has_value())                       return {SkillSlashDispatch::Local, {}};
    if (*norm == "list" || *norm == "help"
        || *norm == "-h" || *norm == "--help")    return {SkillSlashDispatch::Local, {}};
    if (*norm == "install" || norm->starts_with("install "))
                                                  return {SkillSlashDispatch::Local, {}};
    // Direct skill invocation -- strip leading '/' and prepend '$'.
    std::string prompt = "$";
    std::string_view trimmed = *norm;
    while (!trimmed.empty() && trimmed.front() == '/') trimmed.remove_prefix(1);
    prompt += trimmed;
    return {SkillSlashDispatch::Invoke, std::move(prompt)};
}

// Resolve a skill path inside the discovered roots.
static std::optional<std::filesystem::path>
resolve_skill_path_internal(const std::filesystem::path& cwd, std::string_view skill) {
    std::string requested{skill};
    // trim leading '/' and '$'
    while (!requested.empty() && (requested.front() == '/' || requested.front() == '$'))
        requested.erase(requested.begin());
    if (requested.empty()) return std::nullopt;

    auto roots = discover_skill_roots(cwd);
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root.path, ec)) continue;
        for (auto& entry : std::filesystem::directory_iterator(root.path, ec)) {
            if (root.origin == SkillOrigin::SkillsDir) {
                if (!entry.is_directory()) continue;
                auto skill_path = entry.path() / "SKILL.md";
                if (!std::filesystem::is_regular_file(skill_path)) continue;
                std::ifstream f(skill_path);
                if (!f) continue;
                std::string contents((std::istreambuf_iterator<char>(f)), {});
                auto [name, _desc] = parse_skill_frontmatter(contents);
                std::string entry_name = name.value_or(entry.path().filename().string());
                std::string lower_entry, lower_req;
                for (char c : entry_name) lower_entry += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (char c : requested)  lower_req   += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower_entry == lower_req) return skill_path;
            } else {
                // LegacyCommandsDir
                auto path = entry.path();
                std::filesystem::path md_path;
                if (entry.is_directory()) {
                    md_path = path / "SKILL.md";
                    if (!std::filesystem::is_regular_file(md_path)) continue;
                } else if (path.extension() == ".md") {
                    md_path = path;
                } else {
                    continue;
                }
                std::ifstream f(md_path);
                if (!f) continue;
                std::string contents((std::istreambuf_iterator<char>(f)), {});
                auto fallback = md_path.stem().string();
                auto [name, _desc] = parse_skill_frontmatter(contents);
                std::string entry_name = name.value_or(fallback);
                std::string lower_entry, lower_req;
                for (char c : entry_name) lower_entry += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (char c : requested)  lower_req   += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower_entry == lower_req) return md_path;
            }
        }
    }
    return std::nullopt;
}

Result<SkillSlashDispatchResult, std::string>
resolve_skill_invocation(const std::filesystem::path& cwd,
                         std::optional<std::string_view> args) {
    using R = Result<SkillSlashDispatchResult, std::string>;
    auto dispatch = classify_skills_slash_command(args);
    if (dispatch.kind != SkillSlashDispatch::Invoke) return R::ok(std::move(dispatch));

    // Extract the skill name from the "$skill [args]" prompt.
    std::string_view prompt = dispatch.invoke_prompt;
    if (!prompt.empty() && prompt.front() == '$') prompt.remove_prefix(1);
    // Take first whitespace-delimited token as skill name.
    auto space_pos = prompt.find(' ');
    std::string skill_token{prompt.substr(0, space_pos)};
    if (skill_token.empty()) return R::ok(std::move(dispatch));

    auto resolved = resolve_skill_path_internal(cwd, skill_token);
    if (resolved.has_value()) return R::ok(std::move(dispatch));

    // Build error message with available skills list.
    std::string message = "Unknown skill: " + skill_token;
    try {
        auto roots = discover_skill_roots(cwd);
        auto available = load_skills_from_roots(roots);
        std::vector<std::string> names;
        for (const auto& s : available)
            if (!s.shadowed_by.has_value()) names.push_back(s.name);
        if (!names.empty()) {
            message += "\n  Available skills: ";
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i) message += ", ";
                message += names[i];
            }
        }
    } catch (...) {}
    message += "\n  Usage: /skills [list|install <path>|help|<skill> [args]]";
    return R::err(std::move(message));
}

Result<std::string, runtime::ConfigError>
handle_mcp_slash_command(std::optional<std::string_view> args,
                          const std::filesystem::path&    cwd)
{
    auto loader = runtime::ConfigLoader::default_for(cwd);
    return render_mcp_report_for(loader, cwd, args);
}

} // namespace claw::commands
