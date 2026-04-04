#include "scenario.hpp"

namespace claw::mock {

std::optional<Scenario> parse_scenario(std::string_view value) noexcept {
    // trim whitespace
    auto s = value.find_first_not_of(" \t\r\n");
    if (s == std::string_view::npos) return std::nullopt;
    auto e = value.find_last_not_of(" \t\r\n");
    value = value.substr(s, e - s + 1);

    if (value == "streaming_text")                  return Scenario::StreamingText;
    if (value == "read_file_roundtrip")             return Scenario::ReadFileRoundtrip;
    if (value == "grep_chunk_assembly")             return Scenario::GrepChunkAssembly;
    if (value == "write_file_allowed")              return Scenario::WriteFileAllowed;
    if (value == "write_file_denied")               return Scenario::WriteFileDenied;
    if (value == "multi_tool_turn_roundtrip")       return Scenario::MultiToolTurnRoundtrip;
    if (value == "bash_stdout_roundtrip")           return Scenario::BashStdoutRoundtrip;
    if (value == "bash_permission_prompt_approved") return Scenario::BashPermissionPromptApproved;
    if (value == "bash_permission_prompt_denied")   return Scenario::BashPermissionPromptDenied;
    if (value == "plugin_tool_roundtrip")           return Scenario::PluginToolRoundtrip;
    if (value == "auto_compact_triggered")          return Scenario::AutoCompactTriggered;
    if (value == "token_cost_reporting")            return Scenario::TokenCostReporting;
    return std::nullopt;
}

std::string_view scenario_name(Scenario s) noexcept {
    switch (s) {
        case Scenario::StreamingText:                return "streaming_text";
        case Scenario::ReadFileRoundtrip:            return "read_file_roundtrip";
        case Scenario::GrepChunkAssembly:            return "grep_chunk_assembly";
        case Scenario::WriteFileAllowed:             return "write_file_allowed";
        case Scenario::WriteFileDenied:              return "write_file_denied";
        case Scenario::MultiToolTurnRoundtrip:       return "multi_tool_turn_roundtrip";
        case Scenario::BashStdoutRoundtrip:          return "bash_stdout_roundtrip";
        case Scenario::BashPermissionPromptApproved: return "bash_permission_prompt_approved";
        case Scenario::BashPermissionPromptDenied:   return "bash_permission_prompt_denied";
        case Scenario::PluginToolRoundtrip:          return "plugin_tool_roundtrip";
        case Scenario::AutoCompactTriggered:         return "auto_compact_triggered";
        case Scenario::TokenCostReporting:           return "token_cost_reporting";
    }
    return "unknown";
}

std::string_view request_id_for(Scenario s) noexcept {
    switch (s) {
        case Scenario::StreamingText:                return "req_streaming_text";
        case Scenario::ReadFileRoundtrip:            return "req_read_file_roundtrip";
        case Scenario::GrepChunkAssembly:            return "req_grep_chunk_assembly";
        case Scenario::WriteFileAllowed:             return "req_write_file_allowed";
        case Scenario::WriteFileDenied:              return "req_write_file_denied";
        case Scenario::MultiToolTurnRoundtrip:       return "req_multi_tool_turn_roundtrip";
        case Scenario::BashStdoutRoundtrip:          return "req_bash_stdout_roundtrip";
        case Scenario::BashPermissionPromptApproved: return "req_bash_permission_prompt_approved";
        case Scenario::BashPermissionPromptDenied:   return "req_bash_permission_prompt_denied";
        case Scenario::PluginToolRoundtrip:          return "req_plugin_tool_roundtrip";
        case Scenario::AutoCompactTriggered:         return "req_auto_compact_triggered";
        case Scenario::TokenCostReporting:           return "req_token_cost_reporting";
    }
    return "req_unknown";
}

}  // namespace claw::mock
