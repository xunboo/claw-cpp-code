#pragma once
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <filesystem>
#include <cstdint>
#include <tl/expected.hpp>
#include <nlohmann/json.hpp>

namespace claw::runtime {

/// Speaker role associated with a persisted conversation message.
enum class MessageRole { User, Assistant };

/// Structured message content stored inside a Session.
struct TextBlock    { std::string text; };
struct ToolUseBlock { std::string id; std::string name; nlohmann::json input; };
struct ToolResultBlock {
    std::string tool_use_id;
    std::string content;
    bool is_error{false};
};
struct ThinkingBlock { std::string thinking; std::string signature; };

using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock, ThinkingBlock>;

struct TokenUsageMsg {
    uint32_t input_tokens{0};
    uint32_t output_tokens{0};
    uint32_t cache_creation_input_tokens{0};
    uint32_t cache_read_input_tokens{0};
};

/// One conversation message with optional token-usage metadata.
struct ConversationMessage {
    MessageRole role;
    std::vector<ContentBlock> blocks;
    std::optional<TokenUsageMsg> usage;
};

// Maximum JSONL file size before rotation (~256 KB)
inline constexpr std::size_t SESSION_ROTATION_BYTES = 256 * 1024;
inline constexpr std::size_t SESSION_MAX_ROTATED     = 3;

/// Persisted conversational state for the runtime and CLI session manager.
struct Session {
    std::string id;
    std::string model;
    std::string system_prompt;
    std::vector<ConversationMessage> messages;
    std::optional<std::string> parent_session_id;
    std::optional<std::string> branch_name;

    Session() = default;
    explicit Session(std::string id_) : id(std::move(id_)) {}

    // Serialize all messages to JSONL, appending to path; rotates if size exceeds limit
    [[nodiscard]] tl::expected<void, std::string> persist(const std::filesystem::path& path) const;

    // Load a session from a JSONL file
    [[nodiscard]] static tl::expected<Session, std::string> load(const std::filesystem::path& path);

    // Fork: create a child session sharing parent's messages
    [[nodiscard]] Session fork(std::string new_id) const;
};

// JSON serialization helpers
[[nodiscard]] nlohmann::json content_block_to_json(const ContentBlock& block);
[[nodiscard]] ContentBlock content_block_from_json(const nlohmann::json& j);
[[nodiscard]] nlohmann::json message_to_json(const ConversationMessage& msg);
[[nodiscard]] ConversationMessage message_from_json(const nlohmann::json& j);

} // namespace claw::runtime
