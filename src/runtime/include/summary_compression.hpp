#pragma once
#include <string>
#include <vector>
#include <cstddef>

namespace claw::runtime {

inline constexpr std::size_t DEFAULT_MAX_CHARS      = 1200;
inline constexpr std::size_t DEFAULT_MAX_LINES      = 24;
inline constexpr std::size_t DEFAULT_MAX_LINE_CHARS = 160;

struct SummaryCompressionBudget {
    std::size_t max_chars{DEFAULT_MAX_CHARS};
    std::size_t max_lines{DEFAULT_MAX_LINES};
    std::size_t max_line_chars{DEFAULT_MAX_LINE_CHARS};
};

struct SummaryCompressionResult {
    std::string summary;
    std::size_t original_chars{0};
    std::size_t compressed_chars{0};
    std::size_t original_lines{0};
    std::size_t compressed_lines{0};
    std::size_t removed_duplicate_lines{0};
    std::size_t omitted_lines{0};
    bool truncated{false};
};

[[nodiscard]] SummaryCompressionResult compress_summary(std::string_view text, SummaryCompressionBudget budget = {});

// Convenience: compress with default budget, return just the compressed string
[[nodiscard]] std::string compress_summary_text(std::string_view text);

} // namespace claw::runtime
