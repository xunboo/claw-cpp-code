#include "summary_compression.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <format>

namespace claw::runtime {

namespace {

// Priority 0 = highest importance
int line_priority(std::string_view line) {
    // Core detail prefixes / section header "Conversation summary:"
    static constexpr std::string_view CORE_PREFIXES[] = {
        "conversation summary:",
        "error:", "warning:", "note:",
        "failed", "success",
    };
    std::string lower(line);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (auto pfx : CORE_PREFIXES) {
        if (lower.starts_with(pfx)) return 0;
    }
    // Section headers ending in ':'
    if (!line.empty() && line.back() == ':') return 1;
    // Bullet points
    if (line.starts_with("- ") || line.starts_with("  - ")) return 2;
    return 3;
}

std::string collapse_inline_whitespace(std::string_view line) {
    std::string result;
    result.reserve(line.size());
    bool in_ws = false;
    for (char c : line) {
        if (c == '\t' || c == ' ') {
            if (!in_ws) { result += ' '; in_ws = true; }
        } else {
            result += c;
            in_ws = false;
        }
    }
    // Trim trailing
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

std::string truncate_line(std::string_view line, std::size_t max_chars) {
    if (line.size() <= max_chars) return std::string(line);
    // Find last space within limit to avoid mid-word cut
    auto cut = max_chars - 1; // room for '…'
    // Just cut at byte boundary for simplicity (UTF-8 awareness omitted for brevity)
    return std::string(line.substr(0, cut)) + "\xe2\x80\xa6"; // UTF-8 ellipsis
}

} // anonymous namespace

SummaryCompressionResult compress_summary(std::string_view text, SummaryCompressionBudget budget) {
    SummaryCompressionResult result;
    result.original_chars = text.size();

    // Split into lines
    std::vector<std::string> raw_lines;
    {
        std::istringstream ss{std::string(text)};
        std::string ln;
        while (std::getline(ss, ln)) {
            raw_lines.push_back(std::move(ln));
        }
    }
    result.original_lines = raw_lines.size();

    // Normalize + deduplicate (case-insensitive key via BTreeSet equivalent)
    std::vector<std::string> normalized;
    std::set<std::string> seen_lower;
    for (auto& raw : raw_lines) {
        std::string norm = collapse_inline_whitespace(raw);
        if (norm.empty()) { normalized.push_back(norm); continue; }
        std::string key(norm);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (!seen_lower.insert(key).second) {
            ++result.removed_duplicate_lines;
            continue;
        }
        normalized.push_back(std::move(norm));
    }

    // Sort by priority, preserving original order within same priority
    // Build index list sorted by priority (stable)
    std::vector<std::size_t> indices(normalized.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
        return line_priority(normalized[a]) < line_priority(normalized[b]);
    });

    // Select lines within budget
    std::vector<std::size_t> selected;
    std::size_t char_count = 0;
    for (auto idx : indices) {
        const auto& ln = normalized[idx];
        if (selected.size() >= budget.max_lines) break;
        std::size_t line_chars = std::min(ln.size(), budget.max_line_chars);
        if (char_count + line_chars > budget.max_chars && !selected.empty()) break;
        char_count += line_chars;
        selected.push_back(idx);
    }
    result.omitted_lines = normalized.size() - selected.size();

    // Restore original order
    std::sort(selected.begin(), selected.end());

    // Build output
    std::string output;
    bool first = true;
    for (auto idx : selected) {
        auto ln = truncate_line(normalized[idx], budget.max_line_chars);
        if (!first) output += '\n';
        output += ln;
        first = false;
        if (ln.size() > budget.max_line_chars) result.truncated = true;
    }

    if (result.omitted_lines > 0) {
        output += '\n';
        output += std::format("- \xe2\x80\xa6 {} additional line(s) omitted.", result.omitted_lines);
    }

    result.summary = std::move(output);
    result.compressed_chars = result.summary.size();
    result.compressed_lines = selected.size();
    return result;
}

std::string compress_summary_text(std::string_view text) {
    return compress_summary(text).summary;
}

} // namespace claw::runtime
