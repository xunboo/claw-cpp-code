#pragma once
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <compare>

namespace claw::runtime {

// Ordered green levels: Package < Workspace < MergeReady
enum class GreenLevel : uint8_t {
    Package    = 0,
    Workspace  = 1,
    MergeReady = 2,
};

[[nodiscard]] constexpr bool operator<(GreenLevel a, GreenLevel b) noexcept {
    return static_cast<uint8_t>(a) < static_cast<uint8_t>(b);
}
[[nodiscard]] constexpr bool operator<=(GreenLevel a, GreenLevel b) noexcept {
    return !(b < a);
}
[[nodiscard]] constexpr bool operator>(GreenLevel a, GreenLevel b) noexcept {
    return b < a;
}
[[nodiscard]] constexpr bool operator>=(GreenLevel a, GreenLevel b) noexcept {
    return !(a < b);
}
[[nodiscard]] std::string_view green_level_name(GreenLevel level) noexcept;

struct GreenContractPassed {
    GreenLevel achieved;
};
struct GreenContractFailed {
    GreenLevel required;
    GreenLevel achieved;
    std::string reason;
};
struct GreenContractSkipped {
    std::string reason;
};

using GreenContractOutcome = std::variant<GreenContractPassed, GreenContractFailed, GreenContractSkipped>;

struct GreenContract {
    GreenLevel required_level;
    std::optional<std::string> scope;        // e.g. specific crate or package name
    std::vector<std::string> skip_patterns;  // glob patterns for files to skip

    [[nodiscard]] GreenContractOutcome evaluate(GreenLevel achieved, std::optional<std::string_view> skip_reason = std::nullopt) const;
};

} // namespace claw::runtime
