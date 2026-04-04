#pragma once
// Custom lightweight JSON value type (mirrors json.rs hand-written parser).
// For most purposes, prefer nlohmann/json. This is a minimal alternative
// that avoids external dependencies in contexts where only basic JSON is needed.
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <optional>
#include <stdexcept>

namespace claw::runtime {

struct JsonValue;

using JsonNull    = std::monostate;
using JsonBool    = bool;
using JsonNumber  = double;
using JsonString  = std::string;
using JsonArray   = std::vector<JsonValue>;
using JsonObject  = std::unordered_map<std::string, JsonValue>;

struct JsonValue : std::variant<JsonNull, JsonBool, JsonNumber, JsonString, JsonArray, JsonObject> {
    using Base = std::variant<JsonNull, JsonBool, JsonNumber, JsonString, JsonArray, JsonObject>;
    using Base::Base;
    using Base::operator=;

    [[nodiscard]] bool is_null()   const noexcept { return std::holds_alternative<JsonNull>(*this); }
    [[nodiscard]] bool is_bool()   const noexcept { return std::holds_alternative<JsonBool>(*this); }
    [[nodiscard]] bool is_number() const noexcept { return std::holds_alternative<JsonNumber>(*this); }
    [[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<JsonString>(*this); }
    [[nodiscard]] bool is_array()  const noexcept { return std::holds_alternative<JsonArray>(*this); }
    [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<JsonObject>(*this); }

    [[nodiscard]] const JsonString& as_string() const { return std::get<JsonString>(*this); }
    [[nodiscard]] double            as_number() const { return std::get<JsonNumber>(*this); }
    [[nodiscard]] bool              as_bool()   const { return std::get<JsonBool>(*this); }
    [[nodiscard]] const JsonArray&  as_array()  const { return std::get<JsonArray>(*this); }
    [[nodiscard]] const JsonObject& as_object() const { return std::get<JsonObject>(*this); }
};

// Parse a JSON string into JsonValue; throws std::runtime_error on parse error
[[nodiscard]] JsonValue parse_json(std::string_view input);

// Render a JsonValue to a JSON string
[[nodiscard]] std::string render_json(const JsonValue& value, bool pretty = false, int indent = 0);

} // namespace claw::runtime
