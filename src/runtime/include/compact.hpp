#pragma once
#include "session.hpp"
#include <string>
#include <optional>
#include <cstddef>
#include <tl/expected.hpp>

namespace claw::runtime {

/// Thresholds controlling when and how a session is compacted.
struct CompactionConfig {
    std::size_t target_token_budget{8192};
    std::size_t max_summary_tokens{2048};
    bool preserve_tool_results{true};
    std::optional<std::size_t> keep_last_n_messages;
};

/// Result of compacting a session into a summary plus preserved tail messages.
struct CompactionResult {
    std::size_t original_message_count{0};
    std::size_t compacted_message_count{0};
    std::size_t estimated_tokens_before{0};
    std::size_t estimated_tokens_after{0};
    std::string summary; // The generated summary text
};

/// Roughly estimates the token footprint of the current session transcript.
[[nodiscard]] std::size_t estimate_session_tokens(const Session& session);

/// Returns `true` when the session exceeds the configured compaction budget.
[[nodiscard]] bool should_compact(const Session& session, std::size_t token_budget);

/// Normalizes a compaction summary into user-facing continuation text.
[[nodiscard]] std::string format_compact_summary(const CompactionResult& result);

// Read auto-compaction threshold from environment: CLAW_AUTO_COMPACT_THRESHOLD
[[nodiscard]] std::optional<std::size_t> auto_compaction_threshold_from_env();

/// Compacts a session by summarizing older messages and preserving the recent tail.
[[nodiscard]] tl::expected<std::pair<Session, CompactionResult>, std::string>
    compact_session(const Session& session, const CompactionConfig& config);

} // namespace claw::runtime
