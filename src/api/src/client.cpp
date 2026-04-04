// ---------------------------------------------------------------------------
// client.cpp  -  ProviderClient and MessageStream unified dispatch
// ---------------------------------------------------------------------------
#include "client.hpp"
#include "providers/mod.hpp"
#include <stdexcept>

namespace claw::api {

// ── MessageStream ─────────────────────────────────────────────────────────────

std::optional<std::string_view> MessageStream::request_id() const noexcept {
    return std::visit([](auto& s) -> std::optional<std::string_view> {
        return s.request_id();
    }, inner_);
}

std::optional<StreamEvent> MessageStream::next_event() {
    return std::visit([](auto& s) -> std::optional<StreamEvent> {
        return s.next_event();
    }, inner_);
}

// ── ProviderClient factories ──────────────────────────────────────────────────

ProviderClient ProviderClient::from_model(std::string_view model) {
    return from_model_with_anthropic_auth(model, std::nullopt);
}

ProviderClient ProviderClient::from_model_with_anthropic_auth(
    std::string_view model, std::optional<AuthSource> anthropic_auth)
{
    auto resolved = resolve_model_alias(model);
    auto kind     = detect_provider_kind(resolved);
    switch (kind) {
        case ProviderKind::Anthropic: {
            AnthropicClient ac = anthropic_auth
                ? AnthropicClient(*anthropic_auth)
                : AnthropicClient::from_env();
            return ProviderClient(std::move(ac));
        }
        case ProviderKind::Xai: {
            auto oc = OpenAiCompatClient::from_env(OpenAiCompatConfig::xai());
            return ProviderClient(std::move(oc), ProviderKind::Xai);
        }
        case ProviderKind::OpenAi: {
            auto oc = OpenAiCompatClient::from_env(OpenAiCompatConfig::openai());
            return ProviderClient(std::move(oc), ProviderKind::OpenAi);
        }
    }
    throw std::runtime_error("Unknown provider kind");
}

// ── ProviderClient queries ────────────────────────────────────────────────────

ProviderKind ProviderClient::provider_kind() const noexcept { return provider_kind_; }

ProviderClient ProviderClient::with_prompt_cache(PromptCache cache) && {
    if (provider_kind_ == ProviderKind::Anthropic) {
        auto& ac = std::get<AnthropicClient>(inner_);
        inner_ = std::move(ac).with_prompt_cache(std::move(cache));
    }
    return std::move(*this);
}

std::optional<PromptCacheStats> ProviderClient::prompt_cache_stats() const {
    if (provider_kind_ == ProviderKind::Anthropic)
        return std::get<AnthropicClient>(inner_).prompt_cache_stats();
    return std::nullopt;
}

std::optional<PromptCacheRecord> ProviderClient::take_last_prompt_cache_record() {
    if (provider_kind_ == ProviderKind::Anthropic)
        return std::get<AnthropicClient>(inner_).take_last_prompt_cache_record();
    return std::nullopt;
}

// ── ProviderClient API calls ──────────────────────────────────────────────────

std::future<MessageResponse> ProviderClient::send_message(const MessageRequest& request) {
    switch (provider_kind_) {
        case ProviderKind::Anthropic:
            return std::get<AnthropicClient>(inner_).send_message(request);
        default:
            // Both xAI and OpenAI slots use OpenAiCompatClient.
            // OpenAiCompatClient::send_message is synchronous; wrap in async.
            return std::visit([&](auto& client) -> std::future<MessageResponse> {
                if constexpr (std::is_same_v<std::decay_t<decltype(client)>, OpenAiCompatClient>)
                    return std::async(std::launch::async,
                        [&client, request]() mutable -> MessageResponse {
                            return client.send_message(request);
                        });
                else
                    return std::async(std::launch::deferred, []()->MessageResponse{
                        throw std::runtime_error("unreachable"); });
            }, inner_);
    }
}

std::future<MessageStream> ProviderClient::stream_message(const MessageRequest& request) {
    switch (provider_kind_) {
        case ProviderKind::Anthropic: {
            auto& ac = std::get<AnthropicClient>(inner_);
            return std::async(std::launch::async, [&ac, request]() mutable {
                auto fut = ac.stream_message(request);
                return MessageStream(fut.get());
            });
        }
        default:
            return std::visit([&](auto& client) -> std::future<MessageStream> {
                if constexpr (std::is_same_v<std::decay_t<decltype(client)>, OpenAiCompatClient>) {
                    return std::async(std::launch::async,
                        [&client, request]() mutable -> MessageStream {
                            return MessageStream(client.stream_message(request));
                        });
                }
                return std::async(std::launch::deferred, []()->MessageStream{
                    throw std::runtime_error("unreachable"); });
            }, inner_);
    }
}

// ── Free helpers ──────────────────────────────────────────────────────────────

std::string read_base_url() { return AnthropicClient::read_base_url(); }
std::string read_xai_base_url() { return read_openai_compat_base_url(OpenAiCompatConfig::xai()); }

} // namespace claw::api