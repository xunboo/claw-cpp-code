#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace claw::api {

// ---------------------------------------------------------------------------
// ToolDefinition
// ---------------------------------------------------------------------------

struct ToolDefinition {
    std::string name;
    std::optional<std::string> description;
    nlohmann::json input_schema;
};

void to_json(nlohmann::json& j, const ToolDefinition& t);
void from_json(const nlohmann::json& j, ToolDefinition& t);

// ---------------------------------------------------------------------------
// ToolChoice  (Rust enum with data -> std::variant-like tagged struct)
// ---------------------------------------------------------------------------

struct ToolChoice {
    enum class Kind { Auto, Any, Tool };
    Kind kind{Kind::Auto};
    std::string name; // only when kind == Tool

    [[nodiscard]] static ToolChoice auto_() { return {Kind::Auto, {}}; }
    [[nodiscard]] static ToolChoice any()   { return {Kind::Any,  {}}; }
    [[nodiscard]] static ToolChoice tool(std::string n) { return {Kind::Tool, std::move(n)}; }
};

void to_json(nlohmann::json& j, const ToolChoice& tc);
void from_json(const nlohmann::json& j, ToolChoice& tc);

// ---------------------------------------------------------------------------
// ToolResultContentBlock
// ---------------------------------------------------------------------------

struct ToolResultContentBlock {
    enum class Kind { Text, Json };
    Kind kind{Kind::Text};
    std::string text;
    nlohmann::json value; // only when kind == Json
};

void to_json(nlohmann::json& j, const ToolResultContentBlock& b);
void from_json(const nlohmann::json& j, ToolResultContentBlock& b);

// ---------------------------------------------------------------------------
// InputContentBlock
// ---------------------------------------------------------------------------

struct InputContentBlock {
    enum class Kind { Text, ToolUse, ToolResult };
    Kind kind{Kind::Text};

    // Text
    std::string text;

    // ToolUse
    std::string id;
    std::string name;
    nlohmann::json input;

    // ToolResult
    std::string tool_use_id;
    std::vector<ToolResultContentBlock> content;
    bool is_error{false};

    [[nodiscard]] static InputContentBlock text_block(std::string t) {
        InputContentBlock b;
        b.kind = Kind::Text;
        b.text = std::move(t);
        return b;
    }
};

void to_json(nlohmann::json& j, const InputContentBlock& b);
void from_json(const nlohmann::json& j, InputContentBlock& b);

// ---------------------------------------------------------------------------
// InputMessage
// ---------------------------------------------------------------------------

struct InputMessage {
    std::string role;
    std::vector<InputContentBlock> content;

    [[nodiscard]] static InputMessage user_text(std::string text);
    [[nodiscard]] static InputMessage user_tool_result(
        std::string tool_use_id,
        std::string content_text,
        bool is_error);
};

void to_json(nlohmann::json& j, const InputMessage& m);
void from_json(const nlohmann::json& j, InputMessage& m);

// ---------------------------------------------------------------------------
// MessageRequest
// ---------------------------------------------------------------------------

struct MessageRequest {
    std::string model;
    uint32_t max_tokens{0};
    std::vector<InputMessage> messages;
    std::optional<std::string> system;
    std::optional<std::vector<ToolDefinition>> tools;
    std::optional<ToolChoice> tool_choice;
    bool stream{false};

    [[nodiscard]] MessageRequest with_streaming() const {
        MessageRequest copy = *this;
        copy.stream = true;
        return copy;
    }
};

void to_json(nlohmann::json& j, const MessageRequest& r);
void from_json(const nlohmann::json& j, MessageRequest& r);

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

struct Usage {
    uint32_t input_tokens{0};
    uint32_t cache_creation_input_tokens{0};
    uint32_t cache_read_input_tokens{0};
    uint32_t output_tokens{0};

    [[nodiscard]] constexpr uint32_t total_tokens() const noexcept {
        return input_tokens + output_tokens +
               cache_creation_input_tokens + cache_read_input_tokens;
    }
};

void to_json(nlohmann::json& j, const Usage& u);
void from_json(const nlohmann::json& j, Usage& u);

// ---------------------------------------------------------------------------
// OutputContentBlock
// ---------------------------------------------------------------------------

struct OutputContentBlock {
    enum class Kind { Text, ToolUse, Thinking, RedactedThinking };
    Kind kind{Kind::Text};

    std::string text;            // Text / Thinking
    std::optional<std::string> signature; // Thinking
    std::string id;              // ToolUse
    std::string name;            // ToolUse
    nlohmann::json input;        // ToolUse
    nlohmann::json data;         // RedactedThinking
};

void to_json(nlohmann::json& j, const OutputContentBlock& b);
void from_json(const nlohmann::json& j, OutputContentBlock& b);

// ---------------------------------------------------------------------------
// MessageResponse
// ---------------------------------------------------------------------------

struct MessageResponse {
    std::string id;
    std::string kind;
    std::string role;
    std::vector<OutputContentBlock> content;
    std::string model;
    std::optional<std::string> stop_reason;
    std::optional<std::string> stop_sequence;
    Usage usage;
    std::optional<std::string> request_id;

    [[nodiscard]] uint32_t total_tokens() const noexcept { return usage.total_tokens(); }
};

void to_json(nlohmann::json& j, const MessageResponse& r);
void from_json(const nlohmann::json& j, MessageResponse& r);

// ---------------------------------------------------------------------------
// Stream event types
// ---------------------------------------------------------------------------

struct MessageStartEvent {
    MessageResponse message;
};

struct MessageDelta {
    std::optional<std::string> stop_reason;
    std::optional<std::string> stop_sequence;
};

struct MessageDeltaEvent {
    MessageDelta delta;
    Usage usage;
};

struct MessageStopEvent {};

struct ContentBlockStartEvent {
    uint32_t index{0};
    OutputContentBlock content_block;
};

struct ContentBlockDelta {
    enum class Kind { TextDelta, InputJsonDelta, ThinkingDelta, SignatureDelta };
    Kind kind{Kind::TextDelta};
    std::string text;          // TextDelta / ThinkingDelta / SignatureDelta
    std::string partial_json;  // InputJsonDelta
};

void to_json(nlohmann::json& j, const ContentBlockDelta& d);
void from_json(const nlohmann::json& j, ContentBlockDelta& d);

struct ContentBlockDeltaEvent {
    uint32_t index{0};
    ContentBlockDelta delta;
};

struct ContentBlockStopEvent {
    uint32_t index{0};
};

// ---------------------------------------------------------------------------
// StreamEvent  (Rust enum -> tagged union via variant Kind + union of fields)
// ---------------------------------------------------------------------------

struct StreamEvent {
    enum class Kind {
        MessageStart,
        MessageDelta,
        ContentBlockStart,
        ContentBlockDelta,
        ContentBlockStop,
        MessageStop,
    };
    Kind kind{Kind::MessageStart};

    // Only one of these is populated at a time, matching the active Kind.
    MessageStartEvent      message_start;
    MessageDeltaEvent      message_delta;
    ContentBlockStartEvent content_block_start;
    ContentBlockDeltaEvent content_block_delta;
    ContentBlockStopEvent  content_block_stop;
    MessageStopEvent       message_stop;
};

void from_json(const nlohmann::json& j, StreamEvent& e);
void to_json(nlohmann::json& j, const StreamEvent& e);

} // namespace claw::api