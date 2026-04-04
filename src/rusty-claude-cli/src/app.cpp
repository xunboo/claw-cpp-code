// app.cpp -- C++20 port of app.rs
// CliApp, SessionConfig, SessionState, SlashCommand, ConversationClient.
#include "app.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

// API client for real Anthropic calls
#include "providers/anthropic.hpp"
#include "types.hpp"

namespace claw {

// ===========================================================================
// parse_slash_command  (mirrors Rust's SlashCommand::parse)
// ===========================================================================

std::optional<SlashCommand> parse_slash_command(std::string_view input) {
    // Trim leading whitespace.
    while (!input.empty() && input.front() == ' ') input.remove_prefix(1);
    if (input.empty() || input[0] != '/') return std::nullopt;

    // Tokenise the body after the leading '/'.
    std::string body{input.substr(1)};
    std::istringstream ss(body);
    std::string cmd;
    ss >> cmd;

    if (cmd == "help")        return SlashHelp{};
    if (cmd == "status")      return SlashStatus{};
    if (cmd == "compact")     return SlashCompact{};
    if (cmd == "model") {
        std::string arg;
        if (ss >> arg) return SlashModel{std::move(arg)};
        return SlashModel{};
    }
    if (cmd == "permissions") {
        std::string arg;
        if (ss >> arg) return SlashPermissions{std::move(arg)};
        return SlashPermissions{};
    }
    if (cmd == "config") {
        std::string arg;
        if (ss >> arg) return SlashConfig{std::move(arg)};
        return SlashConfig{};
    }
    if (cmd == "memory") return SlashMemory{};
    if (cmd == "clear") {
        std::string flag;
        bool confirm = (ss >> flag && flag == "--confirm");
        return SlashClear{confirm};
    }
    return SlashUnknown{cmd};
}

// ===========================================================================
// ConversationClient — real Anthropic API implementation
// ===========================================================================

ConversationClient::ConversationClient(std::string model)
    : model_{std::move(model)}
{
    // Read API key from environment
    if (auto* key = std::getenv("ANTHROPIC_API_KEY")) {
        api_key_ = key;
    }
    // Read base URL from environment (or use default)
    if (auto* url = std::getenv("ANTHROPIC_BASE_URL")) {
        base_url_ = url;
    } else {
        base_url_ = "https://api.anthropic.com";
    }
}

ConversationClient ConversationClient::from_env(std::string model) {
    return ConversationClient{std::move(model)};
}

TurnSummary ConversationClient::run_turn(
    std::vector<ConversationMessage>& history,
    std::string_view user_input,
    std::function<void(StreamEvent)> event_callback)
{
    using namespace claw::api;

    // Push user message to history
    history.push_back({"user", std::string{user_input}});

    // Build InputMessages from conversation history
    std::vector<InputMessage> api_messages;
    for (const auto& msg : history) {
        InputMessage im;
        im.role = msg.role;
        im.content.push_back(InputContentBlock::text_block(msg.text));
        api_messages.push_back(std::move(im));
    }

    // Build request — mirrors Rust's build_message_request
    MessageRequest request;
    request.model      = model_;
    request.max_tokens  = 8192;
    request.messages    = std::move(api_messages);
    request.stream      = false;

    // Check for API key
    if (api_key_.empty()) {
        throw std::runtime_error(
            "ANTHROPIC_API_KEY not set. Run `claw login` or set the environment variable.");
    }

    // Create AnthropicClient and send request
    auto client = AnthropicClient(api_key_);
    if (!base_url_.empty()) {
        client = std::move(client).with_base_url(base_url_);
    }

    auto future = client.send_message(request);
    MessageResponse response;
    try {
        response = future.get();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("API request failed: ") + e.what());
    }

    // Extract text from response content blocks
    std::string assistant_text;
    for (const auto& block : response.content) {
        switch (block.kind) {
            case OutputContentBlock::Kind::Text:
                if (!assistant_text.empty()) assistant_text += "\n";
                assistant_text += block.text;
                event_callback(TextDelta{block.text});
                break;
            case OutputContentBlock::Kind::ToolUse:
                event_callback(ToolCallStart{block.name, block.input.dump()});
                // Tool execution: in a full implementation, run the tool and feed
                // the result back in a follow-up API call (agentic loop).
                event_callback(ToolCallResult{block.name,
                    "Tool execution requires the agentic loop (not yet wired to tool_executor)",
                    false});
                break;
            case OutputContentBlock::Kind::Thinking:
                // Thinking blocks are internal; emit as text delta for visibility
                if (!block.text.empty())
                    event_callback(TextDelta{"[thinking] " + block.text + "\n"});
                break;
            default:
                break;
        }
    }

    // Report usage
    UsageSummary usage{response.usage.input_tokens, response.usage.output_tokens};
    event_callback(UsageEvent{usage});

    // Push assistant response to history
    history.push_back({"assistant", assistant_text});

    return TurnSummary{assistant_text, usage};
}

// ===========================================================================
// CliApp -- slash-command table
// ===========================================================================

namespace {

struct CommandEntry {
    std::string_view name;
    std::string_view summary;
};

// Mirrors Rust's SLASH_COMMAND_HANDLERS array.
constexpr CommandEntry SLASH_COMMANDS[] = {
    {"/help",               "Show command help"},
    {"/status",             "Show current session status"},
    {"/compact",            "Compact local session history"},
    {"/model [model]",      "Show or switch the active model"},
    {"/permissions [mode]", "Show or switch the active permission mode"},
    {"/config [section]",   "Inspect current config path or section"},
    {"/memory",             "Inspect loaded memory/instruction files"},
    {"/clear [--confirm]",  "Start a fresh local session"},
};

} // anonymous namespace

// ===========================================================================
// CliApp constructor
// ===========================================================================

CliApp::CliApp(SessionConfig config)
    : config_{std::move(config)}
    , renderer_{}
    , state_{config_.model}
    , client_{ConversationClient::from_env(config_.model)}
{}

// ===========================================================================
// run_repl  (mirrors Rust's run_repl)
// ===========================================================================

void CliApp::run_repl() {
    LineEditor editor{"› "};
    std::cout << "Rusty Claude CLI interactive mode\n";
    std::cout << "Type /help for commands. Shift+Enter or Ctrl+J inserts a newline.\n";

    while (true) {
        auto outcome = editor.read_line();
        if (std::holds_alternative<ReadExit>(outcome)) break;
        if (std::holds_alternative<ReadCancel>(outcome)) continue;

        auto& submit = std::get<ReadSubmit>(outcome);
        if (submit.text.empty()) continue;

        editor.push_history(submit.text);
        handle_submission(submit.text, std::cout);
    }
}

// ===========================================================================
// run_prompt  (mirrors Rust's run_prompt)
// ===========================================================================

void CliApp::run_prompt(std::string_view prompt, std::ostream& out) {
    render_response(prompt, out);
}

// ===========================================================================
// handle_submission  (mirrors Rust's handle_submission)
// ===========================================================================

CommandResult CliApp::handle_submission(std::string_view input, std::ostream& out) {
    auto maybe_cmd = parse_slash_command(input);
    if (maybe_cmd.has_value())
        return dispatch_slash_command(*maybe_cmd, out);

    ++state_.turns;
    render_response(input, out);
    return CommandResult::Continue;
}

// ===========================================================================
// dispatch_slash_command  (mirrors Rust's dispatch_slash_command)
// ===========================================================================

CommandResult CliApp::dispatch_slash_command(const SlashCommand& cmd,
                                               std::ostream& out) {
    return std::visit([&](auto&& c) -> CommandResult {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, SlashHelp>)
            return handle_help(out);
        else if constexpr (std::is_same_v<T, SlashStatus>)
            return handle_status(out);
        else if constexpr (std::is_same_v<T, SlashCompact>)
            return handle_compact(out);
        else if constexpr (std::is_same_v<T, SlashModel>)
            return handle_model(
                c.model ? std::optional<std::string_view>{*c.model} : std::nullopt, out);
        else if constexpr (std::is_same_v<T, SlashPermissions>)
            return handle_permissions(
                c.mode ? std::optional<std::string_view>{*c.mode} : std::nullopt, out);
        else if constexpr (std::is_same_v<T, SlashConfig>)
            return handle_config(
                c.section ? std::optional<std::string_view>{*c.section} : std::nullopt, out);
        else if constexpr (std::is_same_v<T, SlashMemory>)
            return handle_memory(out);
        else if constexpr (std::is_same_v<T, SlashClear>)
            return handle_clear(c.confirm, out);
        else {
            // SlashUnknown
            out << "Unknown slash command: /" << c.name << "\n";
            return CommandResult::Continue;
        }
    }, cmd);
}

// ===========================================================================
// handle_help  (mirrors Rust's handle_help)
// ===========================================================================

CommandResult CliApp::handle_help(std::ostream& out) {
    out << "Available commands:\n";
    for (auto& e : SLASH_COMMANDS) {
        // Mirror Rust's {name:<9} format (9-char left-padded name column).
        std::string padded{e.name};
        while (padded.size() < 9) padded += ' ';
        out << "  " << padded << " " << e.summary << "\n";
    }
    return CommandResult::Continue;
}

// ===========================================================================
// handle_status  (mirrors Rust's handle_status)
// ===========================================================================

CommandResult CliApp::handle_status(std::ostream& out) {
    out << "status: turns=" << state_.turns
        << " model=" << state_.last_model
        << " permission-mode=" << permission_mode_label(config_.permission_mode)
        << " output-format=" << output_format_label(config_.output_format)
        << " last-usage=" << state_.last_usage.input_tokens << " in/"
        << state_.last_usage.output_tokens << " out"
        << " config=";
    if (config_.config.has_value())
        out << config_.config->string();
    else
        out << "<none>";
    out << "\n";
    return CommandResult::Continue;
}

// ===========================================================================
// handle_compact  (mirrors Rust's handle_compact)
// ===========================================================================

CommandResult CliApp::handle_compact(std::ostream& out) {
    state_.compacted_messages += state_.turns;
    state_.turns = 0;
    history_.clear();
    out << "Compacted session history into a local summary ("
        << state_.compacted_messages << " messages total compacted).\n";
    return CommandResult::Continue;
}

// ===========================================================================
// handle_model  (mirrors Rust's handle_model)
// ===========================================================================

CommandResult CliApp::handle_model(std::optional<std::string_view> model,
                                    std::ostream& out) {
    if (model.has_value()) {
        config_.model     = std::string{*model};
        state_.last_model = std::string{*model};
        out << "Active model set to " << *model << "\n";
    } else {
        out << "Active model: " << config_.model << "\n";
    }
    return CommandResult::Continue;
}

// ===========================================================================
// handle_permissions  (mirrors Rust's handle_permissions)
// ===========================================================================

CommandResult CliApp::handle_permissions(std::optional<std::string_view> mode,
                                          std::ostream& out) {
    if (!mode.has_value()) {
        out << "Permission mode: " << permission_mode_label(config_.permission_mode) << "\n";
        return CommandResult::Continue;
    }
    if (*mode == "read-only") {
        config_.permission_mode = PermissionMode::ReadOnly;
        out << "Permission mode set to read-only\n";
    } else if (*mode == "workspace-write") {
        config_.permission_mode = PermissionMode::WorkspaceWrite;
        out << "Permission mode set to workspace-write\n";
    } else if (*mode == "danger-full-access") {
        config_.permission_mode = PermissionMode::DangerFullAccess;
        out << "Permission mode set to danger-full-access\n";
    } else {
        out << "Unknown permission mode: " << *mode << "\n";
    }
    return CommandResult::Continue;
}

// ===========================================================================
// handle_config  (mirrors Rust's handle_config)
// ===========================================================================

CommandResult CliApp::handle_config(std::optional<std::string_view> section,
                                     std::ostream& out) {
    // Mirrors Rust handle_config: show config path or section
    auto cwd = std::filesystem::current_path();
    auto claude_json = cwd / ".claude.json";
    auto settings_json = cwd / ".claude" / "settings.json";
    auto path_str = config_.config.has_value()
                  ? config_.config->string()
                  : (std::filesystem::exists(claude_json) ? claude_json.string()
                     : (std::filesystem::exists(settings_json) ? settings_json.string()
                        : std::string("<none>")));

    if (!section.has_value()) {
        out << "Config\n"
            << "  Path             " << path_str << "\n"
            << "  Model            " << config_.model << "\n"
            << "  Permission mode  " << permission_mode_label(config_.permission_mode) << "\n"
            << "  Output format    " << output_format_label(config_.output_format) << "\n";
    } else {
        // Read and display specific section from config file
        if (std::filesystem::exists(claude_json)) {
            try {
                std::ifstream f(claude_json);
                auto j = nlohmann::json::parse(f);
                std::string key{*section};
                if (j.contains(key)) {
                    out << "Config section `" << key << "`:\n"
                        << j[key].dump(2) << "\n";
                } else {
                    out << "Config section `" << key << "` not found in "
                        << claude_json.string() << "\n";
                }
            } catch (const std::exception& e) {
                out << "Error reading config: " << e.what() << "\n";
            }
        } else {
            out << "No config file found at " << claude_json.string() << "\n";
        }
    }
    return CommandResult::Continue;
}

// ===========================================================================
// handle_memory  (mirrors Rust's handle_memory)
// ===========================================================================

CommandResult CliApp::handle_memory(std::ostream& out) {
    // Mirrors Rust handle_memory: scan for CLAUDE.md and .claude/memory files
    auto cwd = std::filesystem::current_path();
    out << "Memory\n";

    auto claude_md = cwd / "CLAUDE.md";
    if (std::filesystem::exists(claude_md))
        out << "  CLAUDE.md        " << claude_md.string() << "\n";
    else
        out << "  CLAUDE.md        (not found)\n";

    // Check for .claude/memory directory
    auto memory_dir = cwd / ".claude" / "memory";
    if (std::filesystem::is_directory(memory_dir)) {
        std::size_t count = 0;
        for (auto& entry : std::filesystem::directory_iterator(memory_dir)) {
            if (entry.is_regular_file()) ++count;
        }
        out << "  Memory files     " << count << " in " << memory_dir.string() << "\n";
    }

    // Check home directory CLAUDE.md
    if (auto* home = std::getenv("USERPROFILE")) {
        auto home_claude = std::filesystem::path(home) / "CLAUDE.md";
        if (std::filesystem::exists(home_claude))
            out << "  User CLAUDE.md   " << home_claude.string() << "\n";
    }

    return CommandResult::Continue;
}

// ===========================================================================
// handle_clear  (mirrors Rust's handle_clear)
// ===========================================================================

CommandResult CliApp::handle_clear(bool confirm, std::ostream& out) {
    if (!confirm) {
        out << "Refusing to clear without confirmation. Re-run as /clear --confirm\n";
        return CommandResult::Continue;
    }
    state_.turns              = 0;
    state_.compacted_messages = 0;
    state_.last_usage         = {};
    history_.clear();
    out << "Started a fresh local session.\n";
    return CommandResult::Continue;
}

// ===========================================================================
// handle_stream_event  (static; mirrors Rust's handle_stream_event)
// ===========================================================================

void CliApp::handle_stream_event(
    const TerminalRenderer& renderer,
    const StreamEvent& event,
    Spinner& stream_spinner,
    Spinner& tool_spinner,
    bool& saw_text,
    UsageSummary& turn_usage,
    std::ostream& out)
{
    std::visit([&](auto&& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, TextDelta>) {
            if (!saw_text) {
                stream_spinner.finish("Streaming response", renderer.color_theme(), out);
                saw_text = true;
            }
            out << e.delta << std::flush;
        } else if constexpr (std::is_same_v<T, ToolCallStart>) {
            if (saw_text) out << "\n";
            tool_spinner.tick(
                "Running tool `" + e.name + "` with " + e.input,
                renderer.color_theme(), out);
        } else if constexpr (std::is_same_v<T, ToolCallResult>) {
            std::string label = e.is_error
                ? "Tool `" + e.name + "` failed"
                : "Tool `" + e.name + "` completed";
            tool_spinner.finish(label, renderer.color_theme(), out);
            std::string rendered = "### Tool `" + e.name + "`\n\n```text\n"
                                 + e.output + "\n```\n";
            renderer.stream_markdown(rendered, out);
        } else if constexpr (std::is_same_v<T, UsageEvent>) {
            turn_usage = e.usage;
        }
    }, event);
}

// ===========================================================================
// write_turn_output  (mirrors Rust's write_turn_output)
// ===========================================================================

void CliApp::write_turn_output(const TurnSummary& summary, std::ostream& out) const {
    switch (config_.output_format) {
        case OutputFormat::Text:
            out << "\nToken usage: "
                << state_.last_usage.input_tokens << " input / "
                << state_.last_usage.output_tokens << " output\n";
            break;
        case OutputFormat::Json:
            // Minimal JSON without nlohmann/json dependency.
            out << "{\"message\":\""
                << summary.assistant_text
                << "\",\"usage\":{\"input_tokens\":"
                << state_.last_usage.input_tokens
                << ",\"output_tokens\":"
                << state_.last_usage.output_tokens << "}}\n";
            break;
        case OutputFormat::Ndjson:
            out << "{\"type\":\"message\",\"text\":\""
                << summary.assistant_text
                << "\",\"usage\":{\"input_tokens\":"
                << state_.last_usage.input_tokens
                << ",\"output_tokens\":"
                << state_.last_usage.output_tokens << "}}\n";
            break;
    }
}

// ===========================================================================
// render_response  (mirrors Rust's render_response)
// ===========================================================================

void CliApp::render_response(std::string_view input, std::ostream& out) {
    Spinner stream_spinner;
    stream_spinner.tick("Opening conversation stream", renderer_.color_theme(), out);

    UsageSummary turn_usage{};
    Spinner tool_spinner;
    bool saw_text = false;
    const auto& renderer = renderer_;

    TurnSummary summary;
    try {
        summary = client_.run_turn(history_, input, [&](StreamEvent ev) {
            handle_stream_event(renderer, ev, stream_spinner, tool_spinner,
                                saw_text, turn_usage, out);
        });
    } catch (const std::exception& ex) {
        stream_spinner.fail("Streaming response failed", renderer_.color_theme(), out);
        throw;
    }

    state_.last_usage = summary.usage;
    if (saw_text) {
        out << "\n";
    } else {
        stream_spinner.finish("Streaming response", renderer_.color_theme(), out);
    }

    write_turn_output(summary, out);
}

} // namespace claw
