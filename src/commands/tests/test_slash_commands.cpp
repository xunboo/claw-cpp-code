// C++20 unit tests for the commands library.
// Mirrors the Rust #[cfg(test)] mod tests block from lib.rs.
//
// Uses Google Test.  Build with -DCOMMANDS_BUILD_TESTS=ON.

#include "commands.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

using namespace commands;
using namespace commands::slash_command;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template <typename T>
static SlashCommand parse_ok(std::string_view input) {
    auto result = parse_slash_command(input);
    EXPECT_FALSE(result.is_error()) << (result.is_error() ? result.error().what() : "");
    EXPECT_TRUE(result.value().has_value());
    EXPECT_TRUE(std::holds_alternative<T>(*result.value()))
        << "Expected variant alternative " << typeid(T).name();
    return *result.value();
}

static std::string parse_error_message(std::string_view input) {
    auto result = parse_slash_command(input);
    EXPECT_TRUE(result.is_error())
        << "Expected parse error for: " << input;
    if (result.is_error()) return result.error().message();
    return {};
}

// ---------------------------------------------------------------------------
// Test: parses_supported_slash_commands
// ---------------------------------------------------------------------------

TEST(SlashCommands, ParsesNoArgCommands) {
    EXPECT_TRUE(std::holds_alternative<Help>(*parse_slash_command("/help").value()));
    EXPECT_TRUE(std::holds_alternative<Status>(*parse_slash_command(" /status ").value()));
    EXPECT_TRUE(std::holds_alternative<Sandbox>(*parse_slash_command("/sandbox").value()));
    EXPECT_TRUE(std::holds_alternative<Compact>(*parse_slash_command("/compact").value()));
    EXPECT_TRUE(std::holds_alternative<Commit>(*parse_slash_command("/commit").value()));
    EXPECT_TRUE(std::holds_alternative<DebugToolCall>(*parse_slash_command("/debug-tool-call").value()));
    EXPECT_TRUE(std::holds_alternative<Cost>(*parse_slash_command("/cost").value()));
    EXPECT_TRUE(std::holds_alternative<Memory>(*parse_slash_command("/memory").value()));
    EXPECT_TRUE(std::holds_alternative<Init>(*parse_slash_command("/init").value()));
    EXPECT_TRUE(std::holds_alternative<Diff>(*parse_slash_command("/diff").value()));
    EXPECT_TRUE(std::holds_alternative<Version>(*parse_slash_command("/version").value()));
}

TEST(SlashCommands, ParsesBughunterWithOptionalScope) {
    {
        auto cmd = parse_ok<Bughunter>("/bughunter runtime");
        EXPECT_EQ(std::get<Bughunter>(cmd).scope, std::optional<std::string>("runtime"));
    }
    {
        auto cmd = parse_ok<Bughunter>("/bughunter");
        EXPECT_FALSE(std::get<Bughunter>(cmd).scope.has_value());
    }
}

TEST(SlashCommands, ParsesPrAndIssue) {
    {
        auto cmd = parse_ok<Pr>("/pr ready for review");
        EXPECT_EQ(std::get<Pr>(cmd).context, std::optional<std::string>("ready for review"));
    }
    {
        auto cmd = parse_ok<Issue>("/issue flaky test");
        EXPECT_EQ(std::get<Issue>(cmd).context, std::optional<std::string>("flaky test"));
    }
}

TEST(SlashCommands, ParsesUltraplanAndTeleport) {
    {
        auto cmd = parse_ok<Ultraplan>("/ultraplan ship both features");
        EXPECT_EQ(std::get<Ultraplan>(cmd).task,
                  std::optional<std::string>("ship both features"));
    }
    {
        auto cmd = parse_ok<Teleport>("/teleport conversation.rs");
        EXPECT_EQ(std::get<Teleport>(cmd).target,
                  std::optional<std::string>("conversation.rs"));
    }
}

TEST(SlashCommands, ParsesModel) {
    {
        auto cmd = parse_ok<Model>("/model claude-opus");
        EXPECT_EQ(std::get<Model>(cmd).model,
                  std::optional<std::string>("claude-opus"));
    }
    {
        auto cmd = parse_ok<Model>("/model");
        EXPECT_FALSE(std::get<Model>(cmd).model.has_value());
    }
}

TEST(SlashCommands, ParsesPermissions) {
    auto cmd = parse_ok<Permissions>("/permissions read-only");
    EXPECT_EQ(std::get<Permissions>(cmd).mode,
              std::optional<std::string>("read-only"));
}

TEST(SlashCommands, ParsesClear) {
    {
        auto cmd = parse_ok<Clear>("/clear");
        EXPECT_FALSE(std::get<Clear>(cmd).confirm);
    }
    {
        auto cmd = parse_ok<Clear>("/clear --confirm");
        EXPECT_TRUE(std::get<Clear>(cmd).confirm);
    }
}

TEST(SlashCommands, ParsesResume) {
    auto cmd = parse_ok<Resume>("/resume session.json");
    EXPECT_EQ(std::get<Resume>(cmd).session_path,
              std::optional<std::string>("session.json"));
}

TEST(SlashCommands, ParsesConfig) {
    {
        auto cmd = parse_ok<Config>("/config");
        EXPECT_FALSE(std::get<Config>(cmd).section.has_value());
    }
    {
        auto cmd = parse_ok<Config>("/config env");
        EXPECT_EQ(std::get<Config>(cmd).section, std::optional<std::string>("env"));
    }
}

TEST(SlashCommands, ParsesMcp) {
    {
        auto cmd = parse_ok<Mcp>("/mcp");
        EXPECT_FALSE(std::get<Mcp>(cmd).action.has_value());
        EXPECT_FALSE(std::get<Mcp>(cmd).target.has_value());
    }
    {
        auto cmd = parse_ok<Mcp>("/mcp show remote");
        EXPECT_EQ(std::get<Mcp>(cmd).action, std::optional<std::string>("show"));
        EXPECT_EQ(std::get<Mcp>(cmd).target, std::optional<std::string>("remote"));
    }
}

TEST(SlashCommands, ParsesExport) {
    auto cmd = parse_ok<Export>("/export notes.txt");
    EXPECT_EQ(std::get<Export>(cmd).path, std::optional<std::string>("notes.txt"));
}

TEST(SlashCommands, ParsesSession) {
    {
        auto cmd = parse_ok<Session>("/session switch abc123");
        EXPECT_EQ(std::get<Session>(cmd).action, std::optional<std::string>("switch"));
        EXPECT_EQ(std::get<Session>(cmd).target, std::optional<std::string>("abc123"));
    }
    {
        auto cmd = parse_ok<Session>("/session fork incident-review");
        EXPECT_EQ(std::get<Session>(cmd).action, std::optional<std::string>("fork"));
        EXPECT_EQ(std::get<Session>(cmd).target,
                  std::optional<std::string>("incident-review"));
    }
}

TEST(SlashCommands, ParsesPlugins) {
    {
        auto cmd = parse_ok<Plugins>("/plugins install demo");
        EXPECT_EQ(std::get<Plugins>(cmd).action, std::optional<std::string>("install"));
        EXPECT_EQ(std::get<Plugins>(cmd).target, std::optional<std::string>("demo"));
    }
    {
        auto cmd = parse_ok<Plugins>("/plugins list");
        EXPECT_EQ(std::get<Plugins>(cmd).action, std::optional<std::string>("list"));
        EXPECT_FALSE(std::get<Plugins>(cmd).target.has_value());
    }
    {
        auto cmd = parse_ok<Plugins>("/plugins enable demo");
        EXPECT_EQ(std::get<Plugins>(cmd).action, std::optional<std::string>("enable"));
        EXPECT_EQ(std::get<Plugins>(cmd).target, std::optional<std::string>("demo"));
    }
    {
        auto cmd = parse_ok<Plugins>("/plugins disable demo");
        EXPECT_EQ(std::get<Plugins>(cmd).action, std::optional<std::string>("disable"));
        EXPECT_EQ(std::get<Plugins>(cmd).target, std::optional<std::string>("demo"));
    }
    // plugin is an alias for plugins
    {
        auto cmd = parse_ok<Plugins>("/plugin list");
        EXPECT_EQ(std::get<Plugins>(cmd).action, std::optional<std::string>("list"));
    }
    // marketplace is also an alias
    {
        auto cmd = parse_ok<Plugins>("/marketplace list");
        EXPECT_EQ(std::get<Plugins>(cmd).action, std::optional<std::string>("list"));
    }
}

TEST(SlashCommands, ParsesSkillsInstall) {
    auto cmd = parse_ok<Skills>("/skills install ./fixtures/help-skill");
    EXPECT_EQ(std::get<Skills>(cmd).args,
              std::optional<std::string>("install ./fixtures/help-skill"));
}

// ---------------------------------------------------------------------------
// Test: rejects_unexpected_arguments_for_no_arg_commands
// ---------------------------------------------------------------------------

TEST(SlashCommands, RejectsUnexpectedArgsForNoArgCommands) {
    std::string error = parse_error_message("/compact now");
    EXPECT_TRUE(error.find("Unexpected arguments for /compact.") != std::string::npos);
    EXPECT_TRUE(error.find("Usage            /compact") != std::string::npos);
    EXPECT_TRUE(error.find("Summary          Compact local session history") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test: rejects_invalid_argument_values
// ---------------------------------------------------------------------------

TEST(SlashCommands, RejectsInvalidPermissionsMode) {
    std::string error = parse_error_message("/permissions admin");
    EXPECT_TRUE(error.find(
        "Unsupported /permissions mode 'admin'. Use read-only, workspace-write, or danger-full-access.")
        != std::string::npos);
    EXPECT_TRUE(error.find(
        "Usage            /permissions [read-only|workspace-write|danger-full-access]")
        != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test: rejects_missing_required_arguments
// ---------------------------------------------------------------------------

TEST(SlashCommands, RejectsMissingRequiredArguments) {
    std::string error = parse_error_message("/teleport");
    EXPECT_TRUE(error.find("Usage: /teleport <symbol-or-path>") != std::string::npos);
    EXPECT_TRUE(error.find("Category         Discovery & debugging") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test: rejects_invalid_session_and_plugin_shapes
// ---------------------------------------------------------------------------

TEST(SlashCommands, RejectsInvalidSessionAndPluginShapes) {
    {
        std::string error = parse_error_message("/session switch");
        EXPECT_TRUE(error.find("Usage: /session switch <session-id>") != std::string::npos);
        EXPECT_TRUE(error.find("/session") != std::string::npos);
    }
    {
        std::string error = parse_error_message("/plugins list extra");
        EXPECT_TRUE(error.find("Usage: /plugin list") != std::string::npos);
        EXPECT_TRUE(error.find("Aliases          /plugins, /marketplace") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Test: rejects_invalid_agents_and_skills_arguments
// ---------------------------------------------------------------------------

TEST(SlashCommands, RejectsInvalidAgentsAndSkillsArguments) {
    {
        std::string error = parse_error_message("/agents show planner");
        EXPECT_TRUE(error.find(
            "Unexpected arguments for /agents: show planner. "
            "Use /agents, /agents list, or /agents help.") != std::string::npos);
        EXPECT_TRUE(error.find("Usage            /agents [list|help]") != std::string::npos);
    }
    {
        std::string error = parse_error_message("/skills show help");
        EXPECT_TRUE(error.find(
            "Unexpected arguments for /skills: show help. "
            "Use /skills, /skills list, /skills install <path>, or /skills help.")
            != std::string::npos);
        EXPECT_TRUE(error.find("Usage            /skills [list|install <path>|help]")
            != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Test: rejects_invalid_mcp_arguments
// ---------------------------------------------------------------------------

TEST(SlashCommands, RejectsInvalidMcpArguments) {
    {
        std::string error = parse_error_message("/mcp show alpha beta");
        EXPECT_TRUE(error.find("Unexpected arguments for /mcp show.") != std::string::npos);
        EXPECT_TRUE(error.find("Usage            /mcp show <server>") != std::string::npos);
    }
    {
        std::string error = parse_error_message("/mcp inspect alpha");
        EXPECT_TRUE(error.find(
            "Unknown /mcp action 'inspect'. Use list, show <server>, or help.")
            != std::string::npos);
        EXPECT_TRUE(error.find("Usage            /mcp [list|show <server>|help]")
            != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Test: renders_help_from_shared_specs
// ---------------------------------------------------------------------------

TEST(SlashCommands, RendersHelpFromSharedSpecs) {
    std::string help = render_slash_command_help();
    EXPECT_TRUE(help.find("Start here        /status, /diff, /agents, /skills, /commit")
                != std::string::npos);
    EXPECT_TRUE(help.find("[resume]          also works with --resume SESSION.jsonl")
                != std::string::npos);
    EXPECT_TRUE(help.find("Session & visibility")  != std::string::npos);
    EXPECT_TRUE(help.find("Workspace & git")       != std::string::npos);
    EXPECT_TRUE(help.find("Discovery & debugging") != std::string::npos);
    EXPECT_TRUE(help.find("Analysis & automation") != std::string::npos);
    EXPECT_TRUE(help.find("/help")      != std::string::npos);
    EXPECT_TRUE(help.find("/status")    != std::string::npos);
    EXPECT_TRUE(help.find("/sandbox")   != std::string::npos);
    EXPECT_TRUE(help.find("/compact")   != std::string::npos);
    EXPECT_TRUE(help.find("/bughunter [scope]") != std::string::npos);
    EXPECT_TRUE(help.find("/commit")    != std::string::npos);
    EXPECT_TRUE(help.find("/pr [context]")  != std::string::npos);
    EXPECT_TRUE(help.find("/issue [context]") != std::string::npos);
    EXPECT_TRUE(help.find("/ultraplan [task]") != std::string::npos);
    EXPECT_TRUE(help.find("/teleport <symbol-or-path>") != std::string::npos);
    EXPECT_TRUE(help.find("/debug-tool-call") != std::string::npos);
    EXPECT_TRUE(help.find("/model [model]") != std::string::npos);
    EXPECT_TRUE(help.find("/permissions [read-only|workspace-write|danger-full-access]")
                != std::string::npos);
    EXPECT_TRUE(help.find("/clear [--confirm]")  != std::string::npos);
    EXPECT_TRUE(help.find("/cost")      != std::string::npos);
    EXPECT_TRUE(help.find("/resume <session-path>") != std::string::npos);
    EXPECT_TRUE(help.find("/config [env|hooks|model|plugins]") != std::string::npos);
    EXPECT_TRUE(help.find("/mcp [list|show <server>|help]") != std::string::npos);
    EXPECT_TRUE(help.find("/memory")    != std::string::npos);
    EXPECT_TRUE(help.find("/init")      != std::string::npos);
    EXPECT_TRUE(help.find("/diff")      != std::string::npos);
    EXPECT_TRUE(help.find("/version")   != std::string::npos);
    EXPECT_TRUE(help.find("/export [file]") != std::string::npos);
    EXPECT_TRUE(help.find("/session [list|switch <session-id>|fork [branch-name]]")
                != std::string::npos);
    EXPECT_TRUE(help.find(
        "/plugin [list|install <path>|enable <name>|disable <name>|uninstall <id>|update <id>]")
        != std::string::npos);
    EXPECT_TRUE(help.find("aliases: /plugins, /marketplace") != std::string::npos);
    EXPECT_TRUE(help.find("/agents [list|help]") != std::string::npos);
    EXPECT_TRUE(help.find("/skills [list|install <path>|help]") != std::string::npos);

    // Total spec count
    EXPECT_EQ(slash_command_specs().size(), 141u);
    EXPECT_GE(resume_supported_slash_commands().size(), 39u);
}

// ---------------------------------------------------------------------------
// Test: renders_per_command_help_detail
// ---------------------------------------------------------------------------

TEST(SlashCommands, RendersPerCommandHelpDetail) {
    auto help = render_slash_command_help_detail("plugins");
    ASSERT_TRUE(help.has_value());
    EXPECT_TRUE(help->find("/plugin") != std::string::npos);
    EXPECT_TRUE(help->find("Summary          Manage Claw Code plugins") != std::string::npos);
    EXPECT_TRUE(help->find("Aliases          /plugins, /marketplace") != std::string::npos);
    EXPECT_TRUE(help->find("Category         Workspace & git") != std::string::npos);
}

TEST(SlashCommands, RendersPerCommandHelpDetailForMcp) {
    auto help = render_slash_command_help_detail("mcp");
    ASSERT_TRUE(help.has_value());
    EXPECT_TRUE(help->find("/mcp") != std::string::npos);
    EXPECT_TRUE(help->find("Summary          Inspect configured MCP servers") != std::string::npos);
    EXPECT_TRUE(help->find("Category         Discovery & debugging") != std::string::npos);
    EXPECT_TRUE(help->find("Resume           Supported with --resume SESSION.jsonl")
                != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test: validate_slash_command_input_rejects_extra_single_value_arguments
// ---------------------------------------------------------------------------

TEST(SlashCommands, RejectsExtraSingleValueArguments) {
    {
        std::string error = parse_error_message("/session switch current next");
        EXPECT_TRUE(error.find("Unexpected arguments for /session switch.") != std::string::npos);
        EXPECT_TRUE(error.find("Usage            /session switch <session-id>") != std::string::npos);
    }
    {
        std::string error = parse_error_message("/plugin enable demo extra");
        EXPECT_TRUE(error.find("Unexpected arguments for /plugin enable.") != std::string::npos);
        EXPECT_TRUE(error.find("Usage            /plugin enable <name>") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Test: suggests_closest_slash_commands_for_typos_and_aliases
// ---------------------------------------------------------------------------

TEST(SlashCommands, SuggestsClosestSlashCommandsForTypos) {
    auto suggestions = suggest_slash_commands("stats", 3);
    bool has_stats  = std::find(suggestions.begin(), suggestions.end(), "/stats")  != suggestions.end();
    bool has_status = std::find(suggestions.begin(), suggestions.end(), "/status") != suggestions.end();
    EXPECT_TRUE(has_stats);
    EXPECT_TRUE(has_status);
    EXPECT_LE(suggestions.size(), 3u);

    auto plugin_suggestions = suggest_slash_commands("/plugns", 3);
    EXPECT_TRUE(std::find(plugin_suggestions.begin(), plugin_suggestions.end(), "/plugin")
                != plugin_suggestions.end());

    EXPECT_TRUE(suggest_slash_commands("zzz", 3).empty());
}

// ---------------------------------------------------------------------------
// Test: non-slash input is parsed as nullopt (not an error)
// ---------------------------------------------------------------------------

TEST(SlashCommands, NonSlashInputIsNullopt) {
    auto result = parse_slash_command("hello world");
    EXPECT_FALSE(result.is_error());
    EXPECT_FALSE(result.value().has_value());
}

// ---------------------------------------------------------------------------
// Test: empty slash is rejected with a message
// ---------------------------------------------------------------------------

TEST(SlashCommands, EmptySlashIsRejected) {
    auto result = parse_slash_command("/");
    EXPECT_TRUE(result.is_error());
    EXPECT_TRUE(std::string(result.error().what()).find("Slash command name is missing")
                != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test: unknown commands become Unknown variant
// ---------------------------------------------------------------------------

TEST(SlashCommands, UnknownCommandBecomesUnknownVariant) {
    auto cmd = parse_ok<Unknown>("/xyzzy-not-a-real-command");
    EXPECT_EQ(std::get<Unknown>(cmd).name, "xyzzy-not-a-real-command");
}
