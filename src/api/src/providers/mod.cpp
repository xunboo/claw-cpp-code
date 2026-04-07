// ---------------------------------------------------------------------------
// providers/mod.cpp  -  provider registry, alias resolution, detection
// ---------------------------------------------------------------------------
#include "providers/mod.hpp"
#include "providers/anthropic.hpp"
#include "providers/openai_compat.hpp"
#include "types.hpp"
#include <algorithm>
#include <string>
#include <nlohmann/json.hpp>

namespace claw::api {

struct RegistryEntry {
    std::string_view alias;
    ProviderMetadata meta;
};

static constexpr RegistryEntry MODEL_REGISTRY[] = {
    {"opus",     {ProviderKind::Anthropic,"ANTHROPIC_API_KEY","ANTHROPIC_BASE_URL",detail::ANTHROPIC_DEFAULT_BASE_URL}},
    {"sonnet",   {ProviderKind::Anthropic,"ANTHROPIC_API_KEY","ANTHROPIC_BASE_URL",detail::ANTHROPIC_DEFAULT_BASE_URL}},
    {"haiku",    {ProviderKind::Anthropic,"ANTHROPIC_API_KEY","ANTHROPIC_BASE_URL",detail::ANTHROPIC_DEFAULT_BASE_URL}},
    {"grok",     {ProviderKind::Xai,"XAI_API_KEY","XAI_BASE_URL",detail::XAI_DEFAULT_BASE_URL}},
    {"grok-3",   {ProviderKind::Xai,"XAI_API_KEY","XAI_BASE_URL",detail::XAI_DEFAULT_BASE_URL}},
    {"grok-mini",{ProviderKind::Xai,"XAI_API_KEY","XAI_BASE_URL",detail::XAI_DEFAULT_BASE_URL}},
    {"grok-3-mini",{ProviderKind::Xai,"XAI_API_KEY","XAI_BASE_URL",detail::XAI_DEFAULT_BASE_URL}},
    {"grok-2",   {ProviderKind::Xai,"XAI_API_KEY","XAI_BASE_URL",detail::XAI_DEFAULT_BASE_URL}},
};

std::string resolve_model_alias(std::string_view model) {
    std::string trimmed{model};
    // strip leading/trailing whitespace
    auto lt = trimmed.find_first_not_of(" \t\n\r");
    if (lt == std::string::npos) return trimmed;
    trimmed = trimmed.substr(lt);
    auto rt = trimmed.find_last_not_of(" \t\n\r");
    if (rt != std::string::npos) trimmed = trimmed.substr(0, rt+1);

    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return std::tolower(c); });

    for (auto& entry : MODEL_REGISTRY) {
        if (entry.alias != lower) continue;
        switch (entry.meta.provider) {
            case ProviderKind::Anthropic:
                if (lower=="opus")   return "claude-opus-4-6";
                if (lower=="sonnet") return "claude-sonnet-4-6";
                if (lower=="haiku")  return "claude-haiku-4-5-20251213";
                return trimmed;
            case ProviderKind::Xai:
                if (lower=="grok"||lower=="grok-3")          return "grok-3";
                if (lower=="grok-mini"||lower=="grok-3-mini") return "grok-3-mini";
                if (lower=="grok-2")                          return "grok-2";
                return trimmed;
            default: return trimmed;
        }
    }
    return trimmed;
}

std::optional<ProviderMetadata> metadata_for_model(std::string_view model) {
    auto canonical = resolve_model_alias(model);
    if (canonical.rfind("claude",0)==0)
        return ProviderMetadata{ProviderKind::Anthropic,"ANTHROPIC_API_KEY","ANTHROPIC_BASE_URL",detail::ANTHROPIC_DEFAULT_BASE_URL};
    if (canonical.rfind("grok",0)==0)
        return ProviderMetadata{ProviderKind::Xai,"XAI_API_KEY","XAI_BASE_URL",detail::XAI_DEFAULT_BASE_URL};
    return std::nullopt;
}

ProviderKind detect_provider_kind(std::string_view model) {
    if (auto meta = metadata_for_model(model)) return meta->provider;
    if (AnthropicClient::has_auth_from_env_or_saved()) return ProviderKind::Anthropic;
    if (openai_has_api_key("OPENAI_API_KEY"))           return ProviderKind::OpenAi;
    if (openai_has_api_key("XAI_API_KEY"))              return ProviderKind::Xai;
    return ProviderKind::Anthropic;
}

uint32_t max_tokens_for_model(std::string_view model) {
    auto limit = model_token_limit(model);
    if (limit) return limit->max_output_tokens;
    auto canonical = resolve_model_alias(model);
    return (canonical.find("opus") != std::string::npos) ? 32'000u : 64'000u;
}

std::optional<ModelTokenLimit> model_token_limit(std::string_view model) {
    auto canonical = resolve_model_alias(model);
    if (canonical == "claude-opus-4-6")
        return ModelTokenLimit{32'000u, 200'000u};
    if (canonical == "claude-sonnet-4-6" || canonical == "claude-haiku-4-5-20251213")
        return ModelTokenLimit{64'000u, 200'000u};
    if (canonical == "grok-3" || canonical == "grok-3-mini")
        return ModelTokenLimit{64'000u, 131'072u};
    return std::nullopt;
}

namespace {

uint32_t estimate_serialized_tokens(const nlohmann::json& value) {
    auto bytes = value.dump();
    return static_cast<uint32_t>(bytes.size() / 4 + 1);
}

uint32_t estimate_message_request_input_tokens(const MessageRequest& request) {
    uint32_t estimate = 0;
    // messages
    {
        nlohmann::json j = request.messages;
        estimate += estimate_serialized_tokens(j);
    }
    // system
    if (request.system) {
        nlohmann::json j = *request.system;
        estimate += estimate_serialized_tokens(j);
    }
    // tools
    if (request.tools) {
        nlohmann::json j = *request.tools;
        estimate += estimate_serialized_tokens(j);
    }
    // tool_choice
    if (request.tool_choice) {
        nlohmann::json j = *request.tool_choice;
        estimate += estimate_serialized_tokens(j);
    }
    return estimate;
}

} // anonymous namespace

void preflight_message_request(const MessageRequest& request) {
    auto limit = model_token_limit(request.model);
    if (!limit) return;

    uint32_t estimated_input_tokens = estimate_message_request_input_tokens(request);
    uint32_t estimated_total_tokens =
        (estimated_input_tokens <= UINT32_MAX - request.max_tokens)
            ? estimated_input_tokens + request.max_tokens
            : UINT32_MAX; // saturating add
    if (estimated_total_tokens > limit->context_window_tokens) {
        throw ApiError(ApiError::context_window_exceeded(
            resolve_model_alias(request.model),
            estimated_input_tokens,
            request.max_tokens,
            estimated_total_tokens,
            limit->context_window_tokens));
    }
}

} // namespace claw::api