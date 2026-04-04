#pragma once

#include <optional>
#include <string_view>

namespace claw::mock {

enum class Scenario {
    StreamingText,
    ReadFileRoundtrip,
    GrepChunkAssembly,
    WriteFileAllowed,
    WriteFileDenied,
    MultiToolTurnRoundtrip,
    BashStdoutRoundtrip,
    BashPermissionPromptApproved,
    BashPermissionPromptDenied,
    PluginToolRoundtrip,
    AutoCompactTriggered,
    TokenCostReporting,
};

inline constexpr std::string_view SCENARIO_PREFIX = "PARITY_SCENARIO:";
inline constexpr std::string_view DEFAULT_MODEL    = "claude-sonnet-4-6";

[[nodiscard]] std::optional<Scenario> parse_scenario(std::string_view value) noexcept;
[[nodiscard]] std::string_view        scenario_name(Scenario s) noexcept;
[[nodiscard]] std::string_view        request_id_for(Scenario s) noexcept;

}  // namespace claw::mock
