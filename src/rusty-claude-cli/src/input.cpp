// input.cpp -- C++20 port of input.rs
// LineEditor with slash-command tab completion, history, and TTY detection.
#include "input.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  include <io.h>
#  define IS_TERMINAL(fd) (_isatty(fd) != 0)
#  define STDIN_FD  0
#  define STDOUT_FD 1
#else
#  include <unistd.h>
#  include <termios.h>
#  define IS_TERMINAL(fd) (isatty(fd) != 0)
#  define STDIN_FD  STDIN_FILENO
#  define STDOUT_FD STDOUT_FILENO
#endif

namespace claw {

// ---- Free helpers ----

std::optional<std::string_view>
slash_command_prefix(std::string_view line, std::size_t pos) noexcept {
    // pos must be at the end of the string (cursor is at end).
    if (pos != line.size()) return std::nullopt;
    auto prefix = line.substr(0, pos);
    if (prefix.empty() || prefix[0] != '/') return std::nullopt;
    return prefix;
}

// ---- LineEditor ----

std::vector<std::string> LineEditor::normalize_completions(std::vector<std::string> candidates) {
    // Keep only '/' prefixed candidates, deduplicate (preserving insertion order).
    std::set<std::string> seen;
    std::vector<std::string> result;
    for (auto& c : candidates) {
        if (!c.empty() && c[0] == '/' && seen.insert(c).second)
            result.push_back(c);
    }
    return result;
}

LineEditor::LineEditor(std::string prompt, std::vector<std::string> completions)
    : prompt_{std::move(prompt)}
    , completions_{normalize_completions(std::move(completions))}
{}

void LineEditor::push_history(std::string entry) {
    // Mirrors Rust: blank entries (all whitespace) are silently ignored.
    bool blank = std::all_of(entry.begin(), entry.end(),
        [](unsigned char c){ return std::isspace(c); });
    if (!blank)
        history_.push_back(std::move(entry));
}

void LineEditor::set_completions(std::vector<std::string> completions) {
    completions_ = normalize_completions(std::move(completions));
}

std::vector<std::string> LineEditor::complete(std::string_view prefix) const {
    // Return all candidates whose stored string starts with `prefix`.
    std::vector<std::string> matches;
    for (auto& c : completions_) {
        if (c.size() >= prefix.size() &&
            std::string_view(c).substr(0, prefix.size()) == prefix)
        {
            matches.push_back(c);
        }
    }
    return matches;
}

ReadOutcome LineEditor::read_line_fallback() const {
    // Non-interactive (pipe) path: simple getline.
    std::cout << prompt_ << std::flush;
    std::string buffer;
    if (!std::getline(std::cin, buffer))
        return ReadExit{};
    // Strip trailing CR (Windows line endings through pipes).
    if (!buffer.empty() && buffer.back() == '\r')
        buffer.pop_back();
    return ReadSubmit{std::move(buffer)};
}

ReadOutcome LineEditor::read_line() {
    // If either stdin or stdout is not a terminal, fall back to the simple path.
    if (!IS_TERMINAL(STDIN_FD) || !IS_TERMINAL(STDOUT_FD))
        return read_line_fallback();

    // Interactive terminal path.
    // We implement a basic line editor loop:
    //   - History navigation with Up/Down arrow keys.
    //   - Tab completion for slash commands.
    //   - Ctrl-C: if the buffer has content -> Cancel, else -> Exit.
    //   - Ctrl-D on empty buffer -> Exit.
    //
    // On platforms where a full readline library is not linked, this uses raw
    // ANSI escape sequences to move the cursor and rewrite the line.

#ifdef _WIN32
    // Windows: use the simple getline path for now (ConPTY handles most editing).
    return read_line_fallback();
#else
    // POSIX: read raw bytes.  We implement the Rust rustyline behaviour as
    // closely as possible without pulling in an external library.

    struct RawMode {
        int fd;
        struct termios saved{};
        bool active{false};

        explicit RawMode(int fd_) : fd(fd_) {
            if (tcgetattr(fd, &saved) == 0) {
                struct termios raw = saved;
                raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
                raw.c_cc[VMIN]  = 1;
                raw.c_cc[VTIME] = 0;
                if (tcsetattr(fd, TCSANOW, &raw) == 0)
                    active = true;
            }
        }
        ~RawMode() {
            if (active) tcsetattr(fd, TCSANOW, &saved);
        }
    };

    // Print prompt.
    std::cout << prompt_ << std::flush;

    std::string buffer;
    // History navigation index: history_.size() means "current (new) line".
    std::size_t hist_idx = history_.size();
    std::string saved_line; // saved current line while navigating history

    RawMode raw(STDIN_FD);

    auto redraw = [&]() {
        // Move to beginning of line, clear it, reprint prompt + buffer.
        std::cout << "\r\x1b[2K" << prompt_ << buffer << std::flush;
    };

    while (true) {
        int ch = std::cin.get();
        if (!std::cin) {
            // EOF / Ctrl-D on empty.
            if (buffer.empty()) {
                std::cout << "\n" << std::flush;
                return ReadExit{};
            }
            // Ctrl-D with content -> submit.
            std::cout << "\n" << std::flush;
            return ReadSubmit{std::move(buffer)};
        }

        if (ch == 4) { // Ctrl-D
            if (buffer.empty()) {
                std::cout << "\n" << std::flush;
                return ReadExit{};
            }
            // Ctrl-D with content, treat as submit.
            std::cout << "\n" << std::flush;
            return ReadSubmit{std::move(buffer)};
        }

        if (ch == 3) { // Ctrl-C
            bool had_input = !buffer.empty();
            buffer.clear();
            std::cout << "\n" << std::flush;
            return had_input ? ReadOutcome{ReadCancel{}} : ReadOutcome{ReadExit{}};
        }

        if (ch == '\n' || ch == '\r') {
            // Submit.
            std::cout << "\n" << std::flush;
            return ReadSubmit{std::move(buffer)};
        }

        if (ch == 10) { // Ctrl-J -- insert newline into buffer (multi-line).
            buffer += '\n';
            std::cout << "\n" << prompt_ << std::flush;
            continue;
        }

        if (ch == 127 || ch == 8) { // Backspace / DEL
            if (!buffer.empty()) {
                buffer.pop_back();
                redraw();
            }
            continue;
        }

        if (ch == 9) { // Tab -- attempt slash-command completion
            auto pfx = slash_command_prefix(buffer, buffer.size());
            if (pfx) {
                auto matches = complete(*pfx);
                if (matches.size() == 1) {
                    buffer = matches[0];
                    redraw();
                } else if (!matches.empty()) {
                    // Show all options below the current line.
                    std::cout << "\n";
                    for (auto& m : matches)
                        std::cout << "  " << m << "\n";
                    redraw();
                }
            }
            continue;
        }

        if (ch == 27) { // ESC sequence (arrow keys etc.)
            int next = std::cin.get();
            if (!std::cin) continue;
            if (next != '[') {
                // Unknown escape -- ignore.
                continue;
            }
            int code = std::cin.get();
            if (!std::cin) continue;
            if (code == 'A') { // Up arrow -- history back
                if (!history_.empty()) {
                    if (hist_idx == history_.size())
                        saved_line = buffer;
                    if (hist_idx > 0) {
                        --hist_idx;
                        buffer = history_[hist_idx];
                        redraw();
                    }
                }
            } else if (code == 'B') { // Down arrow -- history forward
                if (hist_idx < history_.size()) {
                    ++hist_idx;
                    buffer = (hist_idx == history_.size()) ? saved_line : history_[hist_idx];
                    redraw();
                }
            }
            // Other escape sequences (Left/Right etc.) are ignored.
            continue;
        }

        if (ch >= 32 && ch < 127) { // Printable ASCII
            buffer += static_cast<char>(ch);
            std::cout << static_cast<char>(ch) << std::flush;
            continue;
        }

        // Multi-byte UTF-8 leading byte (0xC0..0xFF).
        if ((ch & 0x80) != 0) {
            buffer += static_cast<char>(ch);
            std::cout << static_cast<char>(ch) << std::flush;
            // Read continuation bytes.
            int extra = (ch & 0xE0) == 0xC0 ? 1
                      : (ch & 0xF0) == 0xE0 ? 2
                      : (ch & 0xF8) == 0xF0 ? 3
                      : 0;
            for (int k = 0; k < extra; ++k) {
                int cb = std::cin.get();
                if (!std::cin) break;
                buffer += static_cast<char>(cb);
                std::cout << static_cast<char>(cb);
            }
            std::cout << std::flush;
            continue;
        }
        // Other control chars -- ignore.
    }
#endif
}

} // namespace claw
