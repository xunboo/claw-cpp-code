#pragma once

#include "scenario.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace claw::mock {

// Detect which scenario is described by the JSON request body.
[[nodiscard]] std::optional<Scenario>
detect_scenario_from_json(const nlohmann::json& j) noexcept;

// Detect scenario from a request JSON body.
[[nodiscard]] std::optional<Scenario>
detect_scenario(const nlohmann::json& req);

// Build an SSE (text/event-stream) body for the given scenario.
[[nodiscard]] std::string
build_stream_body(const nlohmann::json& req, Scenario scenario);

// Build a plain JSON response body for the given scenario.
[[nodiscard]] nlohmann::json
build_json_response(const nlohmann::json& req, Scenario scenario);

// Build full HTTP response string.
[[nodiscard]] std::string
build_http_response(const nlohmann::json& req, Scenario scenario);

}  // namespace claw::mock
