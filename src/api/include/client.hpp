#pragma once

// ---------------------------------------------------------------------------
// client.hpp  -  ProviderClient and MessageStream (unified dispatch layer)
//
// Mirrors client.rs: ProviderClient is a std::variant over AnthropicClient,
// OpenAiCompatClient(xai), OpenAiCompatClient(openai).  MessageStream is the
// corresponding unified stream type.
// ---------------------------------------------------------------------------

#include "error.hpp"
#include "prompt_cache.hpp"
#include "types.hpp"
#include "providers/mod.hpp"
#include "providers/anthropic.hpp"
#include "providers/openai_compat.hpp"

#include <future>
#include <optional>
#include <string>
#include <variant>

namespace claw::api {

// ---------------------------------------------------------------------------
// MessageStream  (unified stream that delegates to the right provider)
// ---------------------------------------------------------------------------

class MessageStream {
public:
    using AnthropicStream     = AnthropicMessageStream;
    using OpenAiCompatStream  = OpenAiCompatMessageStream;

    explicit MessageStream(AnthropicStream s)    : inner_(std::move(s)) {}
    explicit MessageStream(OpenAiCompatStream s) : inner_(std::move(s)) {}

    [[nodiscard]] std::optional<std::string_view> request_id() const noexcept;
    [[nodiscard]] std::optional<StreamEvent>      next_event();

private:
    std::variant<AnthropicStream, OpenAiCompatStream> inner_;
};

// ---------------------------------------------------------------------------
// ProviderClient  (std::variant over all concrete client types)
// ---------------------------------------------------------------------------

class ProviderClient {
public:
    // Factory: auto-detect provider from model name.
    [[nodiscard]] static ProviderClient from_model(std::string_view model);

    // Factory: auto-detect provider, optionally supplying an explicit
    // Anthropic AuthSource instead of reading environment variables.
    [[nodiscard]] static ProviderClient from_model_with_anthropic_auth(
        std::string_view model,
        std::optional<AuthSource> anthropic_auth);

    // ── Queries ──────────────────────────────────────────────────────────────

    [[nodiscard]] ProviderKind provider_kind() const noexcept;

    // Attach a prompt cache (only meaningful for the Anthropic provider).
    [[nodiscard]] ProviderClient with_prompt_cache(PromptCache cache) &&;

    [[nodiscard]] std::optional<PromptCacheStats>  prompt_cache_stats() const;
    [[nodiscard]] std::optional<PromptCacheRecord> take_last_prompt_cache_record();

    // ── API calls ────────────────────────────────────────────────────────────

    [[nodiscard]] std::future<MessageResponse> send_message(const MessageRequest& request);
    [[nodiscard]] std::future<MessageStream>   stream_message(const MessageRequest& request);

private:
    using Inner = std::variant<AnthropicClient,
                               OpenAiCompatClient>;
    ProviderKind provider_kind_{ProviderKind::Anthropic};
    Inner inner_;

    explicit ProviderClient(AnthropicClient c)
        : provider_kind_(ProviderKind::Anthropic), inner_(std::move(c)) {}
    explicit ProviderClient(OpenAiCompatClient c, ProviderKind k)
        : provider_kind_(k), inner_(std::move(c)) {}
};

// ---------------------------------------------------------------------------
// Free helpers re-exported from anthropic.hpp / openai_compat.hpp
// ---------------------------------------------------------------------------

[[nodiscard]] std::string read_base_url();
[[nodiscard]] std::string read_xai_base_url();

} // namespace claw::api