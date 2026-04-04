#pragma once
#include <string>
#include <string_view>
#include <cstdint>

namespace claw::runtime {

// Normalize a tool name to be valid for MCP (snake_case, no special chars)
[[nodiscard]] std::string normalize_name_for_mcp(std::string_view name);

// Prefix a tool name with the MCP server prefix
[[nodiscard]] std::string mcp_tool_prefix(std::string_view server_name);
[[nodiscard]] std::string mcp_tool_name(std::string_view server_name, std::string_view tool_name);

// FNV-1a 32-bit hash → hex string (for stable tool IDs)
[[nodiscard]] std::string stable_hex_hash(std::string_view input);

// URL percent-encode / decode
[[nodiscard]] std::string percent_encode(std::string_view input);
[[nodiscard]] std::string percent_decode(std::string_view input);

} // namespace claw::runtime
