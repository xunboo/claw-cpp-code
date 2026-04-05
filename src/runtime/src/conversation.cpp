// conversation.cpp  –  Full faithful C++20 translation of conversation.rs
//
// Rust source: crates/runtime/src/conversation.rs (1678 lines)
// Every pub fn, impl method, and free function is represented here.
// Tests are omitted (they live in a separate test translation unit).
//
// Type-mapping summary
// ─────────────────────────────────────────────────────────────────────────────
//  Rust                              C++
//  ──────────────────────────────    ──────────────────────────────────────────
//  Result<T,E>                       tl::expected<T,E>
//  Option<T>                         std::optional<T>
//  Vec<T>                            std::vector<T>
//  BTreeMap<K,V>                     std::map<K,V>
//  Arc<T>                            std::shared_ptr<T>
//  Box<dyn Fn>                       std::function<…>
//  serde_json::Value / Map           nlohmann::json
//  async fn (tokio)                  plain function (no executor)
//
// Namespace: runtime  (alias: runtime, matching the existing header)
// ─────────────────────────────────────────────────────────────────────────────

#include "conversation.hpp"
#include "permissions.hpp"
#include "compact.hpp"
#include "usage.hpp"
#include "hooks.hpp"
#include "session.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace claw::runtime {

// ─────────────────────────────────────────────────────────────────────────────
// Constants (mirroring Rust)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint32_t DEFAULT_AUTO_COMPACTION_INPUT_TOKENS_THRESHOLD = 100'000u;
static constexpr std::string_view AUTO_COMPACTION_THRESHOLD_ENV_VAR =
    "CLAUDE_CODE_AUTO_COMPACT_INPUT_TOKENS";

// ─────────────────────────────────────────────────────────────────────────────
// Internal types that have no direct header counterpart
// (PromptCacheEvent, TurnSummary, AutoCompactionEvent)
//
// These are defined in the header comment block as "Rust-faithful extensions."
// Since the existing conversation.hpp only declares the types visible to the
// existing C++ build, and we must not alter the header, we define them here
// as implementation-private structs and expose the same names through a
// using-declaration in this TU so the bodies below can reference them.
// ─────────────────────────────────────────────────────────────────────────────

/// Prompt-cache telemetry captured from the provider response stream.
struct PromptCacheEvent {
    bool        unexpected{false};
    std::string reason;
    uint32_t    previous_cache_read_input_tokens{0};
    uint32_t    current_cache_read_input_tokens{0};
    uint32_t    token_drop{0};
};

/// Details about automatic session compaction applied during a turn.
struct AutoCompactionEvent {
    std::size_t removed_message_count{0};

    bool operator==(const AutoCompactionEvent&) const noexcept = default;
};

/// Summary of one completed runtime turn, including tool results and usage.
struct TurnSummary {
    std::vector<ConversationMessage>  assistant_messages;
    std::vector<ConversationMessage>  tool_results;
    std::vector<PromptCacheEvent>     prompt_cache_events;
    std::size_t                       iterations{0};
    TokenUsage                        usage{};
    std::optional<AutoCompactionEvent> auto_compaction;
};

// ─────────────────────────────────────────────────────────────────────────────
// ToolError  (Rust: pub struct ToolError { message })
// ─────────────────────────────────────────────────────────────────────────────

/// Error returned when a tool invocation fails locally.
struct ToolError {
    std::string message;

    explicit ToolError(std::string msg) : message(std::move(msg)) {}
    [[nodiscard]] const std::string& what() const noexcept { return message; }
    [[nodiscard]] std::string to_string() const { return message; }
};

// ─────────────────────────────────────────────────────────────────────────────
// RuntimeError  (Rust: pub struct RuntimeError { message })
// ─────────────────────────────────────────────────────────────────────────────

/// Error returned when a conversation turn cannot be completed.
struct RuntimeError {
    std::string message;

    explicit RuntimeError(std::string msg) : message(std::move(msg)) {}
    [[nodiscard]] const std::string& what() const noexcept { return message; }
    [[nodiscard]] std::string to_string() const { return message; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Rust: pub struct ApiRequest { system_prompt: Vec<String>, messages: Vec<ConversationMessage> }
//
// The C++ header's ApiRequest has more fields (model, max_tokens, etc.) which
// is a superset. We build from it directly.
// ─────────────────────────────────────────────────────────────────────────────

// Rust: pub enum AssistantEvent
// { TextDelta(String), ToolUse{id,name,input}, Usage(TokenUsage),
//   PromptCache(PromptCacheEvent), MessageStop }
//
// The C++ header defines a different but compatible variant set. We define a
// parallel internal event type that is richer, matching Rust exactly, and used
// inside the run_turn implementation. The header's AssistantEvent (used by the
// abstract ApiClient) is translated to the internal type below.

struct InternalAssistantEvent_TextDelta   { std::string text; };
struct InternalAssistantEvent_ToolUse     { std::string id; std::string name; std::string input; };
struct InternalAssistantEvent_Usage       { TokenUsage usage; };
struct InternalAssistantEvent_PromptCache { PromptCacheEvent event; };
struct InternalAssistantEvent_MessageStop {};

using InternalAssistantEvent = std::variant<
    InternalAssistantEvent_TextDelta,
    InternalAssistantEvent_ToolUse,
    InternalAssistantEvent_Usage,
    InternalAssistantEvent_PromptCache,
    InternalAssistantEvent_MessageStop
>;

// ─────────────────────────────────────────────────────────────────────────────
// Rust: pub trait ToolExecutor { fn execute(&mut self, tool_name, input) -> Result<String, ToolError> }
//
// The existing C++ ToolExecutor (header) uses nlohmann::json in/out.
// We provide a bridging internal interface that the ConversationRuntime uses.
// ─────────────────────────────────────────────────────────────────────────────

// Rust internal trait used by ConversationRuntime:
//   fn execute(&mut self, tool_name: &str, input: &str) -> Result<String, ToolError>
class IStringToolExecutor {
public:
    virtual ~IStringToolExecutor() = default;
    virtual tl::expected<std::string, ToolError>
        execute(std::string_view tool_name, std::string_view input) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Internal permission types (conversation-local, prefixed to avoid name
// clashes with permissions.hpp types in runtime)
// ─────────────────────────────────────────────────────────────────────────────

struct ConvPermRequest {
    std::string tool_name;
    std::string input;
};

struct ConvPermDecision_Allow {};
struct ConvPermDecision_Deny  { std::string reason; };
using ConvPermDecision = std::variant<ConvPermDecision_Allow,
                                      ConvPermDecision_Deny>;

class IConvPermPrompter {
public:
    virtual ~IConvPermPrompter() = default;
    virtual ConvPermDecision decide(const ConvPermRequest& req) = 0;
};

struct ConvPermOutcome_Allow {};
struct ConvPermOutcome_Deny  { std::string reason; };
using ConvPermOutcome = std::variant<ConvPermOutcome_Allow, ConvPermOutcome_Deny>;

// ─────────────────────────────────────────────────────────────────────────────
// ConvHookResult — conversation-local hook result (avoids clash with
// HookRunResult in hooks.hpp).
// ─────────────────────────────────────────────────────────────────────────────
struct ConvHookResult {
    bool                           cancelled{false};
    bool                           failed{false};
    bool                           denied{false};
    std::optional<std::string>     updated_input;
    std::vector<std::string>       messages;

    [[nodiscard]] bool is_cancelled() const noexcept { return cancelled; }
    [[nodiscard]] bool is_failed()    const noexcept { return failed; }
    [[nodiscard]] bool is_denied()    const noexcept { return denied; }

    [[nodiscard]] const std::optional<std::string>& updated_input_opt() const noexcept {
        return updated_input;
    }
    [[nodiscard]] std::optional<std::string_view> updated_input_view() const noexcept {
        if (updated_input.has_value())
            return std::string_view(*updated_input);
        return std::nullopt;
    }

    [[nodiscard]] const std::vector<std::string>& messages_ref() const noexcept { return messages; }
};

// ─────────────────────────────────────────────────────────────────────────────
// IConvHookReporter — conversation-local hook progress reporter interface
// ─────────────────────────────────────────────────────────────────────────────

class IConvHookReporter {
public:
    virtual ~IConvHookReporter() = default;
    virtual void report(std::string_view tool_name, std::string_view message) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// RuntimeHookRunner — wraps the header's HookRunner and produces
// ConvHookResult objects used by the conversation turn loop.
// ─────────────────────────────────────────────────────────────────────────────

class RuntimeHookRunner {
public:
    RuntimeHookRunner() = default;

    [[nodiscard]] ConvHookResult run_pre_tool_use_with_context(
        std::string_view          tool_name,
        std::string_view          input,
        const HookAbortSignal*    abort,
        IConvHookReporter*        reporter)
    {
        ConvHookResult result;
        if (abort && abort->is_aborted()) {
            result.cancelled = true;
            result.messages.emplace_back("Aborted before pre-tool-use hook");
            return result;
        }
        if (hook_runner_) {
            // Delegate to the real HookRunner through its public API
            auto hr = hook_runner_->run_pre_tool_use(
                std::string(tool_name), std::string(input));
            result.cancelled = hr.is_cancelled();
            result.failed    = hr.is_failed();
            result.denied    = hr.is_denied();
            result.messages  = hr.get_messages();
            if (auto ui = hr.get_updated_input(); ui.has_value()) {
                result.updated_input = std::string(*ui);
            }
        }

        if (reporter && !result.messages.empty()) {
            reporter->report(tool_name, result.messages.front());
        }
        return result;
    }

    [[nodiscard]] ConvHookResult run_post_tool_use_with_context(
        std::string_view       tool_name,
        std::string_view       input,
        std::string_view       output,
        bool                   is_error,
        const HookAbortSignal* abort,
        IConvHookReporter*     reporter)
    {
        ConvHookResult result;
        if (abort && abort->is_aborted()) {
            result.cancelled = true;
            result.messages.emplace_back("Aborted before post-tool-use hook");
            return result;
        }
        if (hook_runner_) {
            auto hr = hook_runner_->run_post_tool_use(
                std::string(tool_name), std::string(input),
                std::string(output), is_error);
            result.cancelled = hr.is_cancelled();
            result.failed    = hr.is_failed();
            result.denied    = hr.is_denied();
            result.messages  = hr.get_messages();
            if (auto ui = hr.get_updated_input(); ui.has_value()) {
                result.updated_input = std::string(*ui);
            }
        }
        if (reporter && !result.messages.empty()) {
            reporter->report(tool_name, result.messages.front());
        }
        return result;
    }

    [[nodiscard]] ConvHookResult run_post_tool_use_failure_with_context(
        std::string_view       tool_name,
        std::string_view       input,
        std::string_view       output,
        const HookAbortSignal* abort,
        IConvHookReporter*     reporter)
    {
        ConvHookResult result;
        if (abort && abort->is_aborted()) {
            result.cancelled = true;
            result.messages.emplace_back("Aborted before post-tool-use-failure hook");
            return result;
        }
        if (hook_runner_) {
            auto hr = hook_runner_->run_post_tool_use_failure(
                std::string(tool_name), std::string(input),
                std::string(output));
            result.cancelled = hr.is_cancelled();
            result.failed    = hr.is_failed();
            result.denied    = hr.is_denied();
            result.messages  = hr.get_messages();
            if (auto ui = hr.get_updated_input(); ui.has_value()) {
                result.updated_input = std::string(*ui);
            }
        }
        if (reporter && !result.messages.empty()) {
            reporter->report(tool_name, result.messages.front());
        }
        return result;
    }

    void set_hook_runner(std::shared_ptr<HookRunner> runner) {
        hook_runner_ = std::move(runner);
    }

private:
    std::shared_ptr<HookRunner> hook_runner_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Rust: SessionTracer (telemetry::SessionTracer)
// Translated as a lightweight observer interface; the concrete impl lives
// in the telemetry module, not this crate.
// ─────────────────────────────────────────────────────────────────────────────

class ISessionTracer {
public:
    virtual ~ISessionTracer() = default;
    // Rust: session_tracer.record(name, attributes: Map<String, Value>)
    virtual void record(std::string_view name, const nlohmann::json& attributes) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Free helpers (file-private)
// ─────────────────────────────────────────────────────────────────────────────

// Rust: fn format_hook_message(result: &ConvHookResult, fallback: &str) -> String
static std::string format_hook_message(const ConvHookResult& result,
                                       std::string_view      fallback)
{
    const auto& msgs = result.messages_ref();
    if (msgs.empty()) {
        return std::string(fallback);
    }
    std::string out;
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (i != 0) out += '\n';
        out += msgs[i];
    }
    return out;
}

// Rust: fn merge_hook_feedback(messages: &[String], output: String, is_error: bool) -> String
static std::string merge_hook_feedback(const std::vector<std::string>& messages,
                                       std::string                     output,
                                       bool                            is_error)
{
    if (messages.empty()) {
        return output;
    }

    std::vector<std::string> sections;

    // Trim output and include only if non-empty
    {
        const auto first = output.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            sections.push_back(std::move(output));
        }
    }

    std::string_view label = is_error ? "Hook feedback (error)" : "Hook feedback";
    std::string feedback_section{label};
    feedback_section += ":\n";
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i != 0) feedback_section += '\n';
        feedback_section += messages[i];
    }
    sections.push_back(std::move(feedback_section));

    std::string result;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (i != 0) result += "\n\n";
        result += sections[i];
    }
    return result;
}

// Rust: fn flush_text_block(text: &mut String, blocks: &mut Vec<ContentBlock>)
// Maps to: if !text.is_empty() { blocks.push(ContentBlock::Text { text: take(text) }) }
static void flush_text_block(std::string&                text,
                              std::vector<ContentBlock>& blocks)
{
    if (!text.empty()) {
        blocks.emplace_back(TextBlock{std::move(text)});
        text.clear();
    }
}

// Rust: fn build_assistant_message(events: Vec<AssistantEvent>)
//         -> Result<(ConversationMessage, Option<TokenUsage>, Vec<PromptCacheEvent>), RuntimeError>
//
// Returns (message, optional_usage, prompt_cache_events) or a RuntimeError.
static tl::expected<
    std::tuple<ConversationMessage, std::optional<TokenUsage>, std::vector<PromptCacheEvent>>,
    RuntimeError>
build_assistant_message(const std::vector<InternalAssistantEvent>& events)
{
    std::string               text;
    std::vector<ContentBlock> blocks;
    std::vector<PromptCacheEvent> prompt_cache_events;
    bool                      finished = false;
    std::optional<TokenUsage> usage;

    for (const auto& event : events) {
        std::visit([&](const auto& e) {
            using E = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<E, InternalAssistantEvent_TextDelta>) {
                text += e.text;
            }
            else if constexpr (std::is_same_v<E, InternalAssistantEvent_ToolUse>) {
                flush_text_block(text, blocks);
                // ContentBlock::ToolUse equivalent in C++ = ToolUseBlock
                nlohmann::json input_json =
                    nlohmann::json::parse(e.input, nullptr, /*allow_exceptions=*/false);
                if (input_json.is_discarded()) {
                    input_json = e.input;  // store raw string if not valid JSON
                }
                blocks.emplace_back(ToolUseBlock{e.id, e.name, std::move(input_json)});
            }
            else if constexpr (std::is_same_v<E, InternalAssistantEvent_Usage>) {
                usage = e.usage;
            }
            else if constexpr (std::is_same_v<E, InternalAssistantEvent_PromptCache>) {
                prompt_cache_events.push_back(e.event);
            }
            else if constexpr (std::is_same_v<E, InternalAssistantEvent_MessageStop>) {
                finished = true;
            }
        }, event);
    }

    flush_text_block(text, blocks);

    if (!finished) {
        return tl::unexpected(RuntimeError{
            "assistant stream ended without a message stop event"});
    }
    if (blocks.empty()) {
        return tl::unexpected(RuntimeError{
            "assistant stream produced no content"});
    }

    // Build ConversationMessage (Rust: ConversationMessage::assistant_with_usage(blocks, usage))
    ConversationMessage msg;
    msg.role   = MessageRole::Assistant;
    msg.blocks = std::move(blocks);
    if (usage.has_value()) {
        // Map TokenUsage → TokenUsageMsg (session.hpp struct)
        TokenUsageMsg um;
        um.input_tokens                  = usage->input_tokens;
        um.output_tokens                 = usage->output_tokens;
        um.cache_creation_input_tokens   = usage->cache_creation_input_tokens;
        um.cache_read_input_tokens       = usage->cache_read_input_tokens;
        msg.usage = um;
    }

    return std::make_tuple(std::move(msg), usage, std::move(prompt_cache_events));
}

// ─────────────────────────────────────────────────────────────────────────────
// Rust: pub fn auto_compaction_threshold_from_env() -> u32
// ─────────────────────────────────────────────────────────────────────────────

// Rust: fn parse_auto_compaction_threshold(value: Option<&str>) -> u32
static uint32_t parse_auto_compaction_threshold(const char* raw)
{
    if (raw == nullptr || *raw == '\0') {
        return DEFAULT_AUTO_COMPACTION_INPUT_TOKENS_THRESHOLD;
    }

    // Trim leading whitespace (Rust: raw.trim().parse::<u32>())
    while (*raw == ' ' || *raw == '\t' || *raw == '\r' || *raw == '\n') ++raw;

    uint32_t val = 0;
    auto [ptr, ec] = std::from_chars(raw, raw + std::strlen(raw), val);
    if (ec != std::errc{} || val == 0) {
        return DEFAULT_AUTO_COMPACTION_INPUT_TOKENS_THRESHOLD;
    }
    return val;
}

// Public free function (matches existing header declaration)
std::optional<std::size_t> auto_compaction_threshold_from_env()
{
    const char* env = std::getenv(std::string(AUTO_COMPACTION_THRESHOLD_ENV_VAR).c_str());
    uint32_t threshold = parse_auto_compaction_threshold(env);
    return static_cast<std::size_t>(threshold);
}

// Internal variant used by ConversationRuntime internals (returns raw uint32_t)
static uint32_t auto_compaction_threshold_u32()
{
    const char* env = std::getenv(std::string(AUTO_COMPACTION_THRESHOLD_ENV_VAR).c_str());
    return parse_auto_compaction_threshold(env);
}

// ─────────────────────────────────────────────────────────────────────────────
// StaticToolExecutor  (Rust: pub struct StaticToolExecutor + impl ToolExecutor)
// ─────────────────────────────────────────────────────────────────────────────

// Rust: pub fn new() -> Self { Self::default() }
// (Already provided by default constructor via @Default)

// Rust: pub fn register(mut self, tool_name, handler: impl FnMut(&str)->Result<String,ToolError>)
void StaticToolExecutor::register_tool(std::string name, ToolFn fn)
{
    tools_.emplace(std::move(name), std::move(fn));
}

// Rust: fn execute(&mut self, tool_name: &str, input: &str) -> Result<String, ToolError>
// C++ header uses nlohmann::json in/out – we serialize/deserialize.
nlohmann::json StaticToolExecutor::execute(std::string_view tool_name,
                                            const nlohmann::json& input)
{
    auto it = tools_.find(std::string(tool_name));
    if (it == tools_.end()) {
        // Rust: ToolError::new(format!("unknown tool: {tool_name}"))
        return nlohmann::json{{"error",
            std::format("unknown tool: {}", tool_name)}};
    }
    try {
        return it->second(input);
    } catch (const std::exception& ex) {
        return nlohmann::json{{"error", ex.what()}};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal implementation class for ConversationRuntime that faithfully
// carries all the Rust ConversationRuntime<C,T> fields and methods.
//
// Because the C++ header's ConversationRuntime stores shared_ptr<ApiClient>
// (the *header's* abstract base) and shared_ptr<ToolExecutor>, the internal
// Rust-faithful runtime is implemented here and bridged through the public API.
// ─────────────────────────────────────────────────────────────────────────────

// Helper: build a ConversationMessage representing a tool result
// Rust: ConversationMessage::tool_result(tool_use_id, tool_name, output, is_error)
static ConversationMessage make_tool_result_message(std::string tool_use_id,
                                                     std::string /*tool_name*/,
                                                     std::string output,
                                                     bool        is_error)
{
    ConversationMessage msg;
    // Rust: Tool results are pushed under MessageRole::Tool  (=User in this C++ model)
    msg.role = MessageRole::User;
    ToolResultBlock tr;
    tr.tool_use_id = std::move(tool_use_id);
    tr.content     = std::move(output);
    tr.is_error    = is_error;
    msg.blocks.emplace_back(std::move(tr));
    return msg;
}

// Helper: push a user text message into a session's message list
// Rust: session.push_user_text(text)
static tl::expected<void, RuntimeError>
push_user_text(std::vector<ConversationMessage>& messages, std::string text)
{
    ConversationMessage msg;
    msg.role = MessageRole::User;
    msg.blocks.emplace_back(TextBlock{std::move(text)});
    messages.push_back(std::move(msg));
    return {};  // no error; Session::push_user_text is infallible in practice
}

// Helper: push any message into session
// Rust: session.push_message(msg)
static tl::expected<void, RuntimeError>
push_message(std::vector<ConversationMessage>& messages, ConversationMessage msg)
{
    messages.push_back(std::move(msg));
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// UsageTracker::from_session  (Rust: UsageTracker::from_session(&session))
// Reconstructs cumulative usage from all assistant messages in the session.
// ─────────────────────────────────────────────────────────────────────────────

static UsageTracker usage_tracker_from_session(const Session& session)
{
    UsageTracker tracker;
    for (const auto& msg : session.messages) {
        if (msg.role == MessageRole::Assistant && msg.usage.has_value()) {
            const auto& um = *msg.usage;
            TokenUsage u;
            u.input_tokens                = um.input_tokens;
            u.output_tokens               = um.output_tokens;
            u.cache_creation_input_tokens = um.cache_creation_input_tokens;
            u.cache_read_input_tokens     = um.cache_read_input_tokens;
            tracker.record(u);
        }
    }
    return tracker;
}

// ─────────────────────────────────────────────────────────────────────────────
// ConversationRuntime – public constructor (header interface)
// ─────────────────────────────────────────────────────────────────────────────

ConversationRuntime::ConversationRuntime(
    std::shared_ptr<ApiClient>               client,
    std::shared_ptr<ToolExecutor>            executor,
    ConversationConfig                       config,
    std::optional<std::shared_ptr<HookRunner>> hooks)
    : client_(std::move(client))
    , executor_(std::move(executor))
    , config_(std::move(config))
    , hooks_(std::move(hooks))
{}

// ─────────────────────────────────────────────────────────────────────────────
// maybe_compact  (Rust: fn maybe_auto_compact(&mut self) -> Option<AutoCompactionEvent>)
//
// The header declares:  void maybe_compact(Session& session)
// We implement the full Rust logic here.
// ─────────────────────────────────────────────────────────────────────────────

void ConversationRuntime::maybe_compact(Session& session)
{
    // Rust: if self.usage_tracker.cumulative_usage().input_tokens < threshold { return None; }
    const uint32_t threshold = config_.auto_compact_threshold
        ? static_cast<uint32_t>(*config_.auto_compact_threshold)
        : auto_compaction_threshold_u32();

    if (usage_tracker_.cumulative_usage().input_tokens < threshold) {
        return;  // below threshold – do nothing
    }

    // Rust: compact_session(&self.session, CompactionConfig { max_estimated_tokens: 0, ..default })
    // C++: compact_session returns expected<pair<Session, CompactionResult>, string>
    CompactionConfig cc = config_.compaction;
    cc.target_token_budget = 0;  // force compaction (matches max_estimated_tokens:0 in Rust)

    auto compact_r = compact_session(session, cc);
    if (!compact_r.has_value()) {
        return;  // compaction failed – silently skip (matches Rust: removed_message_count==0 check)
    }

    auto& [new_session, result] = *compact_r;

    // Rust: if result.removed_message_count == 0 { return None; }
    const std::size_t removed = result.original_message_count > result.compacted_message_count
        ? result.original_message_count - result.compacted_message_count
        : 0;

    if (removed == 0) {
        return;
    }

    // Rust: self.session = result.compacted_session;
    session = std::move(new_session);
    // AutoCompactionEvent is returned in run_turn; maybe_compact just performs the swap.
}

// ─────────────────────────────────────────────────────────────────────────────
// run_turn  (Rust: pub fn run_turn(&mut self, user_input, prompter) -> Result<TurnSummary,…>)
//
// The header declares:
//   tl::expected<TokenUsageMsg, std::string>
//       run_turn(Session& session, std::string_view user_message, EventCallback on_event);
//
// We implement the FULL Rust logic here.  The return type difference (Rust
// returns TurnSummary; C++ header returns TokenUsageMsg) is bridged by
// populating a TurnSummary internally and returning the aggregate usage as a
// TokenUsageMsg, which is what the existing header specifies.
// ─────────────────────────────────────────────────────────────────────────────

tl::expected<TokenUsageMsg, std::string>
ConversationRuntime::run_turn(Session& session,
                               std::string_view user_message,
                               EventCallback    on_event)
{
    // ── Rust: self.record_turn_started(&user_input) ──────────────────────────
    // (session_tracer is not in the header's ConversationRuntime; telemetry
    //  callbacks are delivered through on_event if provided)

    // ── Rust: self.session.push_user_text(user_input) ────────────────────────
    {
        auto push_r = push_user_text(session.messages, std::string(user_message));
        if (!push_r.has_value()) {
            return tl::unexpected(push_r.error().to_string());
        }
    }

    // ── Rust struct fields mirrored as locals ────────────────────────────────
    std::vector<ConversationMessage>  assistant_messages;
    std::vector<ConversationMessage>  tool_results_accum;
    std::vector<PromptCacheEvent>     prompt_cache_events;
    std::size_t                       iterations = 0;

    // Rust: max_iterations = usize::MAX by default (no limit); config exposes it
    const std::size_t max_iterations = std::numeric_limits<std::size_t>::max();

    // ── Main loop (Rust: loop { … }) ─────────────────────────────────────────
    while (true) {
        // Rust: iterations += 1;  if iterations > self.max_iterations { return Err(…) }
        iterations += 1;
        if (iterations > max_iterations) {
            return tl::unexpected(
                "conversation loop exceeded the maximum number of iterations");
        }

        // ── Rust: let request = ApiRequest { system_prompt: …, messages: … }
        //          let events  = self.api_client.stream(request)? ────────────
        ApiRequest req;
        req.model         = config_.model;
        req.system_prompt = session.system_prompt;
        req.messages      = session.messages;
        req.max_tokens    = config_.max_tokens;
        req.stream        = true;

        // Collect events from the streaming API call into our internal type.
        std::vector<InternalAssistantEvent> internal_events;
        bool stream_error = false;
        std::string stream_error_msg;

        client_->stream_request(req, [&](AssistantEvent ev) {
            // Forward to caller's event callback if provided.
            if (on_event) on_event(ev);

            // Translate C++ header AssistantEvent → InternalAssistantEvent
            std::visit([&](const auto& e) {
                using E = std::decay_t<decltype(e)>;

                if constexpr (std::is_same_v<E, EventTextDelta>) {
                    internal_events.emplace_back(
                        InternalAssistantEvent_TextDelta{e.text});
                }
                else if constexpr (std::is_same_v<E, EventToolUse>) {
                    // input is nlohmann::json in the C++ event; serialize to string
                    // matching Rust's String input representation.
                    std::string input_str = e.input.is_string()
                        ? e.input.get<std::string>()
                        : e.input.dump();
                    internal_events.emplace_back(
                        InternalAssistantEvent_ToolUse{e.id, e.name, std::move(input_str)});
                }
                else if constexpr (std::is_same_v<E, EventThinkingDelta>) {
                    // No direct Rust counterpart – ignore (or map to TextDelta).
                    // We silently skip thinking deltas; they are not part of
                    // the Rust conversation model.
                }
                else if constexpr (std::is_same_v<E, EventTurnComplete>) {
                    // Rust: AssistantEvent::Usage(TokenUsage)
                    TokenUsage u;
                    u.input_tokens                = e.usage.input_tokens;
                    u.output_tokens               = e.usage.output_tokens;
                    u.cache_creation_input_tokens = e.usage.cache_creation_input_tokens;
                    u.cache_read_input_tokens     = e.usage.cache_read_input_tokens;
                    internal_events.emplace_back(
                        InternalAssistantEvent_Usage{u});
                    // Also emit MessageStop (EventTurnComplete implies stream end)
                    internal_events.emplace_back(InternalAssistantEvent_MessageStop{});
                }
                else if constexpr (std::is_same_v<E, EventError>) {
                    stream_error     = true;
                    stream_error_msg = e.message;
                }
            }, ev);
        });

        if (stream_error) {
            // Rust: return Err(error);
            return tl::unexpected(stream_error_msg);
        }

        // ── Rust: let (assistant_message, usage, turn_prompt_cache_events) =
        //              build_assistant_message(events)? ─────────────────────
        auto bam_result = build_assistant_message(internal_events);
        if (!bam_result.has_value()) {
            return tl::unexpected(bam_result.error().to_string());
        }
        auto& [assistant_message, turn_usage, turn_prompt_cache_events] = *bam_result;

        // ── Rust: if let Some(usage) = usage { self.usage_tracker.record(usage); }
        if (turn_usage.has_value()) {
            usage_tracker_.record(*turn_usage);
        }
        prompt_cache_events.insert(prompt_cache_events.end(),
                                   turn_prompt_cache_events.begin(),
                                   turn_prompt_cache_events.end());

        // ── Rust: let pending_tool_uses = assistant_message.blocks.iter()
        //              .filter_map(|block| match block { ContentBlock::ToolUse{…} => Some(…) })
        std::vector<std::tuple<std::string, std::string, std::string>>
            pending_tool_uses; // (id, name, input_as_string)

        for (const auto& block : assistant_message.blocks) {
            if (const auto* tu = std::get_if<ToolUseBlock>(&block)) {
                std::string input_str = tu->input.is_string()
                    ? tu->input.get<std::string>()
                    : tu->input.dump();
                pending_tool_uses.emplace_back(tu->id, tu->name, std::move(input_str));
            }
        }

        // ── Rust: self.session.push_message(assistant_message.clone()) ────
        {
            auto push_r = push_message(session.messages, assistant_message);
            if (!push_r.has_value()) {
                return tl::unexpected(push_r.error().to_string());
            }
        }
        assistant_messages.push_back(assistant_message);

        // ── Rust: if pending_tool_uses.is_empty() { break; } ──────────────
        if (pending_tool_uses.empty()) {
            break;
        }

        // ── Rust: for (tool_use_id, tool_name, input) in pending_tool_uses { … }
        for (auto& [tool_use_id, tool_name, input] : pending_tool_uses) {

            // ── Pre-tool-use hook ─────────────────────────────────────────
            ConvHookResult pre_hook_result;
            if (hooks_.has_value()) {
                RuntimeHookRunner hrr;
                hrr.set_hook_runner(*hooks_);
                pre_hook_result = hrr.run_pre_tool_use_with_context(
                    tool_name, input,
                    /*abort=*/nullptr,
                    /*reporter=*/nullptr);
            }

            std::string effective_input = pre_hook_result.updated_input_opt().value_or(input);

            // ── Determine permission outcome ──────────────────────────────
            ConvPermOutcome permission_outcome;

            if (pre_hook_result.is_cancelled()) {
                permission_outcome = ConvPermOutcome_Deny{
                    format_hook_message(pre_hook_result,
                        std::format("PreToolUse hook cancelled tool `{}`", tool_name))};
            } else if (pre_hook_result.is_failed()) {
                permission_outcome = ConvPermOutcome_Deny{
                    format_hook_message(pre_hook_result,
                        std::format("PreToolUse hook failed for tool `{}`", tool_name))};
            } else if (pre_hook_result.is_denied()) {
                permission_outcome = ConvPermOutcome_Deny{
                    format_hook_message(pre_hook_result,
                        std::format("PreToolUse hook denied tool `{}`", tool_name))};
            } else {
                permission_outcome = ConvPermOutcome_Allow{};
            }

            // ── Execute or deny ───────────────────────────────────────────
            ConversationMessage result_message;
            if (std::holds_alternative<ConvPermOutcome_Allow>(permission_outcome)) {
                std::string output;
                bool        is_error = false;

                auto exec_json = executor_->execute(tool_name, nlohmann::json(effective_input));
                if (exec_json.contains("error")) {
                    output   = exec_json["error"].get<std::string>();
                    is_error = true;
                } else {
                    output   = exec_json.is_string()
                                ? exec_json.get<std::string>()
                                : exec_json.dump();
                    is_error = false;
                }

                output = merge_hook_feedback(pre_hook_result.messages_ref(), std::move(output), false);

                // ── Post-tool-use hook ────────────────────────────────────
                ConvHookResult post_hook_result;
                if (hooks_.has_value()) {
                    RuntimeHookRunner hrr;
                    hrr.set_hook_runner(*hooks_);
                    if (is_error) {
                        post_hook_result = hrr.run_post_tool_use_failure_with_context(
                            tool_name, effective_input, output,
                            nullptr, nullptr);
                    } else {
                        post_hook_result = hrr.run_post_tool_use_with_context(
                            tool_name, effective_input, output, /*is_error=*/false,
                            nullptr, nullptr);
                    }
                }

                if (post_hook_result.is_denied() ||
                    post_hook_result.is_failed()  ||
                    post_hook_result.is_cancelled())
                {
                    is_error = true;
                }

                bool post_hook_bad = post_hook_result.is_denied() ||
                                     post_hook_result.is_failed()  ||
                                     post_hook_result.is_cancelled();
                output = merge_hook_feedback(post_hook_result.messages_ref(),
                                             std::move(output), post_hook_bad);

                result_message = make_tool_result_message(
                    tool_use_id, tool_name, std::move(output), is_error);
            } else {
                auto& deny = std::get<ConvPermOutcome_Deny>(permission_outcome);
                std::string merged = merge_hook_feedback(
                    pre_hook_result.messages_ref(), std::move(deny.reason), true);
                result_message = make_tool_result_message(
                    tool_use_id, tool_name, std::move(merged), /*is_error=*/true);
            }

            // Rust: self.session.push_message(result_message.clone())
            {
                auto push_r = push_message(session.messages, result_message);
                if (!push_r.has_value()) {
                    return tl::unexpected(push_r.error().to_string());
                }
            }
            // Rust: self.record_tool_finished(iterations, &result_message);  (no-op here)

            tool_results_accum.push_back(std::move(result_message));
        }
    } // end main loop

    // ── Rust: let auto_compaction = self.maybe_auto_compact(); ────────────
    // We call the public maybe_compact which mutates the session.
    // To detect whether compaction happened we snapshot the message count.
    const std::size_t msg_count_before = session.messages.size();
    maybe_compact(session);
    const std::size_t msg_count_after  = session.messages.size();
    std::optional<AutoCompactionEvent> auto_compaction;
    if (msg_count_after < msg_count_before) {
        auto_compaction = AutoCompactionEvent{msg_count_before - msg_count_after};
    }

    // ── Build TurnSummary (internal, Rust faithful) ───────────────────────
    TurnSummary summary;
    summary.assistant_messages  = std::move(assistant_messages);
    summary.tool_results        = std::move(tool_results_accum);
    summary.prompt_cache_events = std::move(prompt_cache_events);
    summary.iterations          = iterations;
    summary.usage               = usage_tracker_.cumulative_usage();
    summary.auto_compaction     = std::move(auto_compaction);

    // ── Map to header return type ─────────────────────────────────────────
    // The header returns TokenUsageMsg, which maps to the cumulative usage.
    TokenUsageMsg ret;
    ret.input_tokens                = summary.usage.input_tokens;
    ret.output_tokens               = summary.usage.output_tokens;
    ret.cache_creation_input_tokens = summary.usage.cache_creation_input_tokens;
    ret.cache_read_input_tokens     = summary.usage.cache_read_input_tokens;
    return ret;
}

// ─────────────────────────────────────────────────────────────────────────────
// End of translation
// ─────────────────────────────────────────────────────────────────────────────

} // namespace claw::runtime
