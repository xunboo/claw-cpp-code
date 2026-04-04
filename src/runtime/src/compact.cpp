// compact.cpp — C++20 translation of compact.rs
//
// Implements session compaction: estimating token counts, deciding when to
// compact, summarising older messages, merging previous summaries, and
// producing a compacted Session.
//
// The Rust source has a richer Session type (System/Tool roles, string-based
// ToolUse.input, compaction/fork metadata).  The C++ Session (session.hpp)
// exposes only User/Assistant roles plus TextBlock/ToolUseBlock/ToolResultBlock/
// ThinkingBlock.  We translate faithfully while mapping System→User (stored as
// a TextBlock summary preamble) and Tool→Assistant where needed.

#include "compact.hpp"
#include "session.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <format>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace claw::runtime {

// ─── Constants (mirrors compact.rs) ──────────────────────────────────────────

static constexpr std::string_view COMPACT_CONTINUATION_PREAMBLE =
    "This session is being continued from a previous conversation that ran out of context. "
    "The summary below covers the earlier portion of the conversation.\n\n";

static constexpr std::string_view COMPACT_RECENT_MESSAGES_NOTE =
    "Recent messages are preserved verbatim.";

static constexpr std::string_view COMPACT_DIRECT_RESUME_INSTRUCTION =
    "Continue the conversation from where it left off without asking the user any further "
    "questions. Resume directly \xe2\x80\x94 do not acknowledge the summary, do not recap "
    "what was happening, and do not preface with continuation text.";

// ─── Internal helpers — string utilities ─────────────────────────────────────

// Returns the Unicode scalar count of a UTF-8 string (approximate: counts
// leading bytes only, matching Rust .chars().count()).
static std::size_t utf8_char_count(std::string_view s) {
    std::size_t count = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0u) != 0x80u) ++count; // not a continuation byte
    }
    return count;
}

// Truncate to at most max_chars Unicode scalar values, appending U+2026 (…)
// if truncated.  Mirrors Rust truncate_summary.
static std::string truncate_summary(std::string_view content, std::size_t max_chars) {
    if (utf8_char_count(content) <= max_chars) return std::string(content);
    // Walk bytes until we have collected max_chars scalars.
    std::size_t byte_pos = 0;
    std::size_t count    = 0;
    while (byte_pos < content.size() && count < max_chars) {
        unsigned char c = static_cast<unsigned char>(content[byte_pos]);
        // Determine byte width of this scalar.
        std::size_t width = 1;
        if      ((c & 0x80u) == 0)    width = 1;
        else if ((c & 0xE0u) == 0xC0u) width = 2;
        else if ((c & 0xF0u) == 0xE0u) width = 3;
        else if ((c & 0xF8u) == 0xF0u) width = 4;
        byte_pos += width;
        ++count;
    }
    // byte_pos may overshoot at end; cap to content size.
    if (byte_pos > content.size()) byte_pos = content.size();
    std::string result(content.substr(0, byte_pos));
    result += "\xe2\x80\xa6"; // U+2026 HORIZONTAL ELLIPSIS in UTF-8
    return result;
}

// Collapse multiple consecutive blank lines into one.  Mirrors Rust collapse_blank_lines.
static std::string collapse_blank_lines(std::string_view content) {
    std::string result;
    bool last_blank = false;
    std::size_t pos  = 0;
    while (pos <= content.size()) {
        std::size_t nl = content.find('\n', pos);
        std::string_view line;
        if (nl == std::string_view::npos) {
            line = content.substr(pos);
            pos  = content.size() + 1;
        } else {
            line = content.substr(pos, nl - pos);
            pos  = nl + 1;
        }
        // Check whether the line is blank (only whitespace).
        bool is_blank = true;
        for (char c : line) {
            if (c != ' ' && c != '\t' && c != '\r') { is_blank = false; break; }
        }
        if (is_blank && last_blank) continue;
        result += line;
        result += '\n';
        last_blank = is_blank;
    }
    return result;
}

// Extract the content between <tag> and </tag>.  Returns std::nullopt if not found.
// Mirrors Rust extract_tag_block.
static std::optional<std::string> extract_tag_block(std::string_view content, std::string_view tag) {
    std::string start_tag = std::format("<{}>", tag);
    std::string end_tag   = std::format("</{}>", tag);
    auto start_pos = content.find(start_tag);
    if (start_pos == std::string_view::npos) return std::nullopt;
    auto inner_start = start_pos + start_tag.size();
    auto end_pos = content.find(end_tag, inner_start);
    if (end_pos == std::string_view::npos) return std::nullopt;
    return std::string(content.substr(inner_start, end_pos - inner_start));
}

// Strip the first <tag>...</tag> block from content.  Mirrors Rust strip_tag_block.
static std::string strip_tag_block(std::string_view content, std::string_view tag) {
    std::string start_tag = std::format("<{}>", tag);
    std::string end_tag   = std::format("</{}>", tag);
    auto start_pos = content.find(start_tag);
    auto end_pos   = content.find(end_tag);
    if (start_pos == std::string_view::npos || end_pos == std::string_view::npos) {
        return std::string(content);
    }
    auto end_full = end_pos + end_tag.size();
    std::string result(content.substr(0, start_pos));
    result += content.substr(end_full);
    return result;
}

// ─── Internal helpers — ContentBlock text extraction ─────────────────────────

// Returns the text content of a block for token-estimation purposes.
static std::size_t estimate_block_tokens(const ContentBlock& block) {
    return std::visit([](const auto& b) -> std::size_t {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, TextBlock>) {
            return b.text.size() / 4 + 1;
        } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
            return (b.name.size() + b.input.dump().size()) / 4 + 1;
        } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
            return b.content.size() / 4 + 1;
        } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
            return b.thinking.size() / 4 + 1;
        } else {
            return 1;
        }
    }, block);
}

// Returns the raw text of a block (for summary / key-file extraction).
static std::string block_text(const ContentBlock& block) {
    return std::visit([](const auto& b) -> std::string {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, TextBlock>)      return b.text;
        else if constexpr (std::is_same_v<T, ToolUseBlock>)   return b.input.dump();
        else if constexpr (std::is_same_v<T, ToolResultBlock>) return b.content;
        else if constexpr (std::is_same_v<T, ThinkingBlock>)  return b.thinking;
        else return {};
    }, block);
}

// Returns a one-line summary of a block (≤160 chars).  Mirrors Rust summarize_block.
static std::string summarize_block(const ContentBlock& block) {
    std::string raw = std::visit([](const auto& b) -> std::string {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, TextBlock>) {
            return b.text;
        } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
            return std::format("tool_use {}({})", b.name, b.input.dump());
        } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
            return std::format("tool_result {}: {}{}", b.tool_use_id,
                               b.is_error ? "error " : "", b.content);
        } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
            return std::format("thinking: {}", b.thinking);
        } else {
            return {};
        }
    }, block);
    return truncate_summary(raw, 160);
}

// Find first non-empty text block in a message.
static std::optional<std::string_view> first_text_block(const ConversationMessage& msg) {
    for (const auto& block : msg.blocks) {
        if (const auto* tb = std::get_if<TextBlock>(&block)) {
            // Trim check: is the text non-blank?
            bool blank = true;
            for (char c : tb->text) {
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { blank = false; break; }
            }
            if (!blank) return std::string_view(tb->text);
        }
    }
    return std::nullopt;
}

// ─── Internal helpers — file candidate extraction ─────────────────────────────

static bool has_interesting_extension(std::string_view candidate) {
    // Find last '.'
    auto dot = candidate.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string_view ext = candidate.substr(dot + 1);
    // Case-insensitive compare
    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    };
    static constexpr std::string_view EXTS[] = {"rs", "ts", "tsx", "js", "json", "md"};
    for (auto& e : EXTS) { if (ieq(ext, e)) return true; }
    return false;
}

static std::vector<std::string> extract_file_candidates(std::string_view content) {
    std::vector<std::string> result;
    // Split on whitespace.
    std::size_t pos = 0;
    while (pos < content.size()) {
        // Skip whitespace
        while (pos < content.size() &&
               (content[pos] == ' ' || content[pos] == '\t' ||
                content[pos] == '\n' || content[pos] == '\r')) ++pos;
        if (pos >= content.size()) break;
        // Find end of token
        std::size_t end = pos;
        while (end < content.size() &&
               content[end] != ' ' && content[end] != '\t' &&
               content[end] != '\n' && content[end] != '\r') ++end;
        std::string_view token = content.substr(pos, end - pos);
        pos = end;

        // Trim punctuation chars from both ends.
        static constexpr std::string_view PUNCT = ",.;:)(\"'`";
        std::size_t tstart = 0, tend = token.size();
        while (tstart < tend && PUNCT.find(token[tstart]) != std::string_view::npos) ++tstart;
        while (tend > tstart && PUNCT.find(token[tend - 1]) != std::string_view::npos) --tend;
        token = token.substr(tstart, tend - tstart);

        if (token.find('/') != std::string_view::npos && has_interesting_extension(token)) {
            result.emplace_back(token);
        }
    }
    return result;
}

// ─── Internal: collect_key_files ─────────────────────────────────────────────

static std::vector<std::string> collect_key_files(const std::vector<ConversationMessage>& messages) {
    std::vector<std::string> files;
    for (const auto& msg : messages) {
        for (const auto& block : msg.blocks) {
            auto text = block_text(block);
            auto candidates = extract_file_candidates(text);
            for (auto& c : candidates) files.push_back(std::move(c));
        }
    }
    std::ranges::sort(files);
    auto [first_dup, last] = std::ranges::unique(files);
    files.erase(first_dup, files.end());
    if (files.size() > 8) files.resize(8);
    return files;
}

// ─── Internal: collect_recent_role_summaries ─────────────────────────────────

// Mirror Rust: take last `limit` messages matching `role`, reverse-iterate,
// collect text truncated to 160 chars, then reverse back.
static std::vector<std::string> collect_recent_role_summaries(
    const std::vector<ConversationMessage>& messages,
    MessageRole role,
    std::size_t limit)
{
    std::vector<std::string> result;
    for (auto it = messages.rbegin(); it != messages.rend() && result.size() < limit; ++it) {
        if (it->role != role) continue;
        auto tv = first_text_block(*it);
        if (!tv) continue;
        result.push_back(truncate_summary(*tv, 160));
    }
    std::ranges::reverse(result);
    return result;
}

// ─── Internal: infer_pending_work ────────────────────────────────────────────

static std::vector<std::string> infer_pending_work(const std::vector<ConversationMessage>& messages) {
    std::vector<std::string> result;
    for (auto it = messages.rbegin(); it != messages.rend() && result.size() < 3; ++it) {
        auto tv = first_text_block(*it);
        if (!tv) continue;
        std::string lower(*tv);
        std::ranges::transform(lower, lower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (lower.find("todo")      != std::string::npos ||
            lower.find("next")      != std::string::npos ||
            lower.find("pending")   != std::string::npos ||
            lower.find("follow up") != std::string::npos ||
            lower.find("remaining") != std::string::npos) {
            result.push_back(truncate_summary(*tv, 160));
        }
    }
    std::ranges::reverse(result);
    return result;
}

// ─── Internal: infer_current_work ────────────────────────────────────────────

static std::optional<std::string> infer_current_work(const std::vector<ConversationMessage>& messages) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        auto tv = first_text_block(*it);
        if (!tv) continue;
        // Check non-empty after trimming whitespace
        bool blank = true;
        for (char c : *tv) {
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { blank = false; break; }
        }
        if (!blank) return truncate_summary(*tv, 200);
    }
    return std::nullopt;
}

// ─── Internal: summarize_messages ────────────────────────────────────────────
// Produces a <summary>...</summary> block from a slice of messages.
// Mirrors Rust summarize_messages.

static std::string summarize_messages(const std::vector<ConversationMessage>& messages) {
    std::size_t user_count      = 0;
    std::size_t assistant_count = 0;
    std::size_t tool_count      = 0;

    // In C++ we only have User/Assistant; "tool" role doesn't exist in the
    // C++ header.  Count ToolUseBlock/ToolResultBlock blocks as tool activity.
    for (const auto& msg : messages) {
        if (msg.role == MessageRole::User)      ++user_count;
        else if (msg.role == MessageRole::Assistant) {
            bool has_tool = false;
            for (const auto& b : msg.blocks) {
                if (std::holds_alternative<ToolUseBlock>(b) ||
                    std::holds_alternative<ToolResultBlock>(b)) { has_tool = true; break; }
            }
            if (has_tool) ++tool_count;
            else          ++assistant_count;
        }
    }

    // Collect unique tool names from ToolUseBlock / ToolResultBlock.
    std::vector<std::string> tool_names;
    for (const auto& msg : messages) {
        for (const auto& block : msg.blocks) {
            std::visit([&](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, ToolUseBlock>)    tool_names.push_back(b.name);
                else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                    // ToolResultBlock has tool_use_id, not name; skip for now (no name available)
                }
            }, block);
        }
    }
    std::ranges::sort(tool_names);
    auto [dup_first, dup_last] = std::ranges::unique(tool_names);
    tool_names.erase(dup_first, tool_names.end());

    std::vector<std::string> lines;
    lines.emplace_back("<summary>");
    lines.emplace_back("Conversation summary:");
    lines.push_back(std::format(
        "- Scope: {} earlier messages compacted (user={}, assistant={}, tool={}).",
        messages.size(), user_count, assistant_count, tool_count));

    if (!tool_names.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < tool_names.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += tool_names[i];
        }
        lines.push_back(std::format("- Tools mentioned: {}.", joined));
    }

    auto recent_user = collect_recent_role_summaries(messages, MessageRole::User, 3);
    if (!recent_user.empty()) {
        lines.emplace_back("- Recent user requests:");
        for (const auto& req : recent_user)
            lines.push_back(std::format("  - {}", req));
    }

    auto pending = infer_pending_work(messages);
    if (!pending.empty()) {
        lines.emplace_back("- Pending work:");
        for (const auto& item : pending)
            lines.push_back(std::format("  - {}", item));
    }

    auto key_files = collect_key_files(messages);
    if (!key_files.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < key_files.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += key_files[i];
        }
        lines.push_back(std::format("- Key files referenced: {}.", joined));
    }

    if (auto cw = infer_current_work(messages)) {
        lines.push_back(std::format("- Current work: {}", *cw));
    }

    lines.emplace_back("- Key timeline:");
    for (const auto& msg : messages) {
        std::string role_str = (msg.role == MessageRole::User) ? "user" : "assistant";
        std::string content;
        for (std::size_t i = 0; i < msg.blocks.size(); ++i) {
            if (i > 0) content += " | ";
            content += summarize_block(msg.blocks[i]);
        }
        lines.push_back(std::format("  - {}: {}", role_str, content));
    }
    lines.emplace_back("</summary>");

    std::string result;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result += '\n';
        result += lines[i];
    }
    return result;
}

// ─── Internal: extract_summary_highlights ────────────────────────────────────
// Returns all non-blank, non-header lines that precede "- Key timeline:".
// Mirrors Rust extract_summary_highlights.

static std::vector<std::string> extract_summary_highlights(std::string_view summary) {
    // Run format_compact_summary first (declared publicly, so we call it).
    // But we need it here — forward-declare the free helper below.
    // We duplicate the formatting inline to avoid circular dependency.
    // (format_compact_summary is a free function in the same TU; use it.)
    // NOTE: format_compact_summary takes a CompactionResult in the C++ header,
    // but the Rust version takes a &str.  For internal summary processing we
    // work with the raw string directly.  We replicate the transformation here.

    // Strip <analysis>...</analysis>
    std::string without_analysis = strip_tag_block(summary, "analysis");
    // Replace <summary>content</summary> with "Summary:\ncontent"
    auto summary_content = extract_tag_block(without_analysis, "summary");
    std::string formatted;
    if (summary_content) {
        std::string old_block = std::format("<summary>{}</summary>", *summary_content);
        // Trim the content
        std::string_view trimmed = *summary_content;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t' ||
               trimmed.front() == '\n' || trimmed.front() == '\r')) trimmed.remove_prefix(1);
        while (!trimmed.empty() && (trimmed.back()  == ' ' || trimmed.back()  == '\t' ||
               trimmed.back()  == '\n' || trimmed.back()  == '\r')) trimmed.remove_suffix(1);
        std::string replacement = std::format("Summary:\n{}", trimmed);
        // Replace in without_analysis
        auto pos = without_analysis.find(old_block);
        if (pos != std::string::npos) {
            formatted = without_analysis.substr(0, pos) + replacement +
                        without_analysis.substr(pos + old_block.size());
        } else {
            formatted = without_analysis;
        }
    } else {
        formatted = without_analysis;
    }
    std::string collapsed = collapse_blank_lines(formatted);
    // Trim
    std::string_view sv = collapsed;
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
           sv.front() == '\n' || sv.front() == '\r')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back()  == ' ' || sv.back()  == '\t' ||
           sv.back()  == '\n' || sv.back()  == '\r')) sv.remove_suffix(1);

    std::vector<std::string> lines;
    bool in_timeline = false;

    std::size_t pos = 0;
    while (pos <= sv.size()) {
        std::size_t nl = sv.find('\n', pos);
        std::string_view line;
        if (nl == std::string_view::npos) {
            line = sv.substr(pos);
            pos  = sv.size() + 1;
        } else {
            line = sv.substr(pos, nl - pos);
            pos  = nl + 1;
        }
        // trim_end
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
               line.back() == '\r')) line.remove_suffix(1);

        if (line.empty() || line == "Summary:" || line == "Conversation summary:") continue;
        if (line == "- Key timeline:") { in_timeline = true; continue; }
        if (in_timeline) continue;
        lines.emplace_back(line);
    }
    return lines;
}

// Returns all lines under "- Key timeline:" until a blank line.
// Mirrors Rust extract_summary_timeline.
static std::vector<std::string> extract_summary_timeline(std::string_view summary) {
    // Same formatting as extract_summary_highlights (duplicate for simplicity).
    std::string without_analysis = strip_tag_block(summary, "analysis");
    auto summary_content = extract_tag_block(without_analysis, "summary");
    std::string formatted;
    if (summary_content) {
        std::string old_block = std::format("<summary>{}</summary>", *summary_content);
        std::string_view trimmed = *summary_content;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t' ||
               trimmed.front() == '\n' || trimmed.front() == '\r')) trimmed.remove_prefix(1);
        while (!trimmed.empty() && (trimmed.back()  == ' ' || trimmed.back()  == '\t' ||
               trimmed.back()  == '\n' || trimmed.back()  == '\r')) trimmed.remove_suffix(1);
        std::string replacement = std::format("Summary:\n{}", trimmed);
        auto pos = without_analysis.find(old_block);
        if (pos != std::string::npos) {
            formatted = without_analysis.substr(0, pos) + replacement +
                        without_analysis.substr(pos + old_block.size());
        } else {
            formatted = without_analysis;
        }
    } else {
        formatted = without_analysis;
    }
    std::string collapsed = collapse_blank_lines(formatted);
    std::string_view sv = collapsed;
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
           sv.front() == '\n' || sv.front() == '\r')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back()  == ' ' || sv.back()  == '\t' ||
           sv.back()  == '\n' || sv.back()  == '\r')) sv.remove_suffix(1);

    std::vector<std::string> lines;
    bool in_timeline = false;

    std::size_t pos = 0;
    while (pos <= sv.size()) {
        std::size_t nl = sv.find('\n', pos);
        std::string_view line;
        if (nl == std::string_view::npos) {
            line = sv.substr(pos);
            pos  = sv.size() + 1;
        } else {
            line = sv.substr(pos, nl - pos);
            pos  = nl + 1;
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
               line.back() == '\r')) line.remove_suffix(1);
        if (line == "- Key timeline:") { in_timeline = true; continue; }
        if (!in_timeline) continue;
        if (line.empty()) break;
        lines.emplace_back(line);
    }
    return lines;
}

// ─── Internal: format_compact_summary_str ────────────────────────────────────
// Takes a raw summary string (as produced by summarize_messages or
// merge_compact_summaries) and returns the human-readable form.
// Mirrors Rust format_compact_summary(&str).
// (The public C++ API format_compact_summary takes a CompactionResult; this
//  helper is used internally and by get_compact_continuation_message_str.)

static std::string format_compact_summary_str(std::string_view summary) {
    std::string without_analysis = strip_tag_block(summary, "analysis");
    auto summary_content = extract_tag_block(without_analysis, "summary");
    std::string formatted;
    if (summary_content) {
        std::string old_block = std::format("<summary>{}</summary>", *summary_content);
        std::string_view trimmed = *summary_content;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t' ||
               trimmed.front() == '\n' || trimmed.front() == '\r')) trimmed.remove_prefix(1);
        while (!trimmed.empty() && (trimmed.back()  == ' ' || trimmed.back()  == '\t' ||
               trimmed.back()  == '\n' || trimmed.back()  == '\r')) trimmed.remove_suffix(1);
        std::string replacement = std::format("Summary:\n{}", trimmed);
        auto pos = without_analysis.find(old_block);
        if (pos != std::string::npos) {
            formatted = without_analysis.substr(0, pos) + replacement +
                        without_analysis.substr(pos + old_block.size());
        } else {
            formatted = without_analysis;
        }
    } else {
        formatted = without_analysis;
    }
    std::string collapsed = collapse_blank_lines(formatted);
    // Trim
    std::string_view sv = collapsed;
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
           sv.front() == '\n' || sv.front() == '\r')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back()  == ' ' || sv.back()  == '\t' ||
           sv.back()  == '\n' || sv.back()  == '\r')) sv.remove_suffix(1);
    return std::string(sv);
}

// ─── Internal: get_compact_continuation_message_str ──────────────────────────
// Mirrors Rust get_compact_continuation_message.

static std::string get_compact_continuation_message_str(
    std::string_view summary,
    bool suppress_follow_up_questions,
    bool recent_messages_preserved)
{
    std::string base = std::string(COMPACT_CONTINUATION_PREAMBLE) + format_compact_summary_str(summary);

    if (recent_messages_preserved) {
        base += "\n\n";
        base += COMPACT_RECENT_MESSAGES_NOTE;
    }
    if (suppress_follow_up_questions) {
        base += '\n';
        base += COMPACT_DIRECT_RESUME_INSTRUCTION;
    }
    return base;
}

// ─── Internal: merge_compact_summaries ───────────────────────────────────────
// Mirrors Rust merge_compact_summaries.

static std::string merge_compact_summaries(
    std::optional<std::string_view> existing_summary,
    std::string_view new_summary)
{
    if (!existing_summary) return std::string(new_summary);

    auto previous_highlights = extract_summary_highlights(*existing_summary);
    std::string new_formatted  = format_compact_summary_str(new_summary);
    auto new_highlights  = extract_summary_highlights(new_formatted);
    auto new_timeline    = extract_summary_timeline(new_formatted);

    std::vector<std::string> lines;
    lines.emplace_back("<summary>");
    lines.emplace_back("Conversation summary:");

    if (!previous_highlights.empty()) {
        lines.emplace_back("- Previously compacted context:");
        for (const auto& l : previous_highlights)
            lines.push_back(std::format("  {}", l));
    }
    if (!new_highlights.empty()) {
        lines.emplace_back("- Newly compacted context:");
        for (const auto& l : new_highlights)
            lines.push_back(std::format("  {}", l));
    }
    if (!new_timeline.empty()) {
        lines.emplace_back("- Key timeline:");
        for (const auto& l : new_timeline)
            lines.push_back(std::format("  {}", l));
    }

    lines.emplace_back("</summary>");

    std::string result;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result += '\n';
        result += lines[i];
    }
    return result;
}

// ─── Internal: extract_existing_compacted_summary ─────────────────────────────
// Checks whether the first message of a session is a compacted-summary
// system message and, if so, extracts its summary text.
// In the C++ header there is no System role; we use a sentinel TextBlock prefix
// (COMPACT_CONTINUATION_PREAMBLE) as the marker, stored in User messages.

static std::optional<std::string> extract_existing_compacted_summary(
    const ConversationMessage& msg)
{
    // The continuation message is stored as a User message with a TextBlock
    // starting with COMPACT_CONTINUATION_PREAMBLE.
    auto tv = first_text_block(msg);
    if (!tv) return std::nullopt;

    if (!tv->starts_with(COMPACT_CONTINUATION_PREAMBLE)) return std::nullopt;

    std::string_view remainder = tv->substr(COMPACT_CONTINUATION_PREAMBLE.size());

    // Strip trailing recent-messages note (if present).
    std::string recent_note_str = std::format("\n\n{}", COMPACT_RECENT_MESSAGES_NOTE);
    auto note_pos = remainder.find(recent_note_str);
    if (note_pos != std::string_view::npos) {
        remainder = remainder.substr(0, note_pos);
    }

    // Strip trailing direct-resume instruction (if present).
    std::string resume_str = std::format("\n{}", COMPACT_DIRECT_RESUME_INSTRUCTION);
    auto resume_pos = remainder.find(resume_str);
    if (resume_pos != std::string_view::npos) {
        remainder = remainder.substr(0, resume_pos);
    }

    // Trim
    while (!remainder.empty() && (remainder.front() == ' '  || remainder.front() == '\t' ||
           remainder.front() == '\n' || remainder.front() == '\r')) remainder.remove_prefix(1);
    while (!remainder.empty() && (remainder.back()  == ' '  || remainder.back()  == '\t' ||
           remainder.back()  == '\n' || remainder.back()  == '\r')) remainder.remove_suffix(1);

    return std::string(remainder);
}

// ─── Internal: compacted_summary_prefix_len ──────────────────────────────────

static std::size_t compacted_summary_prefix_len(const Session& session) {
    if (session.messages.empty()) return 0;
    return extract_existing_compacted_summary(session.messages.front()).has_value() ? 1 : 0;
}

// ─── Public API ──────────────────────────────────────────────────────────────

std::size_t estimate_session_tokens(const Session& session) {
    std::size_t total = 0;
    // Include system_prompt in estimate.
    total += session.system_prompt.size() / 4 + (session.system_prompt.empty() ? 0 : 1);
    for (const auto& msg : session.messages) {
        for (const auto& block : msg.blocks) {
            total += estimate_block_tokens(block);
        }
    }
    return total;
}

bool should_compact(const Session& session, std::size_t token_budget) {
    // Mirrors Rust should_compact: skip the compacted summary prefix, then
    // check if the remaining compactable messages are > preserve_recent_messages
    // AND their token sum >= max_estimated_tokens.
    // The C++ header signature is simpler (just token_budget), so we use
    // a fixed preserve_recent_messages of 4 (matching CompactionConfig::default).
    static constexpr std::size_t DEFAULT_PRESERVE = 4;

    std::size_t start = compacted_summary_prefix_len(session);
    if (start >= session.messages.size()) return false;

    const auto& compactable = session.messages;
    std::size_t compactable_len = session.messages.size() - start;

    if (compactable_len <= DEFAULT_PRESERVE) return false;

    std::size_t tokens = 0;
    for (std::size_t i = start; i < session.messages.size(); ++i) {
        for (const auto& block : session.messages[i].blocks) {
            tokens += estimate_block_tokens(block);
        }
    }
    return tokens >= token_budget;
}

// The public C++ format_compact_summary takes a CompactionResult and formats
// the stats header plus the summary text it contains.
std::string format_compact_summary(const CompactionResult& result) {
    // Format the stats line, then append the raw summary string processed
    // through the Rust-equivalent pipeline.
    std::string header = std::format(
        "Compacted session: {} \xe2\x86\x92 {} messages, ~{} \xe2\x86\x92 ~{} tokens",
        result.original_message_count,
        result.compacted_message_count,
        result.estimated_tokens_before,
        result.estimated_tokens_after);
    if (!result.summary.empty()) {
        header += '\n';
        header += format_compact_summary_str(result.summary);
    }
    return header;
}

std::optional<std::size_t> auto_compaction_threshold_from_env() {
    const char* val = std::getenv("CLAW_AUTO_COMPACT_THRESHOLD");
    if (!val || !*val) return std::nullopt;
    std::size_t threshold{};
    auto [ptr, ec] = std::from_chars(val, val + std::strlen(val), threshold);
    if (ec != std::errc{}) return std::nullopt;
    return threshold;
}

// compact_session: the main compaction entry point.
// Mirrors Rust compact_session closely, adapting for the C++ Session type.
//
// What it does:
//   1. If token count is below the budget, return the session unchanged.
//   2. Detect whether the first message is an existing compacted summary.
//   3. Remove the prefix (existing summary, if any) and then everything from
//      [prefix_end .. len - preserve_recent_messages).
//   4. Summarise the removed messages; merge with the existing summary.
//   5. Build a new continuation message (User TextBlock) and prepend it.
//   6. Return the compacted session plus a CompactionResult.
tl::expected<std::pair<Session, CompactionResult>, std::string>
compact_session(const Session& session, const CompactionConfig& config) {
    CompactionResult cr;
    cr.original_message_count  = session.messages.size();
    cr.estimated_tokens_before = estimate_session_tokens(session);

    // Check if compaction is needed.
    if (!should_compact(session, config.target_token_budget)) {
        cr.compacted_message_count  = session.messages.size();
        cr.estimated_tokens_after   = cr.estimated_tokens_before;
        return std::make_pair(session, std::move(cr));
    }

    // Determine how many messages to preserve at the end.
    std::size_t preserve_n = config.keep_last_n_messages.value_or(4);

    // Find the existing-summary prefix length (0 or 1).
    std::size_t prefix_len = compacted_summary_prefix_len(session);

    // Optionally extract the existing summary text.
    std::optional<std::string> existing_summary_text;
    if (prefix_len > 0) {
        existing_summary_text = extract_existing_compacted_summary(session.messages.front());
    }

    // keep_from: index from which preserved messages start.
    std::size_t keep_from = session.messages.size() > preserve_n
        ? session.messages.size() - preserve_n
        : 0;

    // Messages to compact: [prefix_len .. keep_from)
    std::vector<ConversationMessage> to_remove;
    for (std::size_t i = prefix_len; i < keep_from; ++i) {
        to_remove.push_back(session.messages[i]);
    }

    // Messages to preserve: [keep_from .. end)
    std::vector<ConversationMessage> preserved;
    for (std::size_t i = keep_from; i < session.messages.size(); ++i) {
        preserved.push_back(session.messages[i]);
    }

    // Build summary.
    std::string new_raw_summary   = summarize_messages(to_remove);
    std::optional<std::string_view> existing_sv;
    if (existing_summary_text) existing_sv = *existing_summary_text;
    std::string merged_summary    = merge_compact_summaries(existing_sv, new_raw_summary);
    std::string formatted_summary = format_compact_summary_str(merged_summary);

    bool has_preserved = !preserved.empty();
    std::string continuation = get_compact_continuation_message_str(merged_summary, true, has_preserved);

    // Build compacted messages: continuation message + preserved.
    std::vector<ConversationMessage> compacted_messages;
    ConversationMessage summary_msg;
    summary_msg.role = MessageRole::User;
    summary_msg.blocks.push_back(TextBlock{continuation});
    compacted_messages.push_back(std::move(summary_msg));
    for (auto& m : preserved) compacted_messages.push_back(std::move(m));

    Session compacted_session      = session;
    compacted_session.messages     = std::move(compacted_messages);

    cr.compacted_message_count  = compacted_session.messages.size();
    cr.estimated_tokens_after   = estimate_session_tokens(compacted_session);
    cr.summary                  = std::move(merged_summary);

    return std::make_pair(std::move(compacted_session), std::move(cr));
}

} // namespace claw::runtime
