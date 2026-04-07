#pragma once

// ---------------------------------------------------------------------------
// providers/mod.hpp  -  provider registry, alias resolution, detection
// ---------------------------------------------------------------------------

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../error.hpp"

// Forward declaration for preflight_message_request
namespace claw::api { struct MessageRequest; }

namespace claw::api {

// ---------------------------------------------------------------------------
// ProviderKind
// ---------------------------------------------------------------------------

enum class ProviderKind { Anthropic, Xai, OpenAi };

// ---------------------------------------------------------------------------
// ProviderMetadata
// ---------------------------------------------------------------------------

struct ProviderMetadata {
    ProviderKind     provider;
    std::string_view auth_env;
    std::string_view base_url_env;
    std::string_view default_base_url;
};

// ---------------------------------------------------------------------------
// Free functions (mirrors providers/mod.rs public API)
// ---------------------------------------------------------------------------

/// Resolve a human-friendly model alias such as "opus" or "grok" to a
/// canonical model identifier.
[[nodiscard]] std::string resolve_model_alias(std::string_view model);

/// Return the ProviderMetadata entry for a canonical model name, or
/// std::nullopt when the model is not recognised.
[[nodiscard]] std::optional<ProviderMetadata> metadata_for_model(std::string_view model);

/// Infer the ProviderKind from a model name (falling back to env-var
/// presence checks and ultimately Anthropic).
[[nodiscard]] ProviderKind detect_provider_kind(std::string_view model);

/// Return the maximum output-token limit appropriate for a model.
[[nodiscard]] uint32_t max_tokens_for_model(std::string_view model);

// ---------------------------------------------------------------------------
// ModelTokenLimit  — context-window metadata for known models
// ---------------------------------------------------------------------------

struct ModelTokenLimit {
    uint32_t max_output_tokens{0};
    uint32_t context_window_tokens{0};
};

/// Return the ModelTokenLimit for a known model, or std::nullopt for
/// unrecognised models.
[[nodiscard]] std::optional<ModelTokenLimit> model_token_limit(std::string_view model);

/// Heuristic preflight check: reject a request whose estimated token
/// count exceeds the model's context window.  Returns Ok for unknown
/// models.
void preflight_message_request(const MessageRequest& request);

// ---------------------------------------------------------------------------
// Internal helpers used by provider implementations
// ---------------------------------------------------------------------------

namespace detail {
/// Default Anthropic base URL constant (defined in anthropic.hpp but
/// declared here so providers/mod.cpp can reference it without a circular
/// dependency).
inline constexpr std::string_view ANTHROPIC_DEFAULT_BASE_URL = "https://api.anthropic.com";
inline constexpr std::string_view XAI_DEFAULT_BASE_URL       = "https://api.x.ai/v1";
} // namespace detail

} // namespace claw::api