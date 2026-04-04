// ---------------------------------------------------------------------------
// providers/mod.cpp  -  provider registry, alias resolution, detection
// ---------------------------------------------------------------------------
#include "providers/mod.hpp"
#include "providers/anthropic.hpp"
#include "providers/openai_compat.hpp"
#include <algorithm>
#include <string>

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
    auto canonical = resolve_model_alias(model);
    return (canonical.find("opus") != std::string::npos) ? 32'000u : 64'000u;
}

} // namespace claw::api