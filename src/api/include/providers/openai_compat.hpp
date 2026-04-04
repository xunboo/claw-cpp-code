#pragma once

// ---------------------------------------------------------------------------
// providers/openai_compat.hpp  -  OpenAI-compatible provider (xAI, OpenAI)
// ---------------------------------------------------------------------------

#include "../error.hpp"
#include "../types.hpp"

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace claw::api {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

inline constexpr std::string_view DEFAULT_XAI_BASE_URL    = "https://api.x.ai/v1";
inline constexpr std::string_view DEFAULT_OPENAI_BASE_URL = "https://api.openai.com/v1";

// ---------------------------------------------------------------------------
// OpenAiCompatConfig
// ---------------------------------------------------------------------------

struct OpenAiCompatConfig {
    std::string_view provider_name;
    std::string_view api_key_env;
    std::string_view base_url_env;
    std::string_view default_base_url;

    [[nodiscard]] static constexpr OpenAiCompatConfig xai() noexcept {
        return {"xAI", "XAI_API_KEY", "XAI_BASE_URL", DEFAULT_XAI_BASE_URL};
    }
    [[nodiscard]] static constexpr OpenAiCompatConfig openai() noexcept {
        return {"OpenAI", "OPENAI_API_KEY", "OPENAI_BASE_URL", DEFAULT_OPENAI_BASE_URL};
    }

    [[nodiscard]] std::vector<std::string_view> credential_env_vars() const noexcept;
};

// ---------------------------------------------------------------------------
// Internal SSE chunk types  (used by the streaming state machine)
// ---------------------------------------------------------------------------

struct OpenAiUsage {
    uint32_t prompt_tokens{0};
    uint32_t completion_tokens{0};
};

struct DeltaFunction {
    std::optional<std::string> name;
    std::optional<std::string> arguments;
};

struct DeltaToolCall {
    uint32_t index{0};
    std::optional<std::string> id;
    DeltaFunction function;
};

struct ChunkDelta {
    std::optional<std::string>  content;
    std::vector<DeltaToolCall>  tool_calls;
};

struct ChunkChoice {
    ChunkDelta delta;
    std::optional<std::string> finish_reason;
};

struct ChatCompletionChunk {
    std::string id;
    std::optional<std::string>  model;
    std::vector<ChunkChoice>    choices;
    std::optional<OpenAiUsage>  usage;
};

// ---------------------------------------------------------------------------
// OpenAiSseParser  (private helper, exposed for testing)
// ---------------------------------------------------------------------------

class OpenAiSseParser {
public:
    OpenAiSseParser() = default;
    [[nodiscard]] std::vector<ChatCompletionChunk> push(std::string_view chunk);

private:
    std::string buffer_;
};

// ---------------------------------------------------------------------------
// StreamState  (accumulates SSE chunks into Anthropic-shaped StreamEvents)
// ---------------------------------------------------------------------------

struct ToolCallState {
    uint32_t    openai_index{0};
    std::optional<std::string> id;
    std::optional<std::string> name;
    std::string arguments;
    size_t      emitted_len{0};
    bool        started{false};
    bool        stopped{false};

    void apply(const DeltaToolCall& tc);
    [[nodiscard]] uint32_t block_index() const noexcept { return openai_index + 1; }
    [[nodiscard]] std::optional<ContentBlockStartEvent> start_event() const;
    [[nodiscard]] std::optional<ContentBlockDeltaEvent> delta_event();
};

class StreamState {
public:
    explicit StreamState(std::string model);

    [[nodiscard]] std::vector<StreamEvent> ingest_chunk(const ChatCompletionChunk& chunk);
    [[nodiscard]] std::vector<StreamEvent> finish();

private:
    std::string model_;
    bool message_started_{false};
    bool text_started_{false};
    bool text_finished_{false};
    bool finished_{false};
    std::optional<std::string> stop_reason_;
    std::optional<Usage>       usage_;
    std::map<uint32_t, ToolCallState> tool_calls_;
};

// ---------------------------------------------------------------------------
// OpenAiCompatMessageStream
// ---------------------------------------------------------------------------

class OpenAiCompatMessageStream {
public:
    OpenAiCompatMessageStream(
        std::optional<std::string> request_id,
        std::string                raw_body,
        StreamState                state);

    [[nodiscard]] std::optional<std::string_view> request_id() const noexcept;
    [[nodiscard]] std::optional<StreamEvent>      next_event();

private:
    std::optional<std::string>  request_id_;
    std::string                 raw_body_;
    size_t                      body_pos_{0};
    OpenAiSseParser             parser_;
    std::deque<StreamEvent>     pending_;
    bool                        done_{false};
    StreamState                 state_;
};

// ---------------------------------------------------------------------------
// OpenAiCompatClient
// ---------------------------------------------------------------------------

class OpenAiCompatClient {
public:
    OpenAiCompatClient(std::string api_key, OpenAiCompatConfig config);

    [[nodiscard]] static OpenAiCompatClient from_env(OpenAiCompatConfig config);

    [[nodiscard]] OpenAiCompatClient with_base_url(std::string base_url) &&;
    [[nodiscard]] OpenAiCompatClient with_retry_policy(uint32_t max_retries,
                                                       std::chrono::milliseconds initial_backoff,
                                                       std::chrono::milliseconds max_backoff) &&;

    [[nodiscard]] std::optional<std::string_view> request_id_from_last_response() const noexcept;

    [[nodiscard]] MessageResponse           send_message(const MessageRequest& req);
    [[nodiscard]] OpenAiCompatMessageStream stream_message(const MessageRequest& req);

    // Static utilities
    [[nodiscard]] static bool        has_api_key(std::string_view key);
    [[nodiscard]] static std::string read_base_url(OpenAiCompatConfig config);

private:
    std::string         api_key_;
    OpenAiCompatConfig  config_;
    std::string         base_url_;
    uint32_t            max_retries_{2};
    std::chrono::milliseconds initial_backoff_{200};
    std::chrono::milliseconds max_backoff_{2000};
    mutable std::string last_request_id_;

    [[nodiscard]] std::chrono::milliseconds backoff_for_attempt(uint32_t attempt) const;
    [[nodiscard]] static std::string chat_completions_endpoint(std::string_view base_url);
    [[nodiscard]] static std::string normalize_finish_reason(std::string_view value);
};

// ---------------------------------------------------------------------------
// Free helpers (used by providers/mod.cpp)
// ---------------------------------------------------------------------------

[[nodiscard]] std::string read_openai_compat_base_url(OpenAiCompatConfig config);
[[nodiscard]] bool        openai_has_api_key(std::string_view key);
[[nodiscard]] std::string read_xai_base_url();

// Translation helpers (exposed for unit testing)
[[nodiscard]] nlohmann::json build_chat_completion_request(
    const MessageRequest& req, OpenAiCompatConfig config);

} // namespace claw::api