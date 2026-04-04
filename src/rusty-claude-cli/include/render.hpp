#pragma once
// render.hpp -- C++20 port of render.rs
// Terminal rendering: ColorTheme, Spinner, MarkdownStreamState, TerminalRenderer

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace claw {

// ---- ANSI colour helpers ----

/// A 3-bit/8-colour ANSI colour code.
enum class AnsiColor : int {
    Black       = 30,
    Red         = 31,
    Green       = 32,
    Yellow      = 33,
    Blue        = 34,
    Magenta     = 35,
    Cyan        = 36,
    White       = 37,
    DarkGrey    = 90,  // bright-black
    DarkCyan    = 36,  // alias -- reuse cyan for dark-cyan compat
    Reset       = 0,
};

/// Build an ANSI SGR escape sequence for a foreground colour.
[[nodiscard]] std::string ansi_fg(AnsiColor c);
/// Build an ANSI SGR bold escape.
[[nodiscard]] std::string ansi_bold();
/// Build an ANSI SGR italic escape.
[[nodiscard]] std::string ansi_italic();
/// Build an ANSI SGR underline escape.
[[nodiscard]] std::string ansi_underline();
/// Reset all SGR attributes.
[[nodiscard]] std::string ansi_reset();
/// Apply a 256-colour background (used for code blocks, e.g. colour 236).
[[nodiscard]] std::string ansi_bg256(int colour_index);

// ---- ColorTheme ----

/// Mirrors Rust's ColorTheme struct.  All fields are ANSI colour codes.
struct ColorTheme {
    AnsiColor heading{AnsiColor::Cyan};
    AnsiColor emphasis{AnsiColor::Magenta};
    AnsiColor strong{AnsiColor::Yellow};
    AnsiColor inline_code{AnsiColor::Green};
    AnsiColor link{AnsiColor::Blue};
    AnsiColor quote{AnsiColor::DarkGrey};
    AnsiColor table_border{AnsiColor::DarkCyan};
    AnsiColor code_block_border{AnsiColor::DarkGrey};
    AnsiColor spinner_active{AnsiColor::Blue};
    AnsiColor spinner_done{AnsiColor::Green};
    AnsiColor spinner_failed{AnsiColor::Red};
};

// ---- Spinner ----

/// Mirrors Rust's Spinner struct.
/// Renders a braille spinner to the terminal using ANSI cursor-control escapes.
class Spinner {
public:
    static constexpr const char* FRAMES[] = {
        "\u283b", "\u2819", "\u2839", "\u2838", "\u283c",
        "\u2834", "\u2826", "\u2827", "\u2807", "\u280f"
    };
    static constexpr int FRAME_COUNT = 10;

    Spinner() = default;

    /// Overwrite the current terminal line with an in-progress frame + label.
    void tick(std::string_view label, const ColorTheme& theme, std::ostream& out);

    /// Overwrite the current line with a success checkmark + label, then newline.
    void finish(std::string_view label, const ColorTheme& theme, std::ostream& out);

    /// Overwrite the current line with a failure X + label, then newline.
    void fail(std::string_view label, const ColorTheme& theme, std::ostream& out);

private:
    std::size_t frame_index_{0};
};

// ---- Internal render state types ----

enum class ListKind { Unordered, Ordered };

struct ListState {
    ListKind kind{ListKind::Unordered};
    std::uint64_t next_index{1};
};

struct LinkState {
    std::string destination;
    std::string text;
};

struct TableState {
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> current_row;
    std::string current_cell;
    bool in_head{false};

    void push_cell();
    void finish_row();
};

struct RenderState {
    std::size_t emphasis{0};
    std::size_t strong{0};
    std::optional<uint8_t> heading_level;
    std::size_t quote{0};
    std::vector<ListState> list_stack;
    std::vector<LinkState> link_stack;
    std::optional<TableState> table;

    [[nodiscard]] std::string style_text(std::string_view text, const ColorTheme& theme) const;
    void append_raw(std::string& output, std::string_view text);
    void append_styled(std::string& output, std::string_view text, const ColorTheme& theme);
};

// ---- TerminalRenderer ----

/// Mirrors Rust's TerminalRenderer struct.
/// Converts Markdown text to ANSI-coloured terminal output.
/// Syntax highlighting uses a simple keyword-based fallback
/// (no external syntect dependency).
class TerminalRenderer {
public:
    TerminalRenderer();

    [[nodiscard]] const ColorTheme& color_theme() const noexcept { return color_theme_; }

    /// Render a complete Markdown string and return the ANSI-annotated result.
    [[nodiscard]] std::string render_markdown(std::string_view markdown) const;

    /// Alias for render_markdown (mirrors Rust's markdown_to_ansi).
    [[nodiscard]] std::string markdown_to_ansi(std::string_view markdown) const;

    /// Syntax-highlight a code snippet and return ANSI-escaped lines.
    [[nodiscard]] std::string highlight_code(std::string_view code, std::string_view language) const;

    /// Write rendered Markdown to an ostream, appending a trailing newline if missing.
    void stream_markdown(std::string_view markdown, std::ostream& out) const;

private:
    ColorTheme color_theme_;

    void render_event(
        std::string_view event_type,
        std::string_view payload,
        RenderState& state,
        std::string& output,
        std::string& code_buffer,
        std::string& code_language,
        bool& in_code_block
    ) const;

    static void start_heading(RenderState& state, uint8_t level, std::string& output);
    void start_quote(RenderState& state, std::string& output) const;
    static void start_item(RenderState& state, std::string& output);
    void start_code_block(std::string_view lang, std::string& output) const;
    void finish_code_block(std::string_view code, std::string_view lang, std::string& output) const;
    void push_text(std::string_view text, RenderState& state,
                   std::string& output, std::string& code_buf, bool in_code) const;

    [[nodiscard]] std::string render_table(const TableState& table) const;
    [[nodiscard]] std::string render_table_row(
        const std::vector<std::string>& row,
        const std::vector<std::size_t>& widths,
        bool is_header) const;
};

// ---- MarkdownStreamState ----

/// Mirrors Rust's MarkdownStreamState struct.
/// Buffers incremental markdown deltas and flushes only at paragraph/block
/// boundaries to avoid torn ANSI sequences.
class MarkdownStreamState {
public:
    /// Accumulate delta; if a safe flush boundary is found, returns the
    /// rendered ANSI string for the ready portion; otherwise returns nullopt.
    [[nodiscard]] std::optional<std::string>
    push(const TerminalRenderer& renderer, std::string_view delta);

    /// Flush any remaining buffered content regardless of boundaries.
    [[nodiscard]] std::optional<std::string>
    flush(const TerminalRenderer& renderer);

private:
    std::string pending_;
};

// ---- free helpers ----

/// Strip ANSI escape sequences from a string (for width-calculation / assertions).
[[nodiscard]] std::string strip_ansi(std::string_view input);

/// Return the printable (visible) column width of a potentially ANSI-escaped string.
[[nodiscard]] std::size_t visible_width(std::string_view input);

/// Find the byte offset at which buffered markdown can be safely flushed.
/// Returns 0 if no boundary exists yet.
[[nodiscard]] std::size_t find_stream_safe_boundary(std::string_view markdown);

} // namespace claw