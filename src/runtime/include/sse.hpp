#pragma once
#include <string>
#include <optional>
#include <vector>
#include <functional>

namespace claw::runtime {

struct SseEvent {
    std::string event_type; // empty if no "event:" field
    std::string data;
    std::string id;
    std::optional<uint32_t> retry_ms;
};

// Callback invoked for each complete SSE event
using SseEventCallback = std::function<void(SseEvent)>;

class IncrementalSseParser {
public:
    explicit IncrementalSseParser(SseEventCallback callback)
        : callback_(std::move(callback)) {}

    // Push a chunk of raw SSE bytes; may call callback zero or more times
    void push_chunk(std::string_view chunk);

    // Signal end of stream; flush any remaining partial event
    void finish();

private:
    void process_line(std::string_view line);
    void dispatch_event();

    SseEventCallback callback_;
    std::string buffer_;          // Unparsed trailing bytes
    std::string current_event_;   // Accumulates "event:" field
    std::string current_data_;    // Accumulates "data:" lines (newline-joined)
    std::string current_id_;
    std::optional<uint32_t> current_retry_;
    bool has_data_{false};
};

} // namespace claw::runtime
