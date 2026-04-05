#pragma once
#include <string>
#include <optional>
#include <cstdint>
#include <vector>
#include <format>

namespace claw::runtime {

/// Per-million-token pricing used for cost estimation.
struct ModelPricing {
    double input_cost_per_million;
    double output_cost_per_million;
    double cache_creation_cost_per_million;
    double cache_read_cost_per_million;

    [[nodiscard]] static constexpr ModelPricing default_sonnet_tier() noexcept {
        return {15.0, 75.0, 18.75, 1.5};
    }
};

/// Token counters accumulated for a conversation turn or session.
struct TokenUsage {
    uint32_t input_tokens{0};
    uint32_t output_tokens{0};
    uint32_t cache_creation_input_tokens{0};
    uint32_t cache_read_input_tokens{0};

    [[nodiscard]] constexpr uint32_t total_tokens() const noexcept {
        return input_tokens + output_tokens + cache_creation_input_tokens + cache_read_input_tokens;
    }

    [[nodiscard]] bool operator==(const TokenUsage&) const noexcept = default;
};

/// Estimated dollar cost derived from a TokenUsage sample.
struct UsageCostEstimate {
    double input_cost_usd{0.0};
    double output_cost_usd{0.0};
    double cache_creation_cost_usd{0.0};
    double cache_read_cost_usd{0.0};

    [[nodiscard]] double total_cost_usd() const noexcept {
        return input_cost_usd + output_cost_usd + cache_creation_cost_usd + cache_read_cost_usd;
    }
};

/// Returns pricing metadata for a known model alias or family.
[[nodiscard]] std::optional<ModelPricing> pricing_for_model(std::string_view model);
[[nodiscard]] UsageCostEstimate estimate_cost_usd(TokenUsage usage, ModelPricing pricing = ModelPricing::default_sonnet_tier());
/// Formats a dollar-denominated value for CLI display.
[[nodiscard]] std::string format_usd(double amount);
[[nodiscard]] std::vector<std::string> token_usage_summary_lines(TokenUsage usage, std::string_view label, std::optional<std::string_view> model = std::nullopt);

/// Aggregates token usage across a running session.
class UsageTracker {
public:
    UsageTracker() = default;

    void record(TokenUsage usage);
    [[nodiscard]] TokenUsage current_turn_usage() const noexcept { return latest_turn_; }
    [[nodiscard]] TokenUsage cumulative_usage() const noexcept { return cumulative_; }
    [[nodiscard]] uint32_t turns() const noexcept { return turns_; }

private:
    TokenUsage latest_turn_{};
    TokenUsage cumulative_{};
    uint32_t turns_{0};
};

} // namespace claw::runtime
