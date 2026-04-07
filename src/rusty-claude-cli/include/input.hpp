#pragma once
// input.hpp -- C++20 port of input.rs
// Line-editor with slash-command completion: LineEditor, ReadOutcome

#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace claw {

// ---- ReadOutcome ----

/// Mirrors Rust's ReadOutcome enum.
/// Submit  -- the user typed a line and pressed Enter.
/// Cancel  -- the user pressed Ctrl-C with content in the buffer.
/// Exit    -- the user pressed Ctrl-C on an empty line, or Ctrl-D (EOF).
struct ReadSubmit { std::string text; };
struct ReadCancel {};
struct ReadExit   {};
using ReadOutcome = std::variant<ReadSubmit, ReadCancel, ReadExit>;

// ---- LineEditor ----

/// Mirrors Rust's LineEditor struct.
/// Wraps stdin/stdout into a readline-style line editor with:
///   - history
///   - slash-command tab completion
///   - Ctrl-J / Shift-Enter multi-line entry (best-effort on terminals that support it)
class LineEditor {
public:
    explicit LineEditor(std::string prompt, std::vector<std::string> completions = {});

    /// Add an entry to history (blank entries are ignored, matching Rust behaviour).
    void push_history(std::string entry);

    /// Replace the current completion candidates (normalised: only '/' prefixed, deduplicated).
    void set_completions(std::vector<std::string> completions);

    /// Read one logical line from the terminal.
    /// Falls back to a simple stdin read when not connected to a TTY.
    [[nodiscard]] ReadOutcome read_line();

    /// Load input history from a file (one entry per line). Silently ignored on error.
    void load_history(const std::filesystem::path& path);

    /// Save input history to a file (one entry per line). Silently ignored on error.
    void save_history(const std::filesystem::path& path, std::size_t max_entries = 500) const;

private:
    std::string prompt_;
    std::vector<std::string> completions_; // normalised
    std::vector<std::string> history_;

    /// Non-interactive (pipe) fallback.
    [[nodiscard]] ReadOutcome read_line_fallback() const;

    /// Compute tab-completion matches for the current prefix.
    [[nodiscard]] std::vector<std::string> complete(std::string_view prefix) const;

    static std::vector<std::string> normalize_completions(std::vector<std::string> candidates);
};

// ---- free helpers ----

/// Return the slash-command prefix of [line, pos) if it qualifies for completion,
/// otherwise return an empty optional.  Mirrors the Rust free function.
[[nodiscard]] std::optional<std::string_view>
slash_command_prefix(std::string_view line, std::size_t pos) noexcept;

} // namespace claw