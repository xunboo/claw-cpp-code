#pragma once
// app.hpp -- C++20 port of app.rs
// CliApp, SessionConfig, SessionState, SlashCommand, CommandResult

#include "args.hpp"
#include "input.hpp"
#include "render.hpp"
#include "types.hpp"  // InputMessage, InputContentBlock, ToolResultContentBlock

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace claw {

// Opaque handle for the runtime session (avoids including session.hpp here,
// which would collide with commands' runtime_types.hpp namespace).
struct SessionImpl;

// ---- UsageSummary ----
/// Minimal mirror of runtime::UsageSummary.
struct UsageSummary {
    uint64_t input_tokens{0};
    uint64_t output_tokens{0};
};

// ---- ToolUseRecord / ToolResultRecord ----
/// Records of tool interactions for structured output.
struct ToolUseRecord {
    std::string id;
    std::string name;
    std::string input;
};
struct ToolResultRecord {
    std::string tool_use_id;
    std::string name;
    std::string output;
    bool is_error{false};
};

// ---- TurnSummary ----
/// Mirrors Rust TurnSummary.
struct TurnSummary {
    std::string assistant_text;
    UsageSummary usage;
    std::size_t iterations{1};
    std::vector<ToolUseRecord> tool_uses;
    std::vector<ToolResultRecord> tool_results;
};

// ---- ConversationMessage ----
/// Rich message with full content blocks — mirrors Rust ConversationMessage.
/// Stores API-level InputMessage directly so tool_use/tool_result blocks
/// survive across turns in the conversation history.
struct ConversationMessage {
    claw::api::InputMessage api_message;

    /// Convenience: user text message
    static ConversationMessage user_text(std::string text) {
        ConversationMessage m;
        m.api_message = claw::api::InputMessage::user_text(std::move(text));
        return m;
    }
    /// Convenience: wrap a pre-built InputMessage
    static ConversationMessage from_api(claw::api::InputMessage msg) {
        ConversationMessage m;
        m.api_message = std::move(msg);
        return m;
    }
};

// ---- StreamEvent ----
/// Mirrors runtime::StreamEvent (the events delivered during a streaming turn).
struct TextDelta  { std::string delta; };
struct ToolCallStart { std::string name; std::string input; };
struct ToolCallResult {
    std::string name;
    std::string output;
    bool is_error{false};
};
struct UsageEvent { UsageSummary usage; };
using StreamEvent = std::variant<TextDelta, ToolCallStart, ToolCallResult, UsageEvent>;

// ---- SessionConfig ----
/// Mirrors Rust's SessionConfig struct.
struct SessionConfig {
    std::string model;
    PermissionMode permission_mode{PermissionMode::DangerFullAccess};
    std::optional<std::filesystem::path> config;
    OutputFormat output_format{OutputFormat::Text};
    std::filesystem::path project_root;  // project root for session storage
};

// ---- SessionState ----
/// Mirrors Rust's SessionState struct.
struct SessionState {
    std::size_t turns{0};
    std::size_t compacted_messages{0};
    std::string last_model;
    UsageSummary last_usage;

    explicit SessionState(std::string model) : last_model{std::move(model)} {}
};

// ---- CommandResult ----
/// Mirrors Rust's CommandResult enum (currently a single variant).
enum class CommandResult { Continue };

// ---- SlashCommand ----
/// Mirrors Rust's SlashCommand enum.

struct SlashHelp        {};
struct SlashStatus      {};
struct SlashCompact     {};
struct SlashModel       { std::optional<std::string> model; };
struct SlashPermissions { std::optional<std::string> mode; };
struct SlashConfig      { std::optional<std::string> section; };
struct SlashMemory      {};
struct SlashClear       { bool confirm{false}; };
struct SlashCost        {};
struct SlashDiff        {};
struct SlashCommit      {};
struct SlashExport      { std::optional<std::string> path; };
struct SlashSession     { std::optional<std::string> action; std::optional<std::string> target; };
struct SlashResume      { std::optional<std::string> session; };
struct SlashInit        {};
struct SlashMcp         { std::optional<std::string> args; };
struct SlashAgents      { std::optional<std::string> args; };
struct SlashSkills      { std::optional<std::string> args; };
struct SlashDoctor      {};
struct SlashSandbox     {};
struct SlashVersion     {};
struct SlashExit        {};
struct SlashUnknown     { std::string name; };

using SlashCommand = std::variant<
    SlashHelp, SlashStatus, SlashCompact,
    SlashModel, SlashPermissions, SlashConfig,
    SlashMemory, SlashClear, SlashCost, SlashDiff,
    SlashCommit, SlashExport, SlashSession, SlashResume,
    SlashInit, SlashMcp, SlashAgents, SlashSkills, SlashDoctor,
    SlashSandbox, SlashVersion, SlashExit, SlashUnknown>;

/// Parse a text string as a slash command.
/// Returns nullopt when the string does not start with '/'.
[[nodiscard]] std::optional<SlashCommand> parse_slash_command(std::string_view input);

// ---- ConversationClient ----
/// Calls the Anthropic Messages API and delivers streaming events.
class ConversationClient {
public:
    explicit ConversationClient(std::string model);

    /// Perform one conversation turn.  Calls event_callback for each StreamEvent.
    /// Returns a TurnSummary or throws std::runtime_error on failure.
    [[nodiscard]] TurnSummary run_turn(
        std::vector<ConversationMessage>& history,
        std::string_view user_input,
        std::function<void(StreamEvent)> event_callback);

    static ConversationClient from_env(std::string model);

    /// Set the API key explicitly (if not using env)
    void set_api_key(std::string key) { api_key_ = std::move(key); }

private:
    std::string model_;
    std::string api_key_;
    std::string base_url_;
};

// ---- CliApp ----
/// Mirrors Rust's CliApp struct.
class CliApp {
public:
    explicit CliApp(SessionConfig config);
    ~CliApp();  // defined in app.cpp (unique_ptr to incomplete type)

    /// Run the interactive REPL.
    void run_repl();

    /// Run a single non-interactive prompt, writing output to `out`.
    void run_prompt(std::string_view prompt, std::ostream& out);

    /// Dispatch one line of input (slash command or conversation turn).
    CommandResult handle_submission(std::string_view input, std::ostream& out);

    /// Print help to `out` (static -- no instance state needed).
    static CommandResult handle_help(std::ostream& out);

    /// Load a session from a JSONL file and populate conversation history.
    /// Returns true on success, false on failure.
    bool load_session_from_path(const std::filesystem::path& path);

    /// Full snapshot persist (mirrors Rust's persist_session → save_to_path).
    void persist_session();

    /// Incremental append persist (mirrors Rust's push_message → append).
    void persist_session_incremental();

private:
    void sync_runtime_session();

    SessionConfig config_;
    TerminalRenderer renderer_;
    SessionState state_;
    ConversationClient client_;
    std::vector<ConversationMessage> history_;
    std::unique_ptr<SessionImpl> session_impl_;  // PIMPL — wraps claw::runtime::Session
    std::filesystem::path session_path_;          // .claw/sessions/<id>.jsonl

    CommandResult dispatch_slash_command(const SlashCommand& cmd, std::ostream& out);
    CommandResult handle_status(std::ostream& out);
    CommandResult handle_compact(std::ostream& out);
    CommandResult handle_model(std::optional<std::string_view> model, std::ostream& out);
    CommandResult handle_permissions(std::optional<std::string_view> mode, std::ostream& out);
    CommandResult handle_config(std::optional<std::string_view> section, std::ostream& out);
    CommandResult handle_memory(std::ostream& out);
    CommandResult handle_clear(bool confirm, std::ostream& out);

    static void handle_stream_event(
        const TerminalRenderer& renderer,
        const StreamEvent& event,
        Spinner& stream_spinner,
        Spinner& tool_spinner,
        bool& saw_text,
        UsageSummary& turn_usage,
        std::ostream& out);

    void write_turn_output(const TurnSummary& summary, std::ostream& out) const;
    void render_response(std::string_view input, std::ostream& out);
};

} // namespace claw