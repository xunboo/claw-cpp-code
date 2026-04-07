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
    uint64_t created_at_ms{0};           // set once at creation, never changes
    mutable uint64_t updated_at_ms{0};  // updated on each touch/persist

    Session() = default;
    explicit Session(std::string id_) : id(std::move(id_)) {}

    /// Full JSONL snapshot: writes all messages atomically (rotates if needed).
    /// Used for bootstrap (first save) and after file rotation.
    [[nodiscard]] tl::expected<void, std::string> persist(const std::filesystem::path& path) const;

    /// Incremental append: only writes messages added since the last persist/append.
    /// Falls back to full persist() if the file doesn't exist or is empty.
    /// Mirrors Rust's append_persisted_message logic.
    [[nodiscard]] tl::expected<void, std::string> append_new_messages(const std::filesystem::path& path);

    /// Load a session from a JSONL file
    [[nodiscard]] static tl::expected<Session, std::string> load(const std::filesystem::path& path);

    /// Fork: create a child session sharing parent's messages.
    /// Generates a new session_id internally (mirrors Rust).
    [[nodiscard]] Session fork(std::string fork_branch_name = {}) const;

    /// Number of messages already persisted to disk.
    [[nodiscard]] std::size_t persisted_count() const noexcept { return persisted_count_; }

    /// Mark all current messages as persisted (called after full persist).
    void mark_all_persisted() noexcept { persisted_count_ = messages.size(); }

private:
    mutable std::size_t persisted_count_{0};
};

/// Generate a unique session ID (format: "session-{millis}-{counter}").
/// Mirrors Rust's generate_session_id().
[[nodiscard]] std::string generate_session_id();

// JSON serialization helpers
[[nodiscard]] nlohmann::json content_block_to_json(const ContentBlock& block);
[[nodiscard]] ContentBlock content_block_from_json(const nlohmann::json& j);
[[nodiscard]] nlohmann::json message_to_json(const ConversationMessage& msg);
[[nodiscard]] ConversationMessage message_from_json(const nlohmann::json& j);

} // namespace claw::runtime
