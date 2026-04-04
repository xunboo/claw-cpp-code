// args.cpp -- C++20 port of args.rs
// CLI argument definitions and parser.
#include "args.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace claw {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

} // anonymous namespace

// ---- Free helpers ----

std::optional<PermissionMode> permission_mode_from_str(std::string_view s) noexcept {
    if (s == "read-only")          return PermissionMode::ReadOnly;
    if (s == "workspace-write")    return PermissionMode::WorkspaceWrite;
    if (s == "danger-full-access") return PermissionMode::DangerFullAccess;
    return std::nullopt;
}

std::optional<OutputFormat> output_format_from_str(std::string_view s) noexcept {
    if (s == "text")   return OutputFormat::Text;
    if (s == "json")   return OutputFormat::Json;
    if (s == "ndjson") return OutputFormat::Ndjson;
    return std::nullopt;
}

// ---- parse_cli ----
// Minimal argc/argv parser used by the args.hpp unit.  The full production
// parser lives in main.cpp::parse_args().

Cli parse_cli(int argc, char* argv[]) {
    Cli cli;

    int i = 1; // skip program name
    while (i < argc) {
        std::string_view arg = argv[i];

        auto consume_next = [&](std::string_view flag_name) -> std::string_view {
            if (i + 1 >= argc)
                throw std::runtime_error("missing value for " + std::string(flag_name));
            return argv[++i];
        };

        auto try_strip = [](std::string_view a, std::string_view prefix)
            -> std::optional<std::string_view> {
            if (a.size() > prefix.size() && a.substr(0, prefix.size()) == prefix)
                return a.substr(prefix.size());
            return std::nullopt;
        };

        if (arg == "--help" || arg == "-h") {
            // Recognised but not acted on here; callers handle it.
            ++i;
        } else if (arg == "--version" || arg == "-V") {
            ++i;
        } else if (arg == "--model") {
            cli.model = std::string(consume_next("--model"));
            ++i;
        } else if (auto v = try_strip(arg, "--model=")) {
            cli.model = std::string(*v);
            ++i;
        } else if (arg == "--permission-mode") {
            auto val = consume_next("--permission-mode");
            auto mode = permission_mode_from_str(val);
            if (!mode)
                throw std::runtime_error(
                    "unknown permission mode '" + std::string(val) +
                    "'. Use read-only, workspace-write, or danger-full-access.");
            cli.permission_mode = *mode;
            ++i;
        } else if (auto v = try_strip(arg, "--permission-mode=")) {
            auto mode = permission_mode_from_str(*v);
            if (!mode)
                throw std::runtime_error(
                    "unknown permission mode '" + std::string(*v) + "'.");
            cli.permission_mode = *mode;
            ++i;
        } else if (arg == "--config") {
            cli.config = std::filesystem::path(std::string(consume_next("--config")));
            ++i;
        } else if (auto v = try_strip(arg, "--config=")) {
            cli.config = std::filesystem::path(std::string(*v));
            ++i;
        } else if (arg == "--output-format") {
            auto val = consume_next("--output-format");
            auto fmt = output_format_from_str(val);
            if (!fmt)
                throw std::runtime_error(
                    "unsupported value for --output-format: " + std::string(val) +
                    " (expected text, json, or ndjson)");
            cli.output_format = *fmt;
            ++i;
        } else if (auto v = try_strip(arg, "--output-format=")) {
            auto fmt = output_format_from_str(*v);
            if (!fmt)
                throw std::runtime_error(
                    "unsupported value for --output-format: " + std::string(*v));
            cli.output_format = *fmt;
            ++i;
        } else if (arg == "dump-manifests") {
            cli.command.kind = SubcommandKind::DumpManifests;
            ++i;
        } else if (arg == "bootstrap-plan") {
            cli.command.kind = SubcommandKind::BootstrapPlan;
            ++i;
        } else if (arg == "login") {
            cli.command.kind = SubcommandKind::Login;
            ++i;
        } else if (arg == "logout") {
            cli.command.kind = SubcommandKind::Logout;
            ++i;
        } else if (arg == "prompt") {
            cli.command.kind = SubcommandKind::Prompt;
            ++i;
            while (i < argc)
                cli.command.prompt_words.emplace_back(argv[i++]);
        } else {
            throw std::runtime_error(
                "unknown argument: " + std::string(arg) +
                "\nRun `claw --help` for usage.");
        }
    }

    return cli;
}

} // namespace claw
