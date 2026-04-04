#include "usage.hpp"
#include <algorithm>
#include <format>

namespace claw::runtime {

namespace {

double cost_for_tokens(uint32_t tokens, double usd_per_million) {
    return static_cast<double>(tokens) / 1'000'000.0 * usd_per_million;
}

} // anonymous namespace

std::optional<ModelPricing> pricing_for_model(std::string_view model) {
    // Normalize to lowercase for comparison
    std::string normalized(model);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (normalized.find("haiku") != std::string::npos) {
        return ModelPricing{1.0, 5.0, 1.25, 0.1};
    }
    if (normalized.find("opus") != std::string::npos) {
        return ModelPricing{15.0, 75.0, 18.75, 1.5};
    }
    if (normalized.find("sonnet") != std::string::npos) {
        return ModelPricing::default_sonnet_tier();
    }
    return std::nullopt;
}

UsageCostEstimate estimate_cost_usd(TokenUsage usage, ModelPricing pricing) {
    return UsageCostEstimate{
        .input_cost_usd = cost_for_tokens(usage.input_tokens, pricing.input_cost_per_million),
        .output_cost_usd = cost_for_tokens(usage.output_tokens, pricing.output_cost_per_million),
        .cache_creation_cost_usd = cost_for_tokens(usage.cache_creation_input_tokens, pricing.cache_creation_cost_per_million),
        .cache_read_cost_usd = cost_for_tokens(usage.cache_read_input_tokens, pricing.cache_read_cost_per_million),
    };
}

std::string format_usd(double amount) {
    return std::format("${:.4f}", amount);
}

std::vector<std::string> token_usage_summary_lines(TokenUsage usage, std::string_view label, std::optional<std::string_view> model) {
    std::optional<ModelPricing> pricing;
    if (model.has_value()) {
        pricing = pricing_for_model(*model);
    }

    ModelPricing effective_pricing = pricing.value_or(ModelPricing::default_sonnet_tier());
    auto cost = estimate_cost_usd(usage, effective_pricing);

    std::string model_suffix = model.has_value() ? std::format(" model={}", *model) : "";
    std::string pricing_suffix;
    if (!pricing.has_value() && model.has_value()) {
        pricing_suffix = " pricing=estimated-default";
    }

    return {
        std::format("{}: total_tokens={} input={} output={} cache_write={} cache_read={} estimated_cost={}{}{}",
                    label,
                    usage.total_tokens(),
                    usage.input_tokens,
                    usage.output_tokens,
                    usage.cache_creation_input_tokens,
                    usage.cache_read_input_tokens,
                    format_usd(cost.total_cost_usd()),
                    model_suffix,
                    pricing_suffix),
        std::format("  cost breakdown: input={} output={} cache_write={} cache_read={}",
                    format_usd(cost.input_cost_usd),
                    format_usd(cost.output_cost_usd),
                    format_usd(cost.cache_creation_cost_usd),
                    format_usd(cost.cache_read_cost_usd)),
    };
}

void UsageTracker::record(TokenUsage usage) {
    latest_turn_ = usage;
    cumulative_.input_tokens += usage.input_tokens;
    cumulative_.output_tokens += usage.output_tokens;
    cumulative_.cache_creation_input_tokens += usage.cache_creation_input_tokens;
    cumulative_.cache_read_input_tokens += usage.cache_read_input_tokens;
    ++turns_;
}

} // namespace claw::runtime
