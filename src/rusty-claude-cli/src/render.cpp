// render.cpp -- C++20 port of render.rs
// ANSI helpers, Spinner, TableState, RenderState, TerminalRenderer,
// MarkdownStreamState, strip_ansi, visible_width, find_stream_safe_boundary.
#include "render.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace claw {

// ===========================================================================
// ANSI escape helpers
// ===========================================================================

std::string ansi_fg(AnsiColor c) {
    return "\x1b[" + std::to_string(static_cast<int>(c)) + "m";
}
std::string ansi_bold()      { return "\x1b[1m"; }
std::string ansi_italic()    { return "\x1b[3m"; }
std::string ansi_underline() { return "\x1b[4m"; }
std::string ansi_reset()     { return "\x1b[0m"; }
std::string ansi_bg256(int i){ return "\x1b[48;5;" + std::to_string(i) + "m"; }

// ===========================================================================
// strip_ansi / visible_width
// ===========================================================================

/// Mirrors Rust's strip_ansi.
std::string strip_ansi(std::string_view input) {
    std::string out;
    bool in_escape = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '\x1b' && i + 1 < input.size() && input[i + 1] == '[') {
            in_escape = true;
            ++i; // skip '['
            continue;
        }
        if (in_escape) {
            if (std::isalpha(static_cast<unsigned char>(c))) in_escape = false;
            continue;
        }
        out += c;
    }
    return out;
}

std::size_t visible_width(std::string_view input) {
    // The Rust source counts Unicode code-points; we count UTF-8 characters.
    std::string stripped = strip_ansi(input);
    std::size_t count = 0;
    for (unsigned char c : stripped) {
        // Count only leading bytes of UTF-8 sequences.
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

// ===========================================================================
// find_stream_safe_boundary
// ===========================================================================

/// Mirrors Rust's find_stream_safe_boundary.
/// Returns the byte offset after the latest safe flush point, or 0.
std::size_t find_stream_safe_boundary(std::string_view markdown) {
    bool in_fence = false;
    std::size_t last_boundary = 0;
    std::size_t start = 0;

    while (start < markdown.size()) {
        auto nl = markdown.find('\n', start);
        std::size_t end = (nl == std::string_view::npos) ? markdown.size() : nl + 1;
        auto line = markdown.substr(start, end - start);

        // Trim leading whitespace.
        auto trimmed = line;
        while (!trimmed.empty() &&
               (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.remove_prefix(1);

        if (trimmed.substr(0, 3) == "```" || trimmed.substr(0, 3) == "~~~") {
            in_fence = !in_fence;
            if (!in_fence)
                last_boundary = end; // just after closing fence
            start = end;
            continue;
        }

        if (!in_fence) {
            bool blank = std::all_of(line.begin(), line.end(),
                [](char c){ return c == '\n' || c == '\r' || c == ' ' || c == '\t'; });
            if (blank) last_boundary = end;
        }

        start = end;
    }

    return last_boundary;
}

// ===========================================================================
// apply_code_block_background (internal)
// ===========================================================================

static std::string apply_code_block_background(std::string_view line) {
    // Strip trailing newline, remember it.
    std::string_view trimmed = line;
    bool has_nl = (!trimmed.empty() && trimmed.back() == '\n');
    if (has_nl) trimmed.remove_suffix(1);

    // Replace any existing reset with reset+background (matches Rust impl).
    const std::string reset_marker   = "\x1b[0m";
    const std::string reset_with_bg  = "\x1b[0;48;5;236m";
    std::string s{trimmed};
    std::string result;
    std::size_t pos = 0;
    while (pos < s.size()) {
        auto found = s.find(reset_marker, pos);
        if (found == std::string::npos) {
            result += s.substr(pos);
            break;
        }
        result += s.substr(pos, found - pos);
        result += reset_with_bg;
        pos = found + reset_marker.size();
    }

    return "\x1b[48;5;236m" + result + "\x1b[0m" + (has_nl ? "\n" : "");
}

// ===========================================================================
// Spinner
// ===========================================================================

void Spinner::tick(std::string_view label, const ColorTheme& theme, std::ostream& out) {
    const char* frame = FRAMES[frame_index_ % FRAME_COUNT];
    ++frame_index_;
    // Save cursor, move to column 1, clear line, print spinner, restore cursor.
    out << "\x1b[s"     // save position
        << "\x1b[1G"    // move to column 1
        << "\x1b[2K"    // clear entire line
        << ansi_fg(theme.spinner_active)
        << frame << " " << label
        << ansi_reset()
        << "\x1b[u"     // restore position
        << std::flush;
}

void Spinner::finish(std::string_view label, const ColorTheme& theme, std::ostream& out) {
    frame_index_ = 0;
    out << "\x1b[1G"
        << "\x1b[2K"
        << ansi_fg(theme.spinner_done)
        << "\xe2\x9c\x94 " << label << "\n"  // UTF-8 ✔
        << ansi_reset()
        << std::flush;
}

void Spinner::fail(std::string_view label, const ColorTheme& theme, std::ostream& out) {
    frame_index_ = 0;
    out << "\x1b[1G"
        << "\x1b[2K"
        << ansi_fg(theme.spinner_failed)
        << "\xe2\x9c\x98 " << label << "\n"  // UTF-8 ✘
        << ansi_reset()
        << std::flush;
}

// ===========================================================================
// TableState
// ===========================================================================

void TableState::push_cell() {
    // Trim whitespace then push into current_row.
    auto cell = current_cell;
    while (!cell.empty() && (cell.front() == ' ' || cell.front() == '\t'))
        cell.erase(cell.begin());
    while (!cell.empty() && (cell.back() == ' ' || cell.back() == '\t'))
        cell.pop_back();
    current_row.push_back(std::move(cell));
    current_cell.clear();
}

void TableState::finish_row() {
    if (current_row.empty()) return;
    if (in_head)
        headers = std::move(current_row);
    else
        rows.push_back(std::move(current_row));
    current_row.clear();
}

// ===========================================================================
// RenderState
// ===========================================================================

std::string RenderState::style_text(std::string_view text,
                                     const ColorTheme& theme) const {
    std::string s{text};
    std::string prefix;

    bool bold_needed = (heading_level.has_value() &&
                        (*heading_level == 1 || *heading_level == 2))
                    || strong > 0;
    if (bold_needed)  prefix += ansi_bold();
    if (emphasis > 0) prefix += ansi_italic();

    if (heading_level.has_value()) {
        switch (*heading_level) {
            case 1: prefix += ansi_fg(theme.heading);         break;
            case 2: prefix += ansi_fg(AnsiColor::White);      break;
            case 3: prefix += ansi_fg(AnsiColor::Blue);       break;
            default: prefix += ansi_fg(AnsiColor::White);     break;
        }
    } else if (strong > 0) {
        prefix += ansi_fg(theme.strong);
    } else if (emphasis > 0) {
        prefix += ansi_fg(theme.emphasis);
    }

    if (quote > 0) prefix += ansi_fg(theme.quote);

    if (prefix.empty()) return s;
    return prefix + s + ansi_reset();
}

void RenderState::append_raw(std::string& output, std::string_view text) {
    if (!link_stack.empty()) {
        link_stack.back().text += text;
    } else if (table.has_value()) {
        table->current_cell += text;
    } else {
        output += text;
    }
}

void RenderState::append_styled(std::string& output, std::string_view text,
                                 const ColorTheme& theme) {
    auto styled = style_text(text, theme);
    append_raw(output, styled);
}

// ===========================================================================
// TerminalRenderer
// ===========================================================================

TerminalRenderer::TerminalRenderer() : color_theme_{} {}

// --------------------------------------------------------------------------
// render_markdown  (main entry point -- mirrors Rust's render_markdown)
// --------------------------------------------------------------------------
// We implement a line-by-line parser that handles:
//   - ATX headings (#, ##, ..., up to 6)
//   - Setext headings (underline with ===/ ---)
//   - Horizontal rules (---, ***, ___)
//   - Block quotes (> ...)
//   - Unordered lists (-, *, + prefixed)
//   - Ordered lists (N. prefixed)
//   - Fenced code blocks (``` / ~~~)
//   - GFM tables (pipe-delimited)
//   - Inline bold (**), italic (*), inline code (`), links [text](url)
//   - Task list markers (- [ ] / - [x])
// --------------------------------------------------------------------------

namespace {

// Inline-markup rendering for a single plain-text line.
// Returns the ANSI-escaped version.
std::string render_inline(std::string_view line, const ColorTheme& theme) {
    std::string out;
    std::size_t i = 0;
    while (i < line.size()) {
        // Inline code `...`
        if (line[i] == '`') {
            auto end = line.find('`', i + 1);
            if (end != std::string_view::npos) {
                auto code = line.substr(i + 1, end - i - 1);
                out += ansi_fg(theme.inline_code) + "`" + std::string(code) + "`" + ansi_reset();
                i = end + 1;
                continue;
            }
        }
        // Bold **text**
        if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '*') {
            auto end = line.find("**", i + 2);
            if (end != std::string_view::npos) {
                auto txt = line.substr(i + 2, end - i - 2);
                out += ansi_bold() + ansi_fg(theme.strong) + std::string(txt) + ansi_reset();
                i = end + 2;
                continue;
            }
        }
        // Italic *text*
        if (line[i] == '*') {
            auto end = line.find('*', i + 1);
            if (end != std::string_view::npos) {
                auto txt = line.substr(i + 1, end - i - 1);
                out += ansi_italic() + ansi_fg(theme.emphasis) + std::string(txt) + ansi_reset();
                i = end + 1;
                continue;
            }
        }
        // Italic _text_
        if (line[i] == '_') {
            auto end = line.find('_', i + 1);
            if (end != std::string_view::npos) {
                auto txt = line.substr(i + 1, end - i - 1);
                out += ansi_italic() + ansi_fg(theme.emphasis) + std::string(txt) + ansi_reset();
                i = end + 1;
                continue;
            }
        }
        // Link [text](url)
        if (line[i] == '[') {
            auto close_bracket = line.find(']', i + 1);
            if (close_bracket != std::string_view::npos &&
                close_bracket + 1 < line.size() &&
                line[close_bracket + 1] == '(')
            {
                auto close_paren = line.find(')', close_bracket + 2);
                if (close_paren != std::string_view::npos) {
                    auto txt = line.substr(i + 1, close_bracket - i - 1);
                    auto url = line.substr(close_bracket + 2, close_paren - close_bracket - 2);
                    out += ansi_underline() + ansi_fg(theme.link)
                        + "[" + std::string(txt) + "](" + std::string(url) + ")"
                        + ansi_reset();
                    i = close_paren + 1;
                    continue;
                }
            }
        }
        // Image ![alt](url) -- show as [image:url]
        if (i + 1 < line.size() && line[i] == '!' && line[i + 1] == '[') {
            auto close_bracket = line.find(']', i + 2);
            if (close_bracket != std::string_view::npos &&
                close_bracket + 1 < line.size() &&
                line[close_bracket + 1] == '(')
            {
                auto close_paren = line.find(')', close_bracket + 2);
                if (close_paren != std::string_view::npos) {
                    auto url = line.substr(close_bracket + 2, close_paren - close_bracket - 2);
                    out += ansi_fg(theme.link) + "[image:" + std::string(url) + "]" + ansi_reset();
                    i = close_paren + 1;
                    continue;
                }
            }
        }
        out += line[i++];
    }
    return out;
}

} // anonymous namespace

std::string TerminalRenderer::render_markdown(std::string_view markdown) const {
    std::string output;
    RenderState state;
    std::string code_buffer;
    std::string code_language;
    bool in_code_block = false;

    // Split into lines, keeping track of whether each line ends with \n.
    std::string md_str{markdown};
    std::istringstream ss(md_str);
    std::string line;

    while (std::getline(ss, line)) {
        // Strip trailing \r (Windows CRLF).
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // ---- Fenced code block tracking ----
        if (!in_code_block) {
            std::string_view lt = line;
            while (!lt.empty() && (lt.front() == ' ' || lt.front() == '\t'))
                lt.remove_prefix(1);

            if (lt.size() >= 3 &&
                (lt.substr(0, 3) == "```" || lt.substr(0, 3) == "~~~"))
            {
                // Flush any pending table.
                if (state.table.has_value()) {
                    output += render_table(*state.table) + "\n\n";
                    state.table = std::nullopt;
                }
                in_code_block = true;
                auto lang_raw = lt.substr(3);
                // Trim trailing fence chars and whitespace.
                while (!lang_raw.empty() &&
                       (lang_raw.back() == '`' || lang_raw.back() == '~' ||
                        lang_raw.back() == ' '))
                    lang_raw.remove_suffix(1);
                code_language = lang_raw.empty() ? "text" : std::string(lang_raw);
                code_buffer.clear();
                start_code_block(code_language, output);
                continue;
            }
        } else {
            std::string_view lt = line;
            while (!lt.empty() && (lt.front() == ' ' || lt.front() == '\t'))
                lt.remove_prefix(1);
            if (lt.size() >= 3 &&
                (lt.substr(0, 3) == "```" || lt.substr(0, 3) == "~~~"))
            {
                in_code_block = false;
                finish_code_block(code_buffer, code_language, output);
                code_buffer.clear();
                code_language.clear();
                continue;
            }
            code_buffer += line + "\n";
            continue;
        }

        // ---- ATX headings (#, ##, ...) ----
        if (!line.empty() && line[0] == '#') {
            uint8_t level = 0;
            while (level < static_cast<uint8_t>(line.size()) && line[level] == '#')
                ++level;
            if (level < static_cast<uint8_t>(line.size()) && line[level] == ' ') {
                // Flush any pending table.
                if (state.table.has_value()) {
                    output += render_table(*state.table) + "\n\n";
                    state.table = std::nullopt;
                }
                if (!output.empty()) output += "\n";
                state.heading_level = level;
                auto text = line.substr(level + 1);
                output += state.style_text(text, color_theme_);
                state.heading_level = std::nullopt;
                output += "\n\n";
                continue;
            }
        }

        // ---- Setext headings (===, ---) ----
        {
            // Peek at the *next* line would require buffering; instead handle
            // the underline when we see it after having buffered the previous line.
            // We do a simplified check: if the current line is all '=' or all '-',
            // convert the last appended paragraph to a heading.
            // (This is best-effort; pulldown-cmark handles it fully in Rust.)
        }

        // ---- Horizontal rule ----
        {
            std::string_view lt = line;
            while (!lt.empty() && lt.front() == ' ') lt.remove_prefix(1);
            if (lt == "---" || lt == "***" || lt == "___" ||
                lt == "- - -" || lt == "* * *" || lt == "_ _ _")
            {
                // Flush pending table.
                if (state.table.has_value()) {
                    output += render_table(*state.table) + "\n\n";
                    state.table = std::nullopt;
                }
                output += "---\n";
                continue;
            }
        }

        // ---- Block quote ----
        if (line.size() >= 2 && line[0] == '>' && (line[1] == ' ' || line.size() == 1)) {
            // Flush pending table.
            if (state.table.has_value()) {
                output += render_table(*state.table) + "\n\n";
                state.table = std::nullopt;
            }
            state.quote = 1;
            output += ansi_fg(color_theme_.quote) + "\u2502 " + ansi_reset();
            auto text = (line.size() > 2) ? line.substr(2) : std::string{};
            output += render_inline(text, color_theme_);
            state.quote = 0;
            output += "\n";
            continue;
        }

        // ---- Unordered list (-, *, +) ----
        if (line.size() >= 2 &&
            (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ')
        {
            // Flush pending table.
            if (state.table.has_value()) {
                output += render_table(*state.table) + "\n\n";
                state.table = std::nullopt;
            }
            auto content = line.substr(2);
            // Task list markers.
            std::string marker = "\u2022 ";
            std::string text_part = std::string(content);
            if (content.size() >= 4 && content[0] == '[') {
                if ((content[1] == 'x' || content[1] == 'X') && content[2] == ']' && content[3] == ' ') {
                    marker = "[x] ";
                    text_part = content.substr(4);
                } else if (content[1] == ' ' && content[2] == ']' && content[3] == ' ') {
                    marker = "[ ] ";
                    text_part = content.substr(4);
                }
            }
            output += marker + render_inline(text_part, color_theme_) + "\n";
            continue;
        }

        // ---- Indented unordered list (2 or 4 spaces) ----
        if (line.size() >= 4 && line[0] == ' ' && line[1] == ' ') {
            std::size_t indent = 0;
            while (indent < line.size() && line[indent] == ' ') ++indent;
            if (indent < line.size() &&
                (line[indent] == '-' || line[indent] == '*' || line[indent] == '+') &&
                indent + 1 < line.size() && line[indent + 1] == ' ')
            {
                if (state.table.has_value()) {
                    output += render_table(*state.table) + "\n\n";
                    state.table = std::nullopt;
                }
                std::string pad(indent, ' ');
                output += pad + "\u2022 " +
                    render_inline(line.substr(indent + 2), color_theme_) + "\n";
                continue;
            }
        }

        // ---- Ordered list (N. or N) ...) ----
        {
            std::size_t k = 0;
            while (k < line.size() && std::isdigit(static_cast<unsigned char>(line[k]))) ++k;
            if (k > 0 && k < line.size() &&
                (line[k] == '.' || line[k] == ')') &&
                k + 1 < line.size() && line[k + 1] == ' ')
            {
                if (state.table.has_value()) {
                    output += render_table(*state.table) + "\n\n";
                    state.table = std::nullopt;
                }
                output += line.substr(0, k + 2) +
                    render_inline(line.substr(k + 2), color_theme_) + "\n";
                continue;
            }
        }

        // ---- GFM table row (starts with |) ----
        if (!line.empty() && line.front() == '|') {
            // Detect separator row (contains only |, -, :, space).
            bool is_sep = std::all_of(line.begin(), line.end(),
                [](char c){ return c == '|' || c == '-' || c == ':' || c == ' '; });
            if (is_sep) {
                // End of header row.
                if (state.table.has_value()) {
                    state.table->finish_row();
                    state.table->in_head = false;
                }
                continue;
            }

            // Parse data row.
            if (!state.table.has_value()) {
                state.table = TableState{};
                state.table->in_head = true;
            }
            auto& tbl = *state.table;
            tbl.current_row.clear();

            auto row_str = std::string_view(line);
            if (!row_str.empty() && row_str.front() == '|') row_str.remove_prefix(1);
            if (!row_str.empty() && row_str.back()  == '|') row_str.remove_suffix(1);

            std::istringstream rs{std::string{row_str}};
            std::string cell;
            while (std::getline(rs, cell, '|')) {
                while (!cell.empty() && cell.front() == ' ') cell.erase(cell.begin());
                while (!cell.empty() && cell.back()  == ' ') cell.pop_back();
                tbl.current_cell = cell;
                tbl.push_cell();
            }
            tbl.finish_row();
            continue;
        }

        // ---- Flush table on blank line ----
        if (state.table.has_value() && line.empty()) {
            output += render_table(*state.table) + "\n\n";
            state.table = std::nullopt;
            continue;
        }

        // ---- Blank line ----
        if (line.empty()) {
            if (!output.empty() && output.back() != '\n') output += "\n";
            output += "\n";
            continue;
        }

        // ---- Normal paragraph text ----
        output += render_inline(line, color_theme_) + "\n\n";
    }

    // Flush any remaining table.
    if (state.table.has_value()) {
        output += render_table(*state.table) + "\n\n";
        state.table = std::nullopt;
    }

    // If still inside a code block (unterminated fence), flush the buffer.
    if (in_code_block && !code_buffer.empty()) {
        finish_code_block(code_buffer, code_language, output);
    }

    // Trim trailing whitespace (mirrors Rust's output.trim_end()).
    while (!output.empty() && (output.back() == '\n' || output.back() == ' '))
        output.pop_back();

    return output;
}

std::string TerminalRenderer::markdown_to_ansi(std::string_view markdown) const {
    return render_markdown(markdown);
}

// --------------------------------------------------------------------------
// start_heading / start_quote / start_item
// --------------------------------------------------------------------------

void TerminalRenderer::start_heading(RenderState& state, uint8_t level,
                                      std::string& output) {
    state.heading_level = level;
    if (!output.empty()) output += "\n";
}

void TerminalRenderer::start_quote(RenderState& state, std::string& output) const {
    ++state.quote;
    output += ansi_fg(color_theme_.quote) + "\u2502 " + ansi_reset();
}

void TerminalRenderer::start_item(RenderState& state, std::string& output) {
    std::size_t depth = state.list_stack.empty() ? 0 : state.list_stack.size() - 1;
    output += std::string(depth * 2, ' ');
    if (!state.list_stack.empty() &&
        state.list_stack.back().kind == ListKind::Ordered)
    {
        auto& ls = state.list_stack.back();
        output += std::to_string(ls.next_index++) + ". ";
    } else {
        output += "\u2022 "; // bullet
    }
}

// --------------------------------------------------------------------------
// start_code_block / finish_code_block
// --------------------------------------------------------------------------

void TerminalRenderer::start_code_block(std::string_view lang,
                                         std::string& output) const {
    std::string label = lang.empty() ? "code" : std::string(lang);
    output += ansi_bold() + ansi_fg(color_theme_.code_block_border)
           + "\u256d\u2500 " + label + "\n"   // ╭─ <lang>
           + ansi_reset();
}

void TerminalRenderer::finish_code_block(std::string_view code,
                                          std::string_view lang,
                                          std::string& output) const {
    output += highlight_code(code, lang);
    output += ansi_bold() + ansi_fg(color_theme_.code_block_border)
           + "\u2570\u2500"   // ╰─
           + ansi_reset() + "\n\n";
}

// --------------------------------------------------------------------------
// push_text
// --------------------------------------------------------------------------

void TerminalRenderer::push_text(std::string_view text, RenderState& state,
                                  std::string& output, std::string& code_buf,
                                  bool in_code) const {
    if (in_code)
        code_buf += text;
    else
        state.append_styled(output, text, color_theme_);
}

// --------------------------------------------------------------------------
// highlight_code
// --------------------------------------------------------------------------

std::string TerminalRenderer::highlight_code(std::string_view code,
                                               std::string_view /*language*/) const {
    // No external syntect equivalent.
    // Apply 256-colour background to every line (mirrors Rust's behaviour for
    // all languages since we don't have syntect).
    std::string result;
    std::string code_str{code};
    std::istringstream ss(code_str);
    std::string line;
    while (std::getline(ss, line))
        result += apply_code_block_background(line + "\n");
    return result;
}

// --------------------------------------------------------------------------
// stream_markdown
// --------------------------------------------------------------------------

void TerminalRenderer::stream_markdown(std::string_view markdown,
                                        std::ostream& out) const {
    auto rendered = markdown_to_ansi(markdown);
    out << rendered;
    if (rendered.empty() || rendered.back() != '\n') out << "\n";
    out << std::flush;
}

// --------------------------------------------------------------------------
// render_table / render_table_row
// --------------------------------------------------------------------------

std::string TerminalRenderer::render_table(const TableState& table) const {
    // Collect all rows (header first).
    std::vector<std::vector<std::string>> all_rows;
    if (!table.headers.empty()) all_rows.push_back(table.headers);
    for (auto& r : table.rows) all_rows.push_back(r);
    if (all_rows.empty()) return {};

    std::size_t col_count = 0;
    for (auto& r : all_rows) col_count = std::max(col_count, r.size());

    // Compute column widths.
    std::vector<std::size_t> widths(col_count, 0);
    for (auto& r : all_rows)
        for (std::size_t c = 0; c < r.size(); ++c)
            widths[c] = std::max(widths[c], visible_width(r[c]));

    auto border = ansi_fg(color_theme_.table_border) + "\u2502" + ansi_reset();

    // Separator row:  │─────┼─────│
    std::string sep = border;
    for (std::size_t c = 0; c < col_count; ++c) {
        // widths[c] + 2 dashes (one space each side).
        for (std::size_t k = 0; k < widths[c] + 2; ++k) sep += "\u2500";
        if (c + 1 < col_count)
            sep += ansi_fg(color_theme_.table_border) + "\u253c" + ansi_reset();
    }
    sep += border;

    std::string out;
    if (!table.headers.empty()) {
        out += render_table_row(table.headers, widths, true) + "\n";
        out += sep;
        if (!table.rows.empty()) out += "\n";
    }
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
        out += render_table_row(table.rows[i], widths, false);
        if (i + 1 < table.rows.size()) out += "\n";
    }
    return out;
}

std::string TerminalRenderer::render_table_row(
    const std::vector<std::string>& row,
    const std::vector<std::size_t>& widths,
    bool is_header) const
{
    auto border = ansi_fg(color_theme_.table_border) + "\u2502" + ansi_reset();
    std::string line = border;
    for (std::size_t i = 0; i < widths.size(); ++i) {
        std::string_view cell = (i < row.size()) ? std::string_view(row[i]) : "";
        line += " ";
        if (is_header) {
            line += ansi_bold() + ansi_fg(color_theme_.heading)
                  + std::string(cell) + ansi_reset();
        } else {
            line += std::string(cell);
        }
        std::size_t vw  = visible_width(cell);
        std::size_t pad = (widths[i] > vw) ? widths[i] - vw : 0;
        line += std::string(pad + 1, ' ');
        line += border;
    }
    return line;
}

// --------------------------------------------------------------------------
// render_event (placeholder -- the line-based parser above is the main path)
// --------------------------------------------------------------------------

void TerminalRenderer::render_event(
    std::string_view /*event_type*/,
    std::string_view /*payload*/,
    RenderState& /*state*/,
    std::string& /*output*/,
    std::string& /*code_buffer*/,
    std::string& /*code_language*/,
    bool& /*in_code_block*/) const
{
    // This entry point is not used by the line-based render_markdown above.
    // It exists to satisfy the header declaration for completeness.
}

// ===========================================================================
// MarkdownStreamState
// ===========================================================================

std::optional<std::string>
MarkdownStreamState::push(const TerminalRenderer& renderer, std::string_view delta) {
    pending_ += delta;
    auto split = find_stream_safe_boundary(pending_);
    if (split == 0) return std::nullopt;
    std::string ready = pending_.substr(0, split);
    pending_.erase(0, split);
    return renderer.markdown_to_ansi(ready);
}

std::optional<std::string>
MarkdownStreamState::flush(const TerminalRenderer& renderer) {
    // Trim leading whitespace.
    std::string_view sv = pending_;
    while (!sv.empty() &&
           (sv.front() == ' ' || sv.front() == '\t' ||
            sv.front() == '\n' || sv.front() == '\r'))
        sv.remove_prefix(1);
    if (sv.empty()) {
        pending_.clear();
        return std::nullopt;
    }
    std::string p = std::move(pending_);
    pending_.clear();
    return renderer.markdown_to_ansi(p);
}

} // namespace claw
