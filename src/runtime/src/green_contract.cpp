#include "green_contract.hpp"
#include <format>

namespace claw::runtime {

std::string_view green_level_name(GreenLevel level) noexcept {
    switch (level) {
        case GreenLevel::Package:    return "package";
        case GreenLevel::Workspace:  return "workspace";
        case GreenLevel::MergeReady: return "merge_ready";
    }
    return "unknown";
}

GreenContractOutcome GreenContract::evaluate(GreenLevel achieved, std::optional<std::string_view> skip_reason) const {
    if (skip_reason.has_value()) {
        return GreenContractSkipped{std::string(*skip_reason)};
    }
    if (achieved >= required_level) {
        return GreenContractPassed{achieved};
    }
    return GreenContractFailed{
        .required = required_level,
        .achieved = achieved,
        .reason = std::format("required {} but achieved {}", green_level_name(required_level), green_level_name(achieved)),
    };
}

} // namespace claw::runtime
