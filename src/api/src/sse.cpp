// ---------------------------------------------------------------------------
// sse.cpp  -  SSE parser implementation
// ---------------------------------------------------------------------------

#include "sse.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace claw::api {

// ── SseParser::next_frame ────────────────────────────────────────────────────

std::optional<std::string> SseParser::next_frame() {
    // Look for \n\n or \r\n\r\n separator.
    auto find_sep = [&](std::string_view sep) -> std::optional<std::pair<size_t,size_t>> {
        auto pos = buffer_.find(sep);
        if (pos == std::string::npos) return std::nullopt;
        return std::make_pair(pos, sep.size());
    };

    std::optional<std::pair<size_t,size_t>> found;
    if (auto p = find_sep("\n\n"); p) found = p;
    else if (auto p2 = find_sep("\r\n\r\n"); p2) found = p2;

    if (!found) return std::nullopt;

    auto [pos, sep_len] = *found;
    std::string frame = buffer_.substr(0, pos);
    buffer_.erase(0, pos + sep_len);
    return frame;
}

// ── SseParser::push ──────────────────────────────────────────────────────────

std::vector<StreamEvent> SseParser::push(std::string_view chunk) {
    buffer_.append(chunk);
    std::vector<StreamEvent> events;
    while (auto frame = next_frame()) {
        if (auto evt = parse_frame(*frame); evt)
            events.push_back(std::move(*evt));
    }
    return events;
}

// ── SseParser::finish ────────────────────────────────────────────────────────

std::vector<StreamEvent> SseParser::finish() {
    if (buffer_.empty()) return {};
    std::string trailing = std::move(buffer_);
    buffer_.clear();
    if (auto evt = parse_frame(trailing); evt)
        return {std::move(*evt)};
    return {};
}

// ── parse_frame ──────────────────────────────────────────────────────────────

std::optional<StreamEvent> parse_frame(std::string_view frame_view) {
    // Trim leading/trailing whitespace
    auto trim = [](std::string_view sv) -> std::string_view {
        while (!sv.empty() && (sv.front()==' '||sv.front()=='\t'||sv.front()=='\n'||sv.front()=='\r'))
            sv.remove_prefix(1);
        while (!sv.empty() && (sv.back()==' '||sv.back()=='\t'||sv.back()=='\n'||sv.back()=='\r'))
            sv.remove_suffix(1);
        return sv;
    };

    auto trimmed = trim(frame_view);
    if (trimmed.empty()) return std::nullopt;

    std::vector<std::string_view> data_lines;
    std::optional<std::string_view> event_name;

    // Split into lines
    size_t start = 0;
    while (start <= trimmed.size()) {
        size_t nl = trimmed.find('\n', start);
        std::string_view line = (nl == std::string_view::npos)
            ? trimmed.substr(start)
            : trimmed.substr(start, nl - start);
        // strip trailing \r
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        start = (nl == std::string_view::npos) ? trimmed.size()+1 : nl+1;

        if (line.empty()) continue;
        if (line.front() == ':') continue;  // comment
        if (line.substr(0, 6) == "event:") {
            auto name = line.substr(6);
            while (!name.empty() && (name.front()==' '||name.front()=='\t'||name.front()=='\n'||name.front()=='\r')) name.remove_prefix(1);
            while (!name.empty() && (name.back()==' '||name.back()=='\t'||name.back()=='\n'||name.back()=='\r')) name.remove_suffix(1);
            event_name = name;
            continue;
        }
        if (line.substr(0, 5) == "data:") {
            auto data = line.substr(5);
            while (!data.empty() && (data.front()==' '||data.front()=='\t'||data.front()=='\n'||data.front()=='\r')) data.remove_prefix(1);
            data_lines.push_back(data);
        }
    }

    if (event_name && *event_name == "ping") return std::nullopt;
    if (data_lines.empty()) return std::nullopt;

    // Join multi-line data
    std::string payload;
    for (size_t i = 0; i < data_lines.size(); ++i) {
        if (i > 0) payload += '\n';
        payload += data_lines[i];
    }
    if (payload == "[DONE]") return std::nullopt;

    try {
        auto j = nlohmann::json::parse(payload);
        StreamEvent evt;
        from_json(j, evt);
        return evt;
    } catch (const nlohmann::json::exception& e) {
        throw ApiError::json(e.what());
    } catch (const std::exception& e) {
        throw ApiError::json(e.what());
    }
}

} // namespace claw::api