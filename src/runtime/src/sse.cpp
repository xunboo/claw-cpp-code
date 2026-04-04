#include "sse.hpp"
#include <charconv>

namespace claw::runtime {

void IncrementalSseParser::push_chunk(std::string_view chunk) {
    buffer_ += chunk;

    // Process complete lines
    while (true) {
        auto pos = buffer_.find('\n');
        if (pos == std::string::npos) break;

        std::string_view line(buffer_.data(), pos);
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        process_line(line);
        buffer_.erase(0, pos + 1);
    }
}

void IncrementalSseParser::finish() {
    // Process any remaining buffered content as a line
    if (!buffer_.empty()) {
        process_line(buffer_);
        buffer_.clear();
    }
    // Dispatch final event if any data accumulated
    if (has_data_) {
        dispatch_event();
    }
}

void IncrementalSseParser::process_line(std::string_view line) {
    if (line.empty()) {
        // Empty line = dispatch event
        if (has_data_) {
            dispatch_event();
        }
        return;
    }

    // Comment line
    if (line.front() == ':') {
        return;
    }

    auto colon = line.find(':');
    std::string_view field, value;
    if (colon == std::string_view::npos) {
        field = line;
        value = {};
    } else {
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        // Strip single leading space from value if present
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
    }

    if (field == "data") {
        if (has_data_) {
            current_data_ += '\n';
        }
        current_data_ += value;
        has_data_ = true;
    } else if (field == "event") {
        current_event_ = std::string(value);
    } else if (field == "id") {
        current_id_ = std::string(value);
    } else if (field == "retry") {
        uint32_t ms{};
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), ms);
        if (ec == std::errc{}) {
            current_retry_ = ms;
        }
    }
    // Unknown fields are ignored per SSE spec
}

void IncrementalSseParser::dispatch_event() {
    SseEvent ev{
        .event_type = std::move(current_event_),
        .data = std::move(current_data_),
        .id = std::move(current_id_),
        .retry_ms = current_retry_,
    };
    current_event_.clear();
    current_data_.clear();
    current_id_.clear();
    current_retry_ = std::nullopt;
    has_data_ = false;

    callback_(std::move(ev));
}

} // namespace claw::runtime
