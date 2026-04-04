// tests/test_args.cpp -- C++20 port of args.rs #[cfg(test)] + cli_flags_and_config_defaults.rs
// Uses the catch2-style doctest header-only framework (or GoogleTest -- CMake wires it in).
// Written to be self-contained: no Catch2 / GTest dependency is assumed here;
// we use a minimal assert-based harness mirroring the Rust assert_eq! / assert! macros.

#include "../include/args.hpp"
#include "../include/app.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace claw;

// Minimal test harness.
static int g_passed = 0, g_failed = 0;
#define ASSERT_EQ(a, b) do { \
    if (!((a) == (b))) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #a " != " #b "\n"; \
        ++g_failed; \
    } else { ++g_passed; } \
} while(0)
#define ASSERT_TRUE(x) ASSERT_EQ(!!(x), true)
#define TEST(name) static void name(); \
    struct _Reg_##name { _Reg_##name(){ name(); } } _reg_##name; \
    static void name()

// ---- Tests mirroring args.rs unit tests ----

TEST(parses_requested_flags) {
    // Build a fake argv for parse_cli.
    std::vector<std::string> argv_strs = {
        "rusty-claude-cli",
        "--model", "claude-3-5-haiku",
        "--permission-mode", "read-only",
        "--config", "/tmp/config.toml",
        "--output-format", "ndjson",
    };
    std::vector<char*> argv_ptrs;
    for (auto& s : argv_strs) argv_ptrs.push_back(s.data());
    argv_ptrs.push_back(nullptr);

    try {
        auto cli = parse_cli(static_cast<int>(argv_ptrs.size() - 1), argv_ptrs.data());
        ASSERT_EQ(cli.model, "claude-3-5-haiku");
        ASSERT_EQ(cli.permission_mode, PermissionMode::ReadOnly);
        ASSERT_TRUE(cli.config.has_value());
        ASSERT_EQ(cli.config->string(), "/tmp/config.toml");
        ASSERT_EQ(cli.output_format, OutputFormat::Ndjson);
    } catch (std::exception& e) {
        std::cerr << "FAIL parses_requested_flags threw: " << e.what() << "\n";
        ++g_failed;
    }
}

TEST(parses_login_and_logout_subcommands) {
    {
        std::vector<std::string> argv_strs = {"claw", "login"};
        std::vector<char*> ptrs;
        for (auto& s : argv_strs) ptrs.push_back(s.data());
        ptrs.push_back(nullptr);
        try {
            auto cli = parse_cli(static_cast<int>(ptrs.size()-1), ptrs.data());
            ASSERT_EQ(cli.command.kind, SubcommandKind::Login);
        } catch (std::exception& e) {
            std::cerr << "FAIL login: " << e.what() << "\n"; ++g_failed;
        }
    }
    {
        std::vector<std::string> argv_strs = {"claw", "logout"};
        std::vector<char*> ptrs;
        for (auto& s : argv_strs) ptrs.push_back(s.data());
        ptrs.push_back(nullptr);
        try {
            auto cli = parse_cli(static_cast<int>(ptrs.size()-1), ptrs.data());
            ASSERT_EQ(cli.command.kind, SubcommandKind::Logout);
        } catch (std::exception& e) {
            std::cerr << "FAIL logout: " << e.what() << "\n"; ++g_failed;
        }
    }
}

TEST(defaults_to_danger_full_access) {
    std::vector<std::string> argv_strs = {"claw"};
    std::vector<char*> ptrs;
    for (auto& s : argv_strs) ptrs.push_back(s.data());
    ptrs.push_back(nullptr);
    try {
        auto cli = parse_cli(static_cast<int>(ptrs.size()-1), ptrs.data());
        ASSERT_EQ(cli.permission_mode, PermissionMode::DangerFullAccess);
    } catch (std::exception& e) {
        std::cerr << "FAIL defaults: " << e.what() << "\n"; ++g_failed;
    }
}

// ---- Tests mirroring app.rs unit tests ----

TEST(parses_required_slash_commands) {
    using namespace claw;
    ASSERT_TRUE(parse_slash_command("/help").has_value());
    ASSERT_TRUE(std::holds_alternative<SlashHelp>(*parse_slash_command("/help")));

    ASSERT_TRUE(std::holds_alternative<SlashStatus>(*parse_slash_command(" /status ")));

    auto compact = parse_slash_command("/compact now");
    ASSERT_TRUE(compact.has_value());
    ASSERT_TRUE(std::holds_alternative<SlashCompact>(*compact));

    auto model_cmd = parse_slash_command("/model claude-sonnet");
    ASSERT_TRUE(model_cmd.has_value());
    ASSERT_TRUE(std::holds_alternative<SlashModel>(*model_cmd));
    ASSERT_EQ(std::get<SlashModel>(*model_cmd).model, std::optional<std::string>{"claude-sonnet"});

    auto perms = parse_slash_command("/permissions workspace-write");
    ASSERT_TRUE(perms.has_value() && std::holds_alternative<SlashPermissions>(*perms));
    ASSERT_EQ(std::get<SlashPermissions>(*perms).mode, std::optional<std::string>{"workspace-write"});

    auto cfg = parse_slash_command("/config hooks");
    ASSERT_TRUE(cfg.has_value() && std::holds_alternative<SlashConfig>(*cfg));
    ASSERT_EQ(std::get<SlashConfig>(*cfg).section, std::optional<std::string>{"hooks"});

    ASSERT_TRUE(std::holds_alternative<SlashMemory>(*parse_slash_command("/memory")));

    auto clear = parse_slash_command("/clear --confirm");
    ASSERT_TRUE(clear.has_value() && std::holds_alternative<SlashClear>(*clear));
    ASSERT_EQ(std::get<SlashClear>(*clear).confirm, true);
}

TEST(help_output_lists_commands) {
    std::ostringstream oss;
    auto result = CliApp::handle_help(oss);
    ASSERT_EQ(result, CommandResult::Continue);
    auto out = oss.str();
    ASSERT_TRUE(out.find("/help") != std::string::npos);
    ASSERT_TRUE(out.find("/status") != std::string::npos);
    ASSERT_TRUE(out.find("/compact") != std::string::npos);
    ASSERT_TRUE(out.find("/model [model]") != std::string::npos);
    ASSERT_TRUE(out.find("/permissions [mode]") != std::string::npos);
    ASSERT_TRUE(out.find("/config [section]") != std::string::npos);
    ASSERT_TRUE(out.find("/memory") != std::string::npos);
    ASSERT_TRUE(out.find("/clear [--confirm]") != std::string::npos);
}

TEST(session_state_tracks_config_values) {
    SessionConfig config;
    config.model = "claude";
    config.permission_mode = PermissionMode::DangerFullAccess;
    config.config = std::filesystem::path("settings.toml");
    config.output_format = OutputFormat::Text;

    ASSERT_EQ(config.model, "claude");
    ASSERT_EQ(config.permission_mode, PermissionMode::DangerFullAccess);
    ASSERT_TRUE(config.config.has_value());
    ASSERT_EQ(config.config->string(), "settings.toml");
}

// ---- main ----

int main() {
    // Tests run via static constructors above.
    std::cout << "\nTest results: " << g_passed << " passed, " << g_failed << " failed.\n";
    return (g_failed > 0) ? 1 : 0;
}