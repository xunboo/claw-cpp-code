#pragma once

#include "error.hpp"
#include "types.hpp"
#include <optional>
#include <string>
#include <vector>

namespace claw::api {

/// Incremental SSE parser.  Buffers raw bytes and emits complete StreamEvents.
/// Mirrors the Rust SseParser struct and its push()/finish() methods.
class SseParser {
public:
    SseParser() = default;

    /// Feed a raw byte chunk into the parser.
    /// Returns all events that could be fully parsed from the buffered data.
    [[nodiscard]] std::vector<StreamEvent> push(std::string_view chunk);

    /// Flush any trailing data that was not terminated by a double-newline.
    [[nodiscard]] std::vector<StreamEvent> finish();

private:
    std::string buffer_;

    /// Extract the next complete SSE frame (terminated by \n\n or \r\n\r\n).
    /// Returns std::nullopt when no complete frame is available yet.
    [[nodiscard]] std::optional<std::string> next_frame();
};

/// Parse a single SSE frame string into a StreamEvent.
/// Returns std::nullopt for ping frames, empty frames, and [DONE] sentinels.
[[nodiscard]] std::optional<StreamEvent> parse_frame(std::string_view frame);

} // namespace claw::api