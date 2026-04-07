// main.cpp -- C++20 port of main.rs
// Entry point, CLI argument parsing, action dispatch, and all helper/formatting
// functions (format_status_report, GitWorkspaceSummary, format_model_report,
// format_permissions_report, format_cost_report, format_compact_report,
// format_resume_report, render_repl_help, git helpers, etc.).
#include "app.hpp"
#include "args.hpp"
#include "init.hpp"
#include "input.hpp"
#include "render.hpp"

// Runtime crate headers for full-featured handlers
#include "commands.hpp"
#include "error.hpp"
#include "oauth.hpp"
#include "session.hpp"
#include "prompt.hpp"
#include "bootstrap.hpp"
#include "compat_harness.hpp"
#include "bash.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>   // ShellExecuteA
#else
#  include <unistd.h>    // fork/exec
#endif

namespace claw {
namespace {

// ===========================================================================
// Constants (mirrors main.rs constants)
// ===========================================================================

constexpr std::string_view DEFAULT_MODEL      = "claude-opus-4-6";
constexpr std::string_view DEFAULT_DATE       = "2026-03-31";
constexpr std::string_view LATEST_SESSION_REF = "latest";
constexpr std::string_view PROGRAM_NAME       = "claw";
constexpr std::string_view VERSION_STRING     = "0.1.0-cpp";

constexpr std::string_view CLI_OPTION_SUGGESTIONS[] = {
    "--help", "-h", "--version", "-V",
    "--model", "--output-format", "--permission-mode",
    "--dangerously-skip-permissions",
    "--allowedTools", "--allowed-tools",
    "--resume", "--print", "-p",
};

// ===========================================================================
// CliAction (mirrors Rust's CliAction enum)
// ===========================================================================

struct ActionDumpManifests  {};
struct ActionBootstrapPlan  {};
struct ActionPrintSystemPrompt { std::filesystem::path cwd; std::string date; };
struct ActionVersion        {};
struct ActionHelp           {};
struct ActionResumeSession  {
    std::filesystem::path session_path;
    std::vector<std::string> commands;
};
struct ActionStatus         { std::string model; PermissionMode permission_mode; };
struct ActionSandbox        {};
struct ActionPrompt {
    std::string prompt;
    std::string model;
    OutputFormat output_format{OutputFormat::Text};
    PermissionMode permission_mode{PermissionMode::DangerFullAccess};
};
struct ActionLogin  {};
struct ActionLogout {};
struct ActionInit   {};
struct ActionRepl   {
    std::string model;
    std::optional<std::set<std::string>> allowed_tools;
    PermissionMode permission_mode;
};
struct ActionAgents { std::optional<std::string> args; };
struct ActionMcp    { std::optional<std::string> args; };
struct ActionSkills { std::optional<std::string> args; };
struct ActionDoctor {};

using CliAction = std::variant<
    ActionDumpManifests, ActionBootstrapPlan, ActionPrintSystemPrompt,
    ActionVersion, ActionHelp,
    ActionResumeSession, ActionStatus, ActionSandbox,
    ActionPrompt, ActionLogin, ActionLogout, ActionInit, ActionRepl,
    ActionAgents, ActionMcp, ActionSkills, ActionDoctor>;

// ===========================================================================
// GitWorkspaceSummary (mirrors Rust's GitWorkspaceSummary struct)
// ===========================================================================

struct GitWorkspaceSummary {
    std::size_t changed_files{0};
    std::size_t staged_files{0};
    std::size_t unstaged_files{0};
    std::size_t untracked_files{0};
    std::size_t conflicted_files{0};

    [[nodiscard]] bool is_clean() const { return changed_files == 0; }

    [[nodiscard]] std::string headline() const {
        if (is_clean()) return "clean";
        std::vector<std::string> details;
        if (staged_files    > 0) details.push_back(std::to_string(staged_files)    + " staged");
        if (unstaged_files  > 0) details.push_back(std::to_string(unstaged_files)  + " unstaged");
        if (untracked_files > 0) details.push_back(std::to_string(untracked_files) + " untracked");
        if (conflicted_files> 0) details.push_back(std::to_string(conflicted_files)+ " conflicted");
        std::string joined;
        for (std::size_t i = 0; i < details.size(); ++i) {
            if (i) joined += ", ";
            joined += details[i];
        }
        return "dirty \u00b7 " + std::to_string(changed_files) + " files \u00b7 " + joined;
    }
};

// ===========================================================================
// Levenshtein distance (mirrors Rust's levenshtein_distance)
// ===========================================================================

std::size_t levenshtein_distance(std::string_view left, std::string_view right) {
    if (left.empty())  return right.size();
    if (right.empty()) return left.size();

    std::vector<std::size_t> prev(right.size() + 1);
    std::vector<std::size_t> curr(right.size() + 1);
    for (std::size_t i = 0; i <= right.size(); ++i) prev[i] = i;

    for (std::size_t i = 0; i < left.size(); ++i) {
        curr[0] = i + 1;
        for (std::size_t j = 0; j < right.size(); ++j) {
            std::size_t sub_cost = (left[i] != right[j]) ? 1 : 0;
            curr[j + 1] = std::min({prev[j + 1] + 1, curr[j] + 1, prev[j] + sub_cost});
        }
        std::swap(prev, curr);
    }
    return prev[right.size()];
}

// ===========================================================================
// ranked_suggestions / suggest_closest_term (mirrors Rust)
// ===========================================================================

std::vector<std::string_view>
ranked_suggestions(std::string_view input,
                   const std::vector<std::string_view>& candidates) {
    std::string norm_in{input};
    // Strip leading '/' for comparison.
    if (!norm_in.empty() && norm_in[0] == '/') norm_in.erase(norm_in.begin());
    std::transform(norm_in.begin(), norm_in.end(), norm_in.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    std::vector<std::pair<std::size_t, std::string_view>> scored;
    for (auto c : candidates) {
        std::string nc{c};
        if (!nc.empty() && nc[0] == '/') nc.erase(nc.begin());
        std::transform(nc.begin(), nc.end(), nc.begin(),
            [](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });

        auto dist = levenshtein_distance(norm_in, nc);
        bool prefix_match = (nc.find(norm_in) == 0) || (norm_in.find(nc) == 0);
        std::size_t score = dist + (prefix_match ? 0 : 1);
        if (score <= 4)
            scored.emplace_back(score, c);
    }
    std::stable_sort(scored.begin(), scored.end(),
        [](const auto& a, const auto& b){ return a.first < b.first; });

    std::vector<std::string_view> result;
    for (std::size_t i = 0; i < scored.size() && i < 3; ++i)
        result.push_back(scored[i].second);
    return result;
}

std::optional<std::string_view>
suggest_closest_term(std::string_view input,
                     const std::vector<std::string_view>& candidates) {
    auto suggestions = ranked_suggestions(input, candidates);
    if (!suggestions.empty()) return suggestions[0];
    return std::nullopt;
}

// ===========================================================================
// format_unknown_option (mirrors Rust)
// ===========================================================================

std::string format_unknown_option(std::string_view option) {
    std::vector<std::string_view> cands(
        std::begin(CLI_OPTION_SUGGESTIONS), std::end(CLI_OPTION_SUGGESTIONS));
    std::string msg = "unknown option: " + std::string{option};
    auto suggestion = suggest_closest_term(option, cands);
    if (suggestion) { msg += "\nDid you mean "; msg += *suggestion; msg += "?"; }
    msg += "\nRun `claw --help` for usage.";
    return msg;
}

// ===========================================================================
// resolve_model_alias (mirrors Rust)
// ===========================================================================

std::string resolve_model_alias(std::string_view model) {
    if (model == "opus")   return "claude-opus-4-6";
    if (model == "sonnet") return "claude-sonnet-4-6";
    if (model == "haiku")  return "claude-haiku-4-5-20251213";
    return std::string{model};
}

// ===========================================================================
// normalize_permission_mode / parse_permission_mode_arg (mirrors Rust)
// ===========================================================================

/// Returns the canonical label (e.g. "read-only") or empty on failure.
std::optional<std::string_view> normalize_permission_mode(std::string_view s) noexcept {
    if (s == "read-only")          return "read-only";
    if (s == "workspace-write")    return "workspace-write";
    if (s == "danger-full-access") return "danger-full-access";
    return std::nullopt;
}

PermissionMode parse_permission_mode_arg(std::string_view value) {
    auto m = permission_mode_from_str(value);
    if (!m)
        throw std::runtime_error(
            "unsupported permission mode '" + std::string{value} +
            "'. Use read-only, workspace-write, or danger-full-access.");
    return *m;
}

OutputFormat parse_output_format_arg(std::string_view value) {
    auto f = output_format_from_str(value);
    if (!f)
        throw std::runtime_error(
            "unsupported value for --output-format: " + std::string{value} +
            " (expected text or json)");
    return *f;
}

// ===========================================================================
// join_optional_args (mirrors Rust)
// ===========================================================================

std::optional<std::string>
join_optional_args(const std::vector<std::string>& args) {
    if (args.empty()) return std::nullopt;
    std::string joined;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) joined += " ";
        joined += args[i];
    }
    // trim
    while (!joined.empty() && joined.front() == ' ') joined.erase(joined.begin());
    while (!joined.empty() && joined.back()  == ' ') joined.pop_back();
    return joined.empty() ? std::nullopt : std::optional{joined};
}

// ===========================================================================
// parse_resume_args (mirrors Rust's parse_resume_args)
// ===========================================================================

ActionResumeSession parse_resume_args(const std::vector<std::string>& args) {
    std::filesystem::path session_path;
    std::size_t start = 0;

    // If the first token looks like a slash command, use latest.
    if (args.empty() ||
        (!args[0].empty() && args[0][0] == '/'))
    {
        session_path = LATEST_SESSION_REF;
        start = 0;
    } else {
        session_path = args[0];
        start = 1;
    }

    std::vector<std::string> commands;
    std::string cur_cmd;
    for (std::size_t i = start; i < args.size(); ++i) {
        auto& tok = args[i];
        std::string_view t = tok;
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
            t.remove_prefix(1);
        if (!t.empty() && t[0] == '/') {
            if (!cur_cmd.empty()) {
                commands.push_back(cur_cmd);
                cur_cmd.clear();
            }
            cur_cmd = std::string(t);
        } else {
            if (cur_cmd.empty())
                throw std::runtime_error(
                    "--resume trailing arguments must be slash commands");
            cur_cmd += " ";
            cur_cmd += tok;
        }
    }
    if (!cur_cmd.empty()) commands.push_back(cur_cmd);
    return ActionResumeSession{session_path, commands};
}

// ===========================================================================
// parse_args (mirrors Rust's parse_args)
// ===========================================================================

CliAction parse_args(const std::vector<std::string>& args) {
    std::string model{DEFAULT_MODEL};
    OutputFormat output_format = OutputFormat::Text;
    std::optional<PermissionMode> permission_mode_override;
    bool wants_help    = false;
    bool wants_version = false;
    std::vector<std::string> rest;
    std::size_t i = 0;

    auto next_value = [&](std::string_view flag_name) -> std::string_view {
        if (i + 1 >= args.size())
            throw std::runtime_error("missing value for " + std::string{flag_name});
        return args[++i];
    };

    auto strip_prefix = [](std::string_view arg, std::string_view prefix)
        -> std::optional<std::string_view> {
        if (arg.size() > prefix.size() && arg.substr(0, prefix.size()) == prefix)
            return arg.substr(prefix.size());
        return std::nullopt;
    };

    // Helper: resolve override to concrete mode (mirrors Rust unwrap_or_else)
    auto resolve_permission_mode = [&]() -> PermissionMode {
        return permission_mode_override.value_or(PermissionMode::DangerFullAccess);
    };

    while (i < args.size()) {
        std::string_view arg = args[i];

        if ((arg == "--help" || arg == "-h") && rest.empty()) {
            wants_help = true; ++i; continue;
        }
        if (arg == "--version" || arg == "-V") {
            wants_version = true; ++i; continue;
        }
        if (arg == "--model") {
            model = resolve_model_alias(next_value("--model")); ++i; continue;
        }
        if (auto v = strip_prefix(arg, "--model=")) {
            model = resolve_model_alias(*v); ++i; continue;
        }
        if (arg == "--output-format") {
            output_format = parse_output_format_arg(next_value("--output-format")); ++i; continue;
        }
        if (auto v = strip_prefix(arg, "--output-format=")) {
            output_format = parse_output_format_arg(*v); ++i; continue;
        }
        if (arg == "--permission-mode") {
            permission_mode_override = parse_permission_mode_arg(next_value("--permission-mode")); ++i; continue;
        }
        if (auto v = strip_prefix(arg, "--permission-mode=")) {
            permission_mode_override = parse_permission_mode_arg(*v); ++i; continue;
        }
        if (arg == "--dangerously-skip-permissions") {
            permission_mode_override = PermissionMode::DangerFullAccess; ++i; continue;
        }
        if (arg == "--print") {
            output_format = OutputFormat::Text; ++i; continue;
        }
        if (arg == "-p") {
            std::string prompt;
            for (std::size_t j = i + 1; j < args.size(); ++j) {
                if (j > i + 1) prompt += " ";
                prompt += args[j];
            }
            if (prompt.empty())
                throw std::runtime_error("-p requires a prompt string");
            return ActionPrompt{prompt, resolve_model_alias(model), output_format,
                                resolve_permission_mode()};
        }
        if ((arg == "--resume") && rest.empty()) {
            rest.push_back("--resume"); ++i; continue;
        }
        if (auto v = strip_prefix(arg, "--resume="); v && rest.empty()) {
            rest.push_back("--resume"); rest.push_back(std::string{*v}); ++i; continue;
        }
        if (arg == "--allowedTools" || arg == "--allowed-tools") {
            // Accept the flag and its value; allowed-tools filtering is not
            // implemented in this C++ port.
            if (i + 1 < args.size()) ++i;
            ++i; continue;
        }
        if (auto v = strip_prefix(arg, "--allowedTools="))  { ++i; continue; }
        if (auto v = strip_prefix(arg, "--allowed-tools=")) { ++i; continue; }
        // Unknown flags before any positional arg.
        if (rest.empty() && !arg.empty() && arg[0] == '-') {
            throw std::runtime_error(format_unknown_option(arg));
        }
        rest.push_back(std::string{arg});
        ++i;
    }

    if (wants_help)    return ActionHelp{};
    if (wants_version) return ActionVersion{};

    if (rest.empty()) {
        auto permission_mode = resolve_permission_mode();
        return ActionRepl{model, std::nullopt, permission_mode};
    }

    // --resume
    if (rest[0] == "--resume") {
        std::vector<std::string> resume_args(rest.begin() + 1, rest.end());
        return parse_resume_args(resume_args);
    }

    // Single-word aliases.
    if (rest.size() == 1) {
        auto& w = rest[0];
        if (w == "help")    return ActionHelp{};
        if (w == "version") return ActionVersion{};
        if (w == "status")  return ActionStatus{model, resolve_permission_mode()};
        if (w == "sandbox") return ActionSandbox{};
        if (w == "doctor")  return ActionDoctor{};
        if (w == "login")   return ActionLogin{};
        if (w == "logout")  return ActionLogout{};
        if (w == "init")    return ActionInit{};
    }

    auto permission_mode = resolve_permission_mode();

    // Named subcommands.
    if (rest[0] == "dump-manifests")  return ActionDumpManifests{};
    if (rest[0] == "bootstrap-plan")  return ActionBootstrapPlan{};
    if (rest[0] == "agents")
        return ActionAgents{join_optional_args({rest.begin() + 1, rest.end()})};
    if (rest[0] == "mcp")
        return ActionMcp{join_optional_args({rest.begin() + 1, rest.end()})};
    if (rest[0] == "skills")
        return ActionSkills{join_optional_args({rest.begin() + 1, rest.end()})};
    if (rest[0] == "login")   return ActionLogin{};
    if (rest[0] == "logout")  return ActionLogout{};
    if (rest[0] == "init")    return ActionInit{};
    if (rest[0] == "system-prompt") {
        // system-prompt [--cwd <path>] [--date <date>]
        std::filesystem::path cwd = std::filesystem::current_path();
        std::string date{DEFAULT_DATE};
        for (std::size_t j = 1; j < rest.size(); j += 2) {
            if (rest[j] == "--cwd" && j + 1 < rest.size())
                cwd = rest[j + 1];
            else if (rest[j] == "--date" && j + 1 < rest.size())
                date = rest[j + 1];
            else
                throw std::runtime_error("unknown system-prompt option: " + rest[j]);
        }
        return ActionPrintSystemPrompt{cwd, date};
    }
    if (rest[0] == "prompt") {
        if (rest.size() < 2)
            throw std::runtime_error("prompt subcommand requires a prompt string");
        std::string prompt;
        for (std::size_t j = 1; j < rest.size(); ++j) {
            if (j > 1) prompt += " ";
            prompt += rest[j];
        }
        return ActionPrompt{prompt, model, output_format, permission_mode};
    }

    // Slash commands passed directly on the CLI.
    if (!rest[0].empty() && rest[0][0] == '/') {
        auto cmd = parse_slash_command(rest[0]);
        if (!cmd) throw std::runtime_error("unknown subcommand: " + rest[0]);
        if (std::holds_alternative<SlashHelp>(*cmd)) return ActionHelp{};
        if (std::holds_alternative<SlashUnknown>(*cmd)) {
            auto& u = std::get<SlashUnknown>(*cmd);
            throw std::runtime_error(
                "unknown slash command outside the REPL: /" + u.name +
                "\nRun `claw --help` for CLI usage, or start `claw` and use /help.");
        }
        throw std::runtime_error(
            "slash command " + rest[0] +
            " is interactive-only. Start `claw` and run it there.");
    }

    // Bare words -> treat as a prompt.
    std::string prompt;
    for (std::size_t j = 0; j < rest.size(); ++j) {
        if (j) prompt += " ";
        prompt += rest[j];
    }
    return ActionPrompt{prompt, model, output_format, permission_mode};
}

// ===========================================================================
// Git helpers (mirrors Rust's git helper functions)
// ===========================================================================

std::optional<std::string>
run_git_capture_in(const std::filesystem::path& cwd,
                   const std::vector<std::string>& git_args) {
    claw::runtime::BashCommandInput input;
    input.cwd = cwd.string();
    input.command = "git";
    for (auto& a : git_args) {
        input.command += " '";
        input.command += a;
        input.command += "'";
    }
#ifdef _WIN32
    input.command += " 2>NUL";
#else
    input.command += " 2>/dev/null";
#endif
    auto result = claw::runtime::execute_bash(input);
    if (!result || result->exit_code != 0) return std::nullopt;
    return result->stdout_output;
}

/// Mirrors Rust's resolve_git_branch_for.
std::optional<std::string>
resolve_git_branch_for(const std::filesystem::path& cwd) {
    auto branch = run_git_capture_in(cwd, {"branch", "--show-current"});
    if (branch) {
        auto b = *branch;
        while (!b.empty() && (b.back() == '\n' || b.back() == '\r' || b.back() == ' '))
            b.pop_back();
        if (!b.empty()) return b;
    }
    auto fallback = run_git_capture_in(cwd, {"rev-parse", "--abbrev-ref", "HEAD"});
    if (fallback) {
        auto f = *fallback;
        while (!f.empty() && (f.back() == '\n' || f.back() == '\r' || f.back() == ' '))
            f.pop_back();
        if (!f.empty()) return (f == "HEAD") ? "detached HEAD" : f;
    }
    return std::nullopt;
}

/// Mirrors Rust's parse_git_workspace_summary.
GitWorkspaceSummary
parse_git_workspace_summary(const std::optional<std::string>& status_opt) {
    GitWorkspaceSummary summary;
    if (!status_opt) return summary;
    std::istringstream ss(*status_opt);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.size() < 2 || line.substr(0, 3) == "## " || line.empty())
            continue;
        ++summary.changed_files;
        char idx = line[0], wt = line[1];
        if (idx == '?' && wt == '?') { ++summary.untracked_files; continue; }
        if (idx != ' ') ++summary.staged_files;
        if (wt  != ' ') ++summary.unstaged_files;
        auto is_conflict = [](char c){
            return c == 'U' || c == 'A' || c == 'D';
        };
        if (idx == 'U' || wt == 'U' ||
            (is_conflict(idx) && is_conflict(wt)))
            ++summary.conflicted_files;
    }
    return summary;
}

/// Mirrors Rust's find_git_root_in.
std::optional<std::filesystem::path>
find_git_root_in(const std::filesystem::path& cwd) {
    auto out = run_git_capture_in(cwd, {"rev-parse", "--show-toplevel"});
    if (!out) return std::nullopt;
    std::string path = *out;
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' '))
        path.pop_back();
    if (path.empty()) return std::nullopt;
    return std::filesystem::path(path);
}

// ===========================================================================
// format_* report builders (mirrors Rust)
// ===========================================================================

std::string format_model_report(const std::string& model,
                                  std::size_t message_count,
                                  std::uint32_t turns) {
    return "Model\n"
           "  Current model    " + model + "\n"
           "  Session messages " + std::to_string(message_count) + "\n"
           "  Session turns    " + std::to_string(turns) + "\n\n"
           "Usage\n"
           "  Inspect current model with /model\n"
           "  Switch models with /model <name>";
}

std::string format_model_switch_report(const std::string& previous,
                                        const std::string& next,
                                        std::size_t message_count) {
    return "Model updated\n"
           "  Previous         " + previous + "\n"
           "  Current          " + next + "\n"
           "  Preserved msgs   " + std::to_string(message_count);
}

std::string format_permissions_report(std::string_view mode) {
    struct ModeRow { std::string_view name; std::string_view desc; };
    const ModeRow MODES[] = {
        {"read-only",          "Read/search tools only"},
        {"workspace-write",    "Edit files inside the workspace"},
        {"danger-full-access", "Unrestricted tool access"},
    };
    std::string modes_str;
    for (auto& m : MODES) {
        std::string_view marker = (m.name == mode) ? "\u25cf current" : "\u25cb available";
        std::string padded_name{m.name};
        while (padded_name.size() < 18) padded_name += ' ';
        std::string padded_marker{marker};
        while (padded_marker.size() < 11) padded_marker += ' ';
        modes_str += "  " + padded_name + " " + padded_marker + " " + std::string(m.desc) + "\n";
    }
    return "Permissions\n"
           "  Active mode      " + std::string(mode) + "\n"
           "  Mode status      live session default\n\n"
           "Modes\n"
           + modes_str +
           "\nUsage\n"
           "  Inspect current mode with /permissions\n"
           "  Switch modes with /permissions <mode>";
}

std::string format_permissions_switch_report(const std::string& previous,
                                              const std::string& next) {
    return "Permissions updated\n"
           "  Result           mode switched\n"
           "  Previous mode    " + previous + "\n"
           "  Active mode      " + next + "\n"
           "  Applies to       subsequent tool calls\n"
           "  Usage            /permissions to inspect current mode";
}

struct TokenUsage {
    std::uint64_t input_tokens{0};
    std::uint64_t output_tokens{0};
    std::uint64_t cache_creation_input_tokens{0};
    std::uint64_t cache_read_input_tokens{0};

    [[nodiscard]] std::uint64_t total_tokens() const {
        return input_tokens + output_tokens +
               cache_creation_input_tokens + cache_read_input_tokens;
    }
};

std::string format_cost_report(const TokenUsage& usage) {
    return "Cost\n"
           "  Input tokens     " + std::to_string(usage.input_tokens)                    + "\n"
           "  Output tokens    " + std::to_string(usage.output_tokens)                   + "\n"
           "  Cache create     " + std::to_string(usage.cache_creation_input_tokens)     + "\n"
           "  Cache read       " + std::to_string(usage.cache_read_input_tokens)         + "\n"
           "  Total tokens     " + std::to_string(usage.total_tokens());
}

std::string format_resume_report(const std::string& session_path,
                                   std::size_t message_count,
                                   std::uint32_t turns) {
    return "Session resumed\n"
           "  Session file     " + session_path + "\n"
           "  Messages         " + std::to_string(message_count) + "\n"
           "  Turns            " + std::to_string(turns);
}

std::string render_resume_usage() {
    return "Resume\n"
           "  Usage            /resume <session-path|session-id|" +
               std::string(LATEST_SESSION_REF) + ">\n"
           "  Auto-save        .claw/sessions/<session-id>.jsonl\n"
           "  Tip              use /session list to inspect saved sessions";
}

std::string format_compact_report(std::size_t removed,
                                   std::size_t resulting_messages,
                                   bool skipped) {
    if (skipped)
        return "Compact\n"
               "  Result           skipped\n"
               "  Reason           session below compaction threshold\n"
               "  Messages kept    " + std::to_string(resulting_messages);
    return "Compact\n"
           "  Result           compacted\n"
           "  Messages removed " + std::to_string(removed) + "\n"
           "  Messages kept    " + std::to_string(resulting_messages);
}

std::string format_auto_compaction_notice(std::size_t removed) {
    return "[auto-compacted: removed " + std::to_string(removed) + " messages]";
}

// ===========================================================================
// StatusContext (mirrors Rust's StatusContext struct)
// ===========================================================================

struct StatusContext {
    std::filesystem::path cwd;
    std::optional<std::filesystem::path> session_path;
    std::size_t loaded_config_files{0};
    std::size_t discovered_config_files{0};
    std::size_t memory_file_count{0};
    std::optional<std::filesystem::path> project_root;
    std::optional<std::string> git_branch;
    GitWorkspaceSummary git_summary;
};

StatusContext build_status_context(
    const std::optional<std::filesystem::path>& session_path) {
    StatusContext ctx;
    ctx.cwd = std::filesystem::current_path();
    ctx.session_path = session_path;
    ctx.git_branch   = resolve_git_branch_for(ctx.cwd);
    auto git_status  = run_git_capture_in(ctx.cwd, {"status", "--porcelain=v1", "-b"});
    ctx.git_summary  = parse_git_workspace_summary(git_status);
    ctx.project_root = find_git_root_in(ctx.cwd);
    return ctx;
}

// ===========================================================================
// format_status_report (mirrors Rust's format_status_report)
// ===========================================================================

struct StatusUsage {
    std::size_t   message_count{0};
    std::uint32_t turns{0};
    TokenUsage    latest{};
    TokenUsage    cumulative{};
    std::size_t   estimated_tokens{0};
};

std::string format_status_report(const std::string& model,
                                   const StatusUsage& usage,
                                   std::string_view permission_mode,
                                   const StatusContext& ctx) {
    std::string branch = ctx.git_branch.value_or("unknown");
    std::string project = ctx.project_root.has_value()
                        ? ctx.project_root->string() : "<unknown>";
    std::string session = ctx.session_path.has_value()
                        ? ctx.session_path->string() : "<none>";
    std::string cwd_str = ctx.cwd.string();

    return "Status\n"
           "  Model            " + model + "\n"
           "  Permission mode  " + std::string(permission_mode) + "\n"
           "  Messages         " + std::to_string(usage.message_count) + "\n"
           "  Turns            " + std::to_string(usage.turns) + "\n"
           "  Branch           " + branch + "\n"
           "  Workspace        " + ctx.git_summary.headline() + "\n"
           "  Project root     " + project + "\n"
           "  Directory        " + cwd_str + "\n"
           "  Session          " + session + "\n"
           "\nUsage (this turn)\n"
           "  Input tokens     " + std::to_string(usage.latest.input_tokens) + "\n"
           "  Output tokens    " + std::to_string(usage.latest.output_tokens) + "\n"
           "\nUsage (cumulative)\n"
           "  Input tokens     " + std::to_string(usage.cumulative.input_tokens) + "\n"
           "  Output tokens    " + std::to_string(usage.cumulative.output_tokens) + "\n"
           "  Cache create     " + std::to_string(usage.cumulative.cache_creation_input_tokens) + "\n"
           "  Cache read       " + std::to_string(usage.cumulative.cache_read_input_tokens);
}

// ===========================================================================
// format_sandbox_report (mirrors Rust)
// ===========================================================================

std::string format_sandbox_report(std::string_view status) {
    return "Sandbox\n  Status  " + std::string(status);
}

// ===========================================================================
// render_repl_help (mirrors Rust's render_repl_help)
// ===========================================================================

std::string render_repl_help() {
    return "Slash commands\n\n"
           "  /help                   Show this help\n"
           "  /status                 Live context: model, tokens, git state\n"
           "  /compact                Compact session history to free context\n"
           "  /clear [--confirm]      Start a fresh session\n"
           "  /model [name]           Show or switch the active model\n"
           "  /permissions [mode]     Show or switch permission mode\n"
           "  /config [section]       Inspect config path or section\n"
           "  /memory                 Inspect loaded memory/instruction files\n"
           "  /cost                   Show cumulative token cost\n"
           "  /diff                   Show current git diff\n"
           "  /commit                 Generate a commit message and commit\n"
           "  /export [path]          Export session transcript to a file\n"
           "  /session [list|switch|fork] Manage saved sessions\n"
           "  /resume [session]       Resume a saved session\n"
           "  /init                   Initialise a new project (CLAUDE.md etc.)\n"
           "  /mcp [action] [target]  MCP server management\n"
           "  /agents [args]          List or invoke agents\n"
           "  /skills [args]          List or invoke skills\n"
           "  /sandbox                Show sandbox status\n"
           "  /version                Show version information\n"
           "  /exit, /quit            Exit the REPL";
}

// ===========================================================================
// render_version_report (mirrors Rust)
// ===========================================================================

std::string render_version_report() {
    return "claw " + std::string(VERSION_STRING) +
           " (C++20 port; Rust original by Anthropic)";
}

// ===========================================================================
// render_config_report / render_memory_report (mirrors Rust)
// ===========================================================================

std::string render_config_report(std::optional<std::string_view> section) {
    auto cwd = std::filesystem::current_path();
    auto claw_json = cwd / ".claw.json";
    auto claude_json = cwd / ".claude.json";
    // Prefer .claw.json, fall back to .claude.json for legacy
    auto config_file = std::filesystem::exists(claw_json) ? claw_json : claude_json;
    std::string path_str = std::filesystem::exists(config_file)
                         ? config_file.string() : "<not found>";
    if (!section.has_value())
        return "Config\n  Path  " + path_str;
    return "Config section `" + std::string(*section) +
           "` -- path: " + path_str;
}

std::string render_memory_report() {
    auto cwd = std::filesystem::current_path();
    auto claude_md = cwd / "CLAUDE.md";
    if (std::filesystem::exists(claude_md))
        return "Memory\n  CLAUDE.md  " + claude_md.string();
    return "Memory\n  No CLAUDE.md found in current directory.";
}

// ===========================================================================
// render_diff_report (mirrors Rust)
// ===========================================================================

std::string render_diff_report_for(const std::filesystem::path& cwd) {
    auto diff = run_git_capture_in(cwd, {"diff", "--stat", "HEAD"});
    if (diff) return "Diff\n" + *diff;
    return "Diff\n  (no changes or not a git repository)";
}

std::string render_diff_report() {
    return render_diff_report_for(std::filesystem::current_path());
}

// ===========================================================================
// render_export_text (mirrors Rust's render_export_text)
// ===========================================================================
// Mirrors Rust render_export_text(&Session) -> String
std::string render_export_text(const claw::runtime::Session& session) {
    std::vector<std::string> lines;
    lines.emplace_back("# Conversation Export");
    lines.emplace_back();

    for (std::size_t index = 0; index < session.messages.size(); ++index) {
        const auto& msg = session.messages[index];
        std::string_view role = (msg.role == claw::runtime::MessageRole::User)
            ? "user" : "assistant";
        lines.push_back(std::format("## {}. {}", index + 1, role));

        for (const auto& block : msg.blocks) {
            std::visit([&](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, claw::runtime::TextBlock>) {
                    lines.push_back(b.text);
                } else if constexpr (std::is_same_v<T, claw::runtime::ToolUseBlock>) {
                    lines.push_back(std::format("[tool_use id={} name={}] {}",
                                                b.id, b.name, b.input.dump()));
                } else if constexpr (std::is_same_v<T, claw::runtime::ToolResultBlock>) {
                    lines.push_back(std::format("[tool_result id={} error={}] {}",
                                                b.tool_use_id, b.is_error, b.content));
                } else if constexpr (std::is_same_v<T, claw::runtime::ThinkingBlock>) {
                    lines.push_back("[thinking] " + b.thinking);
                }
            }, block);
        }
        lines.emplace_back();
    }

    std::string result;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) result += '\n';
        result += lines[i];
    }
    return result;
}

// ===========================================================================
// slash_command_completion_candidates (mirrors Rust)
// ===========================================================================

std::vector<std::string> slash_command_completion_candidates() {
    return {
        "/help", "/status", "/compact", "/clear", "/clear --confirm",
        "/model", "/permissions", "/permissions read-only",
        "/permissions workspace-write", "/permissions danger-full-access",
        "/config", "/memory", "/cost", "/diff", "/commit", "/export",
        "/session", "/session list", "/session switch", "/session fork",
        "/resume", "/resume latest", "/init", "/mcp", "/agents", "/skills",
        "/sandbox", "/version", "/exit", "/quit",
    };
}

// ===========================================================================
// open_browser (mirrors Rust's open_browser)
// ===========================================================================

void open_browser(const std::string& url) {
    claw::runtime::BashCommandInput input;
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return;
#elif defined(__APPLE__)
    input.command = "open \"$CLAWD_URL\"";
#else
    input.command = "xdg-open \"$CLAWD_URL\"";
#endif
    input.extra_env.push_back("CLAWD_URL=" + url);
    (void)claw::runtime::execute_bash(input);
}

// ===========================================================================
// Action handlers
// ===========================================================================

void print_help() {
    std::cout
        << "Claw Code CLI (C++20 port)\n\n"
        << "USAGE:\n"
        << "  claw [OPTIONS] [SUBCOMMAND|PROMPT]\n\n"
        << "OPTIONS:\n"
        << "  --model <MODEL>              Model alias or full name [default: claude-opus-4-6]\n"
        << "  --permission-mode <MODE>     read-only | workspace-write | danger-full-access\n"
        << "  --output-format <FMT>        text | json | ndjson [default: text]\n"
        << "  --allowedTools <TOOLS>       Comma-separated list of allowed tool names\n"
        << "  --dangerously-skip-permissions  Alias for --permission-mode danger-full-access\n"
        << "  --resume [SESSION|latest]    Resume a saved session\n"
        << "  -p \"PROMPT\"                  One-shot prompt (non-interactive)\n"
        << "  --print                      Non-interactive output (alias for text format)\n"
        << "  --help, -h                   Show this help\n"
        << "  --version, -V                Print version\n\n"
        << "SUBCOMMANDS:\n"
        << "  login          Start the OAuth login flow\n"
        << "  logout         Clear saved OAuth credentials\n"
        << "  init           Initialise a project for Claw Code\n"
        << "  status         Print a status snapshot\n"
        << "  sandbox        Print sandbox status\n"
        << "  prompt <TEXT>  Run a non-interactive prompt and exit\n"
        << "  agents         List available agents\n"
        << "  mcp            MCP server management\n"
        << "  skills         List available skills\n"
        << "  system-prompt  Print the system prompt for a given directory\n\n"
        << "INTERACTIVE SLASH COMMANDS (start `claw` and type inside the REPL):\n"
        << "  " << render_repl_help() << "\n";
}

void print_version() {
    std::cout << render_version_report() << "\n";
}

void print_status_snapshot(const std::string& model, PermissionMode pm) {
    auto cwd = std::filesystem::current_path();
    auto ctx = build_status_context(std::nullopt);
    StatusUsage usage{};
    std::cout << format_status_report(model, usage,
                   permission_mode_label(pm), ctx) << "\n";
}

void print_sandbox_status_snapshot() {
    std::cout << format_sandbox_report("not-restricted") << "\n";
}

void run_login() {
    // Mirrors Rust run_login(): PKCE OAuth flow
    using namespace claw::runtime;

    constexpr uint16_t DEFAULT_OAUTH_CALLBACK_PORT = 7681;

    auto pkce = generate_pkce_pair();
    if (!pkce) {
        std::cerr << "error: " << pkce.error() << "\n";
        return;
    }
    auto state = generate_state();
    auto redirect_uri = loopback_redirect_uri(DEFAULT_OAUTH_CALLBACK_PORT);

    std::cout << "Starting Claude OAuth login...\n";
    std::cout << "Listening for callback on " << redirect_uri << "\n";

    // Build authorization URL (simplified — full impl reads config for client_id/authorize_url)
    auto authorize_url = std::format(
        "https://console.anthropic.com/oauth/authorize"
        "?response_type=code&code_challenge_method=S256"
        "&code_challenge={}&state={}&redirect_uri={}",
        pkce->code_challenge, state, redirect_uri);

    std::cout << "Open this URL to log in:\n" << authorize_url << "\n";
    open_browser(authorize_url);

    // Note: full callback listener requires HTTP server — print instructions
    std::cout << "\nAfter authorizing, paste the callback URL here (or set "
                 "ANTHROPIC_API_KEY environment variable):\n";
    std::string callback_url;
    if (std::getline(std::cin, callback_url) && !callback_url.empty()) {
        // Extract code from callback URL
        auto code_pos = callback_url.find("code=");
        if (code_pos != std::string::npos) {
            auto code_end = callback_url.find('&', code_pos);
            auto code = callback_url.substr(code_pos + 5,
                code_end != std::string::npos ? code_end - code_pos - 5 : std::string::npos);
            std::cout << "Authorization code received. Token exchange requires HTTP client.\n";
            std::cout << "For now, please set ANTHROPIC_API_KEY environment variable.\n";
        } else {
            std::cerr << "error: no authorization code found in callback URL\n";
        }
    }
}

void run_logout() {
    // Mirrors Rust run_logout(): clear saved OAuth credentials
    auto r = claw::runtime::clear_oauth_credentials();
    if (!r) {
        std::cerr << "error clearing credentials: " << r.error() << "\n";
        return;
    }
    std::cout << "Claude OAuth credentials cleared.\n";
}

void run_init() {
    auto cwd = std::filesystem::current_path();
    auto report = initialize_repo(cwd);
    std::cout << report.render() << "\n";
}

void run_repl_action(const std::string& model, PermissionMode pm) {
    SessionConfig cfg;
    cfg.model            = model;
    cfg.permission_mode  = pm;
    cfg.project_root     = std::filesystem::current_path();

    // Print startup banner (mirrors Rust's LiveCli::startup_banner).
    auto cwd = std::filesystem::current_path();
    auto git_branch = resolve_git_branch_for(cwd).value_or("unknown");
    auto git_status = run_git_capture_in(cwd, {"status", "--porcelain=v1", "-b"});
    auto ws = parse_git_workspace_summary(git_status);

    std::cout << "\x1b[38;5;196m"
                 " ██████╗██╗      █████╗ ██╗    ██╗\n"
                 "██╔════╝██║     ██╔══██╗██║    ██║\n"
                 "██║     ██║     ███████║██║ █╗ ██║\n"
                 "██║     ██║     ██╔══██║██║███╗██║\n"
                 "╚██████╗███████╗██║  ██║╚███╔███╔╝\n"
                 " ╚═════╝╚══════╝╚═╝  ╚═╝ ╚══╝╚══╝"
                 "\x1b[0m \x1b[38;5;208mCode\x1b[0m\n\n"
              << "  \x1b[2mModel\x1b[0m            " << model << "\n"
              << "  \x1b[2mPermissions\x1b[0m      " << permission_mode_label(pm) << "\n"
              << "  \x1b[2mBranch\x1b[0m           " << git_branch << "\n"
              << "  \x1b[2mWorkspace\x1b[0m        " << ws.headline() << "\n"
              << "  \x1b[2mDirectory\x1b[0m        " << cwd.string() << "\n\n"
              << "\n  Type \x1b[1m/help\x1b[0m for commands"
                 " · \x1b[1m/status\x1b[0m for live context"
                 " · \x1b[1m/diff\x1b[0m then \x1b[1m/commit\x1b[0m to ship\n";

    CliApp app{cfg};
    app.run_repl();
}

void run_prompt_action(const ActionPrompt& a) {
    SessionConfig cfg;
    cfg.model            = a.model;
    cfg.permission_mode  = a.permission_mode;
    cfg.output_format    = a.output_format;
    CliApp app{cfg};
    app.run_prompt(a.prompt, std::cout);
}

void resume_session_action(const std::filesystem::path& session_path,
                            const std::vector<std::string>& commands) {
    // Mirrors Rust resume_session_action: load session from JSONL file,
    // print resume report, then dispatch slash commands.
    std::string path_str = session_path.string();

    // Resolve "latest" to the most recent session file
    std::filesystem::path resolved = session_path;
    if (path_str == std::string(LATEST_SESSION_REF)) {
        // Look for sessions in .claw/sessions/
        auto cwd = std::filesystem::current_path();
        auto sessions_dir = cwd / ".claw" / "sessions";
        if (!std::filesystem::is_directory(sessions_dir)) {
            std::cerr << "No saved session found. Start `claw` and chat first.\n";
            return;
        }
        // Find the newest .jsonl file
        std::filesystem::path newest;
        std::filesystem::file_time_type newest_time{};
        for (auto& entry : std::filesystem::directory_iterator(sessions_dir)) {
            if (entry.path().extension() == ".jsonl") {
                auto t = entry.last_write_time();
                if (newest.empty() || t > newest_time) {
                    newest = entry.path();
                    newest_time = t;
                }
            }
        }
        if (newest.empty()) {
            std::cerr << "No saved session found. Start `claw` and chat first.\n";
            return;
        }
        resolved = newest;
    }

    if (!std::filesystem::exists(resolved)) {
        std::cerr << "Session file not found: " << resolved.string() << "\n";
        return;
    }

    // Load session
    auto session_r = claw::runtime::Session::load(resolved);
    if (!session_r) {
        std::cerr << "Failed to load session: " << session_r.error() << "\n";
        return;
    }

    std::cout << format_resume_report(
        resolved.string(), session_r->messages.size(), 0) << "\n";

    // Create CLI app with loaded session and enter REPL
    SessionConfig cfg;
    cfg.model = session_r->model.empty() ? std::string(DEFAULT_MODEL) : session_r->model;
    cfg.permission_mode = PermissionMode::DangerFullAccess;
    cfg.project_root = std::filesystem::current_path();
    CliApp app{cfg};
    app.load_session_from_path(resolved);

    // Dispatch slash commands if provided
    if (!commands.empty()) {
        for (auto& cmd : commands) {
            std::cout << "  > " << cmd << "\n";
            app.handle_submission(cmd, std::cout);
        }
    }

    // Enter REPL with restored history
    app.run_repl();
}

void dump_manifests() {
    // Mirrors Rust dump_manifests(): extract and print manifest counts
    auto cwd = std::filesystem::current_path();
    auto workspace_dir = cwd.parent_path(); // approximate: Rust uses CARGO_MANIFEST_DIR/../..
    auto paths = claw::UpstreamPaths::from_workspace_dir(workspace_dir);
    auto manifest = claw::extract_manifest(paths);
    if (!manifest) {
        std::cerr << "failed to extract manifests: "
                  << manifest.error().message() << "\n";
        std::exit(1);
    }
    std::cout << "commands: " << manifest->commands.entries().size() << "\n";
    std::cout << "tools: " << manifest->tools.entries().size() << "\n";
    std::cout << "bootstrap phases: " << manifest->bootstrap.phases().size() << "\n";
}

void print_bootstrap_plan() {
    // Mirrors Rust: iterate through default bootstrap plan phases
    auto plan = claw::BootstrapPlan::claude_code_default();
    auto phase_name = [](claw::BootstrapPhase p) -> std::string_view {
        switch (p) {
            case claw::BootstrapPhase::CliEntry:                  return "CliEntry";
            case claw::BootstrapPhase::FastPathVersion:           return "FastPathVersion";
            case claw::BootstrapPhase::StartupProfiler:           return "StartupProfiler";
            case claw::BootstrapPhase::SystemPromptFastPath:      return "SystemPromptFastPath";
            case claw::BootstrapPhase::ChromeMcpFastPath:         return "ChromeMcpFastPath";
            case claw::BootstrapPhase::DaemonWorkerFastPath:      return "DaemonWorkerFastPath";
            case claw::BootstrapPhase::BridgeFastPath:            return "BridgeFastPath";
            case claw::BootstrapPhase::DaemonFastPath:            return "DaemonFastPath";
            case claw::BootstrapPhase::BackgroundSessionFastPath: return "BackgroundSessionFastPath";
            case claw::BootstrapPhase::TemplateFastPath:          return "TemplateFastPath";
            case claw::BootstrapPhase::EnvironmentRunnerFastPath: return "EnvironmentRunnerFastPath";
            case claw::BootstrapPhase::MainRuntime:               return "MainRuntime";
        }
        return "Unknown";
    };
    for (auto p : plan.phases()) {
        std::cout << "- " << phase_name(p) << "\n";
    }
}

void print_system_prompt(const std::filesystem::path& cwd,
                          const std::string& date) {
    // Mirrors Rust print_system_prompt(): load and print system prompt sections
#ifdef _WIN32
    std::string os_name = "windows";
#elif defined(__APPLE__)
    std::string os_name = "macos";
#else
    std::string os_name = "linux";
#endif
    auto sections = claw::runtime::load_system_prompt(cwd, date, os_name, "unknown");
    if (!sections) {
        std::cerr << "failed to build system prompt: " << sections.error().message << "\n";
        std::exit(1);
    }
    for (std::size_t i = 0; i < sections->size(); ++i) {
        if (i) std::cout << "\n\n";
        std::cout << (*sections)[i];
    }
    std::cout << "\n";
}

void print_agents(const std::optional<std::string>& args) {
    // Mirrors Rust handle_agents_slash_command: discover agent definitions
    auto cwd = std::filesystem::current_path();
    if (args && (*args == "-h" || *args == "--help" || *args == "help")) {
        std::cout << "Usage: claw agents [list|help]\n"
                     "  list    List available agent definitions\n"
                     "  help    Show this help\n";
        return;
    }
    // Scan for agent definition files (.claw/agents/*.md)
    auto agents_dir = cwd / ".claw" / "agents";
    if (!std::filesystem::is_directory(agents_dir)) {
        std::cout << "Agents\n  No agent definitions found.\n"
                     "  Create agents in .claw/agents/*.md\n";
        return;
    }
    std::cout << "Agents\n";
    std::size_t count = 0;
    for (auto& entry : std::filesystem::directory_iterator(agents_dir)) {
        if (entry.path().extension() == ".md") {
            std::cout << "  " << entry.path().stem().string() << "\n";
            ++count;
        }
    }
    if (count == 0) std::cout << "  (none)\n";
    std::cout << "\n  " << count << " agent(s) found\n";
}

void print_mcp(const std::optional<std::string>& args) {
    // Mirrors Rust handle_mcp_slash_command: load MCP config and print summary
    auto cwd = std::filesystem::current_path();
    if (args && (*args == "-h" || *args == "--help" || *args == "help")) {
        std::cout << "Usage: claw mcp [list|show <server>|help]\n"
                     "  list           List configured MCP servers\n"
                     "  show <server>  Show details for a specific server\n"
                     "  help           Show this help\n";
        return;
    }
    // Load MCP config from .claw.json (or legacy .claude.json)
    auto config_path = cwd / ".claw.json";
    if (!std::filesystem::exists(config_path))
        config_path = cwd / ".claude.json";
    if (!std::filesystem::exists(config_path)) {
        std::cout << "MCP\n  No .claw.json found — no MCP servers configured.\n";
        return;
    }
    try {
        std::ifstream f(config_path);
        auto j = nlohmann::json::parse(f);
        if (j.contains("mcpServers") && j["mcpServers"].is_object()) {
            std::cout << "MCP servers\n";
            for (auto& [name, cfg] : j["mcpServers"].items()) {
                auto command = cfg.value("command", std::string{"<unknown>"});
                std::cout << "  " << name << "  (" << command << ")\n";
            }
        } else {
            std::cout << "MCP\n  No MCP servers configured.\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error reading MCP config: " << e.what() << "\n";
    }
}

void print_skills(const std::optional<std::string>& args) {
    // Mirrors Rust handle_skills_slash_command: discover skill definitions
    auto cwd = std::filesystem::current_path();
    std::optional<std::string_view> sv_args;
    if (args) sv_args = *args;
    auto result = claw::commands::handle_skills_slash_command(sv_args, cwd);
    if (result.has_value()) {
        std::cout << result.value() << "\n";
    } else {
        std::cerr << "error: " << result.error().message() << "\n";
    }
}

// ===========================================================================
// Doctor command  (mirrors Rust's run_doctor / DoctorReport)
// ===========================================================================

enum class DiagnosticLevel : std::uint8_t { Ok, Warn, Fail };

struct DiagnosticCheck {
    std::string name;
    DiagnosticLevel level{DiagnosticLevel::Ok};
    std::string summary;
    std::vector<std::string> details;
};

const char* diagnostic_level_label(DiagnosticLevel l) {
    switch (l) {
    case DiagnosticLevel::Ok:   return "OK";
    case DiagnosticLevel::Warn: return "WARN";
    case DiagnosticLevel::Fail: return "FAIL";
    }
    return "UNKNOWN";
}

std::string render_diagnostic_check(const DiagnosticCheck& check) {
    std::string out = check.name + "\n"
        "  Status           " + std::string(diagnostic_level_label(check.level)) + "\n"
        "  Summary          " + check.summary;
    if (!check.details.empty()) {
        out += "\n  Details";
        for (const auto& d : check.details)
            out += "\n    - " + d;
    }
    return out;
}

struct DoctorReport {
    std::vector<DiagnosticCheck> checks;

    [[nodiscard]] std::tuple<std::size_t, std::size_t, std::size_t> counts() const {
        std::size_t ok = 0, warn = 0, fail = 0;
        for (const auto& c : checks) {
            switch (c.level) {
            case DiagnosticLevel::Ok:   ++ok;   break;
            case DiagnosticLevel::Warn: ++warn; break;
            case DiagnosticLevel::Fail: ++fail; break;
            }
        }
        return {ok, warn, fail};
    }

    [[nodiscard]] bool has_failures() const {
        return std::any_of(checks.begin(), checks.end(),
            [](const auto& c){ return c.level == DiagnosticLevel::Fail; });
    }

    [[nodiscard]] std::string render() const {
        auto [ok_count, warn_count, fail_count] = counts();
        std::vector<std::string> lines = {
            "Doctor",
            std::format("Summary\n  OK               {}\n  Warnings         {}\n  Failures         {}",
                        ok_count, warn_count, fail_count),
        };
        for (const auto& c : checks)
            lines.push_back(render_diagnostic_check(c));
        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i) out += "\n\n";
            out += lines[i];
        }
        return out;
    }
};

DiagnosticCheck check_auth_health() {
    DiagnosticCheck check;
    check.name = "Authentication";
    if (const char* key = std::getenv("ANTHROPIC_API_KEY"); key && std::string_view(key).size() > 0) {
        check.level = DiagnosticLevel::Ok;
        check.summary = "ANTHROPIC_API_KEY is set";
    } else {
        check.level = DiagnosticLevel::Warn;
        check.summary = "ANTHROPIC_API_KEY is not set";
        check.details.push_back("Set ANTHROPIC_API_KEY to authenticate API calls");
    }
    return check;
}

DiagnosticCheck check_workspace_health() {
    DiagnosticCheck check;
    check.name = "Workspace";
    auto cwd = std::filesystem::current_path();
    auto git_root = find_git_root_in(cwd);
    if (git_root) {
        check.level = DiagnosticLevel::Ok;
        check.summary = "Git repository detected at " + git_root->string();
        auto branch = resolve_git_branch_for(cwd);
        if (branch) check.details.push_back("Branch: " + *branch);
    } else {
        check.level = DiagnosticLevel::Warn;
        check.summary = "Not inside a git repository";
        check.details.push_back("Git integration (diff, commit, PR) requires a git repository");
    }
    return check;
}

DiagnosticCheck check_config_health() {
    DiagnosticCheck check;
    check.name = "Configuration";
    auto cwd = std::filesystem::current_path();
    bool has_claw_json = std::filesystem::exists(cwd / ".claw.json");
    bool has_claude_json = std::filesystem::exists(cwd / ".claude.json");
    bool has_claw_dir = std::filesystem::is_directory(cwd / ".claw");
    bool has_claude_dir = std::filesystem::is_directory(cwd / ".claude");
    if (has_claw_json || has_claude_json || has_claw_dir || has_claude_dir) {
        check.level = DiagnosticLevel::Ok;
        check.summary = "Project configuration found";
        if (has_claw_json)  check.details.push_back(".claw.json present");
        if (has_claude_json && !has_claw_json)
            check.details.push_back(".claude.json present (legacy; consider migrating to .claw.json)");
        if (has_claw_dir)   check.details.push_back(".claw/ directory present");
        if (has_claude_dir && !has_claw_dir)
            check.details.push_back(".claude/ directory present (legacy; consider migrating to .claw/)");
    } else {
        check.level = DiagnosticLevel::Warn;
        check.summary = "No project configuration found";
        check.details.push_back("Run `claw init` to create a starter configuration");
    }
    return check;
}

DiagnosticCheck check_system_health() {
    DiagnosticCheck check;
    check.name = "System";
    check.level = DiagnosticLevel::Ok;
    check.summary = "C++20 port";
    check.details.push_back("Version: " + std::string(VERSION_STRING));
#ifdef _WIN32
    check.details.push_back("Platform: Windows");
#elif defined(__APPLE__)
    check.details.push_back("Platform: macOS");
#else
    check.details.push_back("Platform: Linux");
#endif
    return check;
}

DoctorReport render_doctor_report() {
    return DoctorReport{
        .checks = {
            check_auth_health(),
            check_config_health(),
            check_workspace_health(),
            check_system_health(),
        },
    };
}

void run_doctor() {
    auto report = render_doctor_report();
    std::cout << report.render() << "\n";
    if (report.has_failures())
        throw std::runtime_error("doctor found failing checks");
}

// ===========================================================================
// Context window error formatting  (mirrors Rust's format_context_window_blocked_error)
// ===========================================================================

std::string truncate_for_summary(std::string_view text, std::size_t max_len) {
    if (text.size() <= max_len) return std::string(text);
    return std::string(text.substr(0, max_len)) + "...";
}

std::string format_context_window_blocked_error(std::string_view session_id,
                                                 const claw::api::ApiError& error) {
    std::vector<std::string> lines = {
        "Context window blocked",
        "  Failure class    context_window_blocked",
        std::format("  Session          {}", session_id),
    };

    if (auto rid = error.request_id(); rid.has_value() && !rid->empty())
        lines.push_back(std::format("  Trace            {}", *rid));

    if (error.kind() == claw::api::ApiError::Kind::ContextWindowExceeded) {
        lines.push_back(std::format("  Model            {}", error.model()));
        lines.push_back(std::format("  Input estimate   ~{} tokens (heuristic)", error.estimated_input_tokens()));
        lines.push_back(std::format("  Requested output {} tokens", error.requested_output_tokens()));
        lines.push_back(std::format("  Total estimate   ~{} tokens (heuristic)", error.estimated_total_tokens()));
        lines.push_back(std::format("  Context window   {} tokens", error.context_window_tokens()));
    } else {
        auto detail = truncate_for_summary(error.what(), 120);
        if (!detail.empty())
            lines.push_back(std::format("  Detail           {}", detail));
    }

    lines.push_back("");
    lines.push_back("Recovery");
    lines.push_back("  Compact          /compact");
    lines.push_back(std::format("  New session      claw --resume {}", LATEST_SESSION_REF));

    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out;
}

std::string format_user_visible_api_error(std::string_view session_id,
                                           const claw::api::ApiError& error) {
    if (error.is_context_window_failure())
        return format_context_window_blocked_error(session_id, error);
    return std::string(error.what());
}

// ===========================================================================
// dispatch
// ===========================================================================

void dispatch(const CliAction& action) {
    std::visit([](auto&& a) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, ActionDumpManifests>)
            dump_manifests();
        else if constexpr (std::is_same_v<T, ActionBootstrapPlan>)
            print_bootstrap_plan();
        else if constexpr (std::is_same_v<T, ActionPrintSystemPrompt>)
            print_system_prompt(a.cwd, a.date);
        else if constexpr (std::is_same_v<T, ActionVersion>)
            print_version();
        else if constexpr (std::is_same_v<T, ActionHelp>)
            print_help();
        else if constexpr (std::is_same_v<T, ActionResumeSession>)
            resume_session_action(a.session_path, a.commands);
        else if constexpr (std::is_same_v<T, ActionStatus>)
            print_status_snapshot(a.model, a.permission_mode);
        else if constexpr (std::is_same_v<T, ActionSandbox>)
            print_sandbox_status_snapshot();
        else if constexpr (std::is_same_v<T, ActionPrompt>)
            run_prompt_action(a);
        else if constexpr (std::is_same_v<T, ActionLogin>)
            run_login();
        else if constexpr (std::is_same_v<T, ActionLogout>)
            run_logout();
        else if constexpr (std::is_same_v<T, ActionInit>)
            run_init();
        else if constexpr (std::is_same_v<T, ActionRepl>)
            run_repl_action(a.model, a.permission_mode);
            // a.allowed_tools is available but not yet wired into SessionConfig
        else if constexpr (std::is_same_v<T, ActionAgents>)
            print_agents(a.args);
        else if constexpr (std::is_same_v<T, ActionMcp>)
            print_mcp(a.args);
        else if constexpr (std::is_same_v<T, ActionSkills>)
            print_skills(a.args);
        else if constexpr (std::is_same_v<T, ActionDoctor>)
            run_doctor();
    }, action);
}

// ===========================================================================
// run
// ===========================================================================

void run(int argc, char* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    auto action = parse_args(args);
    dispatch(action);
}

} // anonymous namespace

void run_cli(int argc, char* argv[]) { run(argc, argv); }

} // namespace claw

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Enable UTF-8 output on Windows console so box-drawing characters
    // and Unicode symbols render correctly instead of garbled text.
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    // Enable virtual terminal processing for ANSI escape sequences.
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif
    try {
        claw::run_cli(argc, argv);
    } catch (const std::exception& ex) {
        std::string msg = ex.what();
        if (msg.find("`claw --help`") != std::string::npos) {
            std::cerr << "error: " << msg << "\n";
        } else {
            std::cerr << "error: " << msg << "\n\nRun `claw --help` for usage.\n";
        }
        return 1;
    }
    return 0;
}
