#pragma once

// ---------------------------------------------------------------------------
// providers/anthropic.hpp  -  AnthropicClient and related types
// ---------------------------------------------------------------------------

#include "../error.hpp"
#include "../prompt_cache.hpp"
#include "../sse.hpp"
#include "../types.hpp"
#include "telemetry.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace claw::api {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

inline constexpr std::string_view ANTHROPIC_DEFAULT_BASE_URL = "https://api.anthropic.com";
inline constexpr std::string_view ANTHROPIC_API_VERSION      = "2023-06-01";

// ---------------------------------------------------------------------------
// OAuthTokenSet
// ---------------------------------------------------------------------------

struct OAuthTokenSet {
    std::string access_token;
    std::optional<std::string> refresh_token;
    std::optional<uint64_t>    expires_at;
    std::vector<std::string>   scopes;
};

// ---------------------------------------------------------------------------
// OAuthConfig  (mirrors runtime::OAuthConfig)
// ---------------------------------------------------------------------------

struct OAuthConfig {
    std::string client_id;
    std::string authorize_url;
    std::string token_url;
    std::optional<uint16_t>    callback_port;
    std::optional<std::string> manual_redirect_url;
    std::vector<std::string>   scopes;
};

// ---------------------------------------------------------------------------
// AuthSource  (Rust enum -> tagged union struct)
// ---------------------------------------------------------------------------

struct AuthSource {
    enum class Kind { None, ApiKey, BearerToken, ApiKeyAndBearer };
    Kind kind{Kind::None};
    std::string api_key;      // ApiKey | ApiKeyAndBearer
    std::string bearer_token; // BearerToken | ApiKeyAndBearer

    // Factories
    [[nodiscard]] static AuthSource none() { return {Kind::None, {}, {}}; }
    [[nodiscard]] static AuthSource from_api_key(std::string key) {
        return {Kind::ApiKey, std::move(key), {}};
    }
    [[nodiscard]] static AuthSource from_bearer(std::string token) {
        return {Kind::BearerToken, {}, std::move(token)};
    }
    [[nodiscard]] static AuthSource from_oauth(const OAuthTokenSet& t) {
        return from_bearer(t.access_token);
    }

    /// Read ANTHROPIC_API_KEY and/or ANTHROPIC_AUTH_TOKEN from the environment.
    [[nodiscard]] static AuthSource from_env();
    /// Same as from_env() but also checks the saved OAuth token file.
    [[nodiscard]] static AuthSource from_env_or_saved();

    [[nodiscard]] std::optional<std::string_view> get_api_key() const noexcept;
    [[nodiscard]] std::optional<std::string_view> get_bearer_token() const noexcept;
    [[nodiscard]] std::string_view masked_authorization_header() const noexcept;
};

// ---------------------------------------------------------------------------
// AnthropicMessageStream  (per-request streaming object)
// ---------------------------------------------------------------------------

class AnthropicMessageStream {
public:
    AnthropicMessageStream(
        std::optional<std::string> request_id,
        std::vector<uint8_t>       initial_body,
        // In a real async implementation this would hold a live HTTP stream.
        // Here we buffer the full body for simplicity and iterate over frames.
        std::string                raw_body,
        MessageRequest             request,
        std::optional<PromptCache> prompt_cache,
        std::shared_ptr<std::mutex>                    record_mutex,
        std::shared_ptr<std::optional<PromptCacheRecord>> last_record);

    [[nodiscard]] std::optional<std::string_view> request_id() const noexcept;

    /// Returns the next stream event, or std::nullopt when the stream is done.
    /// In a real async implementation this would be a coroutine / future.
    [[nodiscard]] std::optional<StreamEvent> next_event();

private:
    std::optional<std::string>    request_id_;
    std::string                   raw_body_;
    SseParser                     parser_;
    std::deque<StreamEvent>       pending_;
    bool                          done_{false};
    MessageRequest                request_;
    std::optional<PromptCache>    prompt_cache_;
    std::optional<Usage>          latest_usage_;
    bool                          usage_recorded_{false};
    std::shared_ptr<std::mutex>   record_mutex_;
    std::shared_ptr<std::optional<PromptCacheRecord>> last_record_;
    size_t                        body_pos_{0};

    void observe_event(const StreamEvent& event);
    // Feed the next chunk of raw_body_ into the parser.
    [[nodiscard]] bool pump_next_chunk();
};

// ---------------------------------------------------------------------------
// AnthropicClient
// ---------------------------------------------------------------------------

class AnthropicClient {
public:
    // Constructors / factories
    explicit AnthropicClient(std::string api_key);
    explicit AnthropicClient(AuthSource auth);

    [[nodiscard]] static AnthropicClient from_env();

    // Builder-pattern setters (return *this by value, Rust-style)
    [[nodiscard]] AnthropicClient with_auth_source(AuthSource auth) &&;
    [[nodiscard]] AnthropicClient with_auth_token(std::optional<std::string> token) &&;
    [[nodiscard]] AnthropicClient with_base_url(std::string base_url) &&;
    [[nodiscard]] AnthropicClient with_retry_policy(uint32_t max_retries,
                                                    std::chrono::milliseconds initial_backoff,
                                                    std::chrono::milliseconds max_backoff) &&;
    [[nodiscard]] AnthropicClient with_prompt_cache(PromptCache cache) &&;
    [[nodiscard]] AnthropicClient with_session_tracer(claw::telemetry::SessionTracer tracer) &&;
    [[nodiscard]] AnthropicClient with_client_identity(claw::telemetry::ClientIdentity identity) &&;
    [[nodiscard]] AnthropicClient with_beta(std::string beta) &&;
    [[nodiscard]] AnthropicClient with_extra_body_param(std::string key, nlohmann::json value) &&;
    [[nodiscard]] AnthropicClient with_request_profile(claw::telemetry::AnthropicRequestProfile profile) &&;

    // Accessors
    [[nodiscard]] const AuthSource& auth_source() const noexcept { return auth_; }
    [[nodiscard]] const claw::telemetry::AnthropicRequestProfile& request_profile() const noexcept { return request_profile_; }
    [[nodiscard]] const std::optional<claw::telemetry::SessionTracer>& session_tracer() const noexcept { return session_tracer_; }
    [[nodiscard]] const std::optional<PromptCache>& prompt_cache() const noexcept { return prompt_cache_; }
    [[nodiscard]] std::optional<PromptCacheStats> prompt_cache_stats() const;
    [[nodiscard]] std::optional<PromptCacheRecord> take_last_prompt_cache_record();

    // Blocking send / stream (std::future used instead of Rust async fn)
    [[nodiscard]] std::future<MessageResponse> send_message(const MessageRequest& request);
    [[nodiscard]] std::future<AnthropicMessageStream> stream_message(const MessageRequest& request);

    // OAuth helpers
    [[nodiscard]] std::future<OAuthTokenSet> exchange_oauth_code(
        const OAuthConfig& config,
        const std::string& code,
        const std::string& redirect_uri);
    [[nodiscard]] std::future<OAuthTokenSet> refresh_oauth_token(
        const OAuthConfig& config,
        const std::string& refresh_token,
        const std::vector<std::string>& scopes);

    // Static utilities
    [[nodiscard]] static std::string read_base_url();
    [[nodiscard]] static bool has_auth_from_env_or_saved();
    [[nodiscard]] static bool oauth_token_is_expired(const OAuthTokenSet& token_set);
    [[nodiscard]] static std::optional<OAuthTokenSet> resolve_saved_oauth_token(
        const OAuthConfig& config);
    [[nodiscard]] static AuthSource resolve_startup_auth_source(
        std::function<std::optional<OAuthConfig>()> load_oauth_config);

private:
    AuthSource  auth_;
    std::string base_url_{std::string(ANTHROPIC_DEFAULT_BASE_URL)};
    uint32_t    max_retries_{2};
    std::chrono::milliseconds initial_backoff_{200};
    std::chrono::milliseconds max_backoff_{2000};
    std::optional<PromptCache> prompt_cache_;
    std::optional<claw::telemetry::SessionTracer> session_tracer_;
    claw::telemetry::AnthropicRequestProfile request_profile_{
        claw::telemetry::AnthropicRequestProfile::make_default()};
    std::shared_ptr<std::mutex> record_mutex_{std::make_shared<std::mutex>()};
    std::shared_ptr<std::optional<PromptCacheRecord>> last_record_{
        std::make_shared<std::optional<PromptCacheRecord>>()};

    [[nodiscard]] std::chrono::milliseconds backoff_for_attempt(uint32_t attempt) const;
    void store_last_prompt_cache_record(PromptCacheRecord record);
};

// ---------------------------------------------------------------------------
// Global helpers exposed by Rust lib.rs
// ---------------------------------------------------------------------------

[[nodiscard]] bool oauth_token_is_expired(const OAuthTokenSet& token_set);
[[nodiscard]] std::optional<OAuthTokenSet> resolve_saved_oauth_token(const OAuthConfig& config);
[[nodiscard]] AuthSource resolve_startup_auth_source(
    std::function<std::optional<OAuthConfig>()> load_oauth_config);
[[nodiscard]] std::string read_base_url();

} // namespace claw::api