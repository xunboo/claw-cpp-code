#pragma once
#include "session.hpp"
#include "usage.hpp"
#include "compact.hpp"
#include "hooks.hpp"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <tl/expected.hpp>
#include <memory>
#include <nlohmann/json.hpp>

namespace claw::runtime {

struct ApiRequest {
    std::string model;
    std::string system_prompt;
    std::vector<ConversationMessage> messages;
    std::optional<std::size_t> max_tokens;
    bool stream{true};
    nlohmann::json tool_definitions;
};

// Events emitted during a conversation turn
struct EventTextDelta       { std::string text; };
struct EventToolUse         { std::string id; std::string name; nlohmann::json input; };
struct EventThinkingDelta   { std::string thinking; };
struct EventTurnComplete    { TokenUsageMsg usage; };
struct EventError           { std::string message; };

using AssistantEvent = std::variant<EventTextDelta, EventToolUse, EventThinkingDelta, EventTurnComplete, EventError>;

using EventCallback = std::function<void(AssistantEvent)>;

// Abstract API client
class ApiClient {
public:
    virtual ~ApiClient() = default;
    virtual void stream_request(const ApiRequest& req, EventCallback callback) = 0;
};

// Abstract tool executor
class ToolExecutor {
public:
    virtual ~ToolExecutor() = default;
    virtual nlohmann::json execute(std::string_view tool_name, const nlohmann::json& input) = 0;
};

// Static tool executor: maps tool name → function
class StaticToolExecutor : public ToolExecutor {
public:
    using ToolFn = std::function<nlohmann::json(const nlohmann::json&)>;

    void register_tool(std::string name, ToolFn fn);
    nlohmann::json execute(std::string_view tool_name, const nlohmann::json& input) override;

private:
    std::unordered_map<std::string, ToolFn> tools_;
};

struct ConversationConfig {
    std::string model;
    std::optional<std::size_t> max_tokens;
    CompactionConfig compaction;
    std::optional<std::size_t> auto_compact_threshold;
    bool enable_hooks{false};
};

class ConversationRuntime {
public:
    ConversationRuntime(std::shared_ptr<ApiClient> client,
                        std::shared_ptr<ToolExecutor> executor,
                        ConversationConfig config,
                        std::optional<std::shared_ptr<HookRunner>> hooks = std::nullopt);

    // Run one conversation turn: append user message, call API, execute tools, return
    [[nodiscard]] tl::expected<TokenUsageMsg, std::string>
        run_turn(Session& session, std::string_view user_message, EventCallback on_event = nullptr);

    // Check and apply compaction if threshold exceeded
    void maybe_compact(Session& session);

private:
    std::shared_ptr<ApiClient> client_;
    std::shared_ptr<ToolExecutor> executor_;
    ConversationConfig config_;
    std::optional<std::shared_ptr<HookRunner>> hooks_;
    UsageTracker usage_tracker_;
};

// Read CLAW_AUTO_COMPACT_THRESHOLD env variable
[[nodiscard]] std::optional<std::size_t> auto_compaction_threshold_from_env();

} // namespace claw::runtime

