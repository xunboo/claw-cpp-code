#pragma once

#include "tool_types.hpp"

#include <set>
#include <string>
#include <vector>

namespace claw::tools {

/// All built-in tool specs (both "mvp" base tools and deferred/searchable ones).
[[nodiscard]] std::vector<ToolSpec> mvp_tool_specs();

/// Subset of mvp_tool_specs that are not always presented to the model directly
/// (used for ToolSearch).
[[nodiscard]] std::vector<ToolSpec> deferred_tool_specs();

/// Return only the specs in the allowed set (nullptr = all).
[[nodiscard]] std::vector<ToolSpec>
    tool_specs_for_allowed_tools(const std::set<std::string>* allowed_tools);

/// Normalise a tool name: trim, lowercase, replace '-' with '_'.
[[nodiscard]] std::string normalize_tool_name(std::string_view value);

/// Score-based search across searchable specs.
[[nodiscard]] std::vector<std::string>
    search_tool_specs(const std::string& query,
                      std::size_t max_results,
                      const std::vector<struct SearchableToolSpec>& specs);

[[nodiscard]] std::string normalize_tool_search_query(const std::string& query);
[[nodiscard]] std::string canonical_tool_token(std::string_view value);

struct SearchableToolSpec {
    std::string name;
    std::string description;
};

}  // namespace claw::tools
