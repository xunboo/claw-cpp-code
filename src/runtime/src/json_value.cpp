#include "json_value.hpp"
#include <cctype>
#include <charconv>
#include <format>
#include <stdexcept>

namespace claw::runtime {

namespace {

struct Parser {
    std::string_view input;
    std::size_t pos{0};

    void skip_ws() {
        while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) ++pos;
    }

    char peek() const { return pos < input.size() ? input[pos] : '\0'; }
    char consume() { return pos < input.size() ? input[pos++] : '\0'; }

    void expect(char c) {
        if (peek() != c) throw std::runtime_error(std::format("expected '{}' at pos {}", c, pos));
        ++pos;
    }

    JsonValue parse_value() {
        skip_ws();
        if (pos >= input.size()) throw std::runtime_error("unexpected end of input");
        char c = peek();
        if (c == '"') return parse_string();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == 't') { pos += 4; return JsonBool{true}; }
        if (c == 'f') { pos += 5; return JsonBool{false}; }
        if (c == 'n') { pos += 4; return JsonNull{}; }
        return parse_number();
    }

    JsonString parse_string() {
        expect('"');
        std::string result;
        while (pos < input.size() && peek() != '"') {
            char ch = consume();
            if (ch == '\\') {
                char esc = consume();
                switch (esc) {
                    case '"':  result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/':  result += '/'; break;
                    case 'n':  result += '\n'; break;
                    case 'r':  result += '\r'; break;
                    case 't':  result += '\t'; break;
                    case 'b':  result += '\b'; break;
                    case 'f':  result += '\f'; break;
                    default: result += esc; break;
                }
            } else {
                result += ch;
            }
        }
        expect('"');
        return result;
    }

    JsonNumber parse_number() {
        std::size_t start = pos;
        if (peek() == '-') ++pos;
        while (pos < input.size() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.' || peek() == 'e' || peek() == 'E' || peek() == '+' || peek() == '-')) ++pos;
        double val{};
        auto sv = input.substr(start, pos - start);
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
        if (ec != std::errc{}) throw std::runtime_error(std::format("invalid number at pos {}", start));
        return val;
    }

    JsonObject parse_object() {
        expect('{');
        JsonObject obj;
        skip_ws();
        if (peek() == '}') { ++pos; return obj; }
        while (true) {
            skip_ws();
            auto key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            auto val = parse_value();
            obj.emplace(std::move(key), std::move(val));
            skip_ws();
            if (peek() == '}') { ++pos; break; }
            expect(',');
        }
        return obj;
    }

    JsonArray parse_array() {
        expect('[');
        JsonArray arr;
        skip_ws();
        if (peek() == ']') { ++pos; return arr; }
        while (true) {
            skip_ws();
            arr.push_back(parse_value());
            skip_ws();
            if (peek() == ']') { ++pos; break; }
            expect(',');
        }
        return arr;
    }
};

} // anonymous namespace

JsonValue parse_json(std::string_view input) {
    Parser p{input, 0};
    return p.parse_value();
}

std::string render_json(const JsonValue& value, bool pretty, int indent) {
    return std::visit([&](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, JsonNull>) return "null";
        else if constexpr (std::is_same_v<T, JsonBool>) return v ? "true" : "false";
        else if constexpr (std::is_same_v<T, JsonNumber>) {
            if (v == static_cast<double>(static_cast<int64_t>(v))) {
                return std::format("{}", static_cast<int64_t>(v));
            }
            return std::format("{}", v);
        }
        else if constexpr (std::is_same_v<T, JsonString>) {
            std::string result = "\"";
            for (char c : v) {
                switch (c) {
                    case '"': result += "\\\""; break;
                    case '\\': result += "\\\\"; break;
                    case '\n': result += "\\n"; break;
                    case '\r': result += "\\r"; break;
                    case '\t': result += "\\t"; break;
                    default: result += c;
                }
            }
            result += '"';
            return result;
        }
        else if constexpr (std::is_same_v<T, JsonArray>) {
            std::string result = "[";
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (i > 0) result += ',';
                if (pretty) result += '\n' + std::string(static_cast<std::size_t>((indent + 1) * 2), ' ');
                result += render_json(v[i], pretty, indent + 1);
            }
            if (pretty && !v.empty()) result += '\n' + std::string(static_cast<std::size_t>(indent * 2), ' ');
            result += ']';
            return result;
        }
        else if constexpr (std::is_same_v<T, JsonObject>) {
            std::string result = "{";
            bool first = true;
            for (const auto& [k, val] : v) {
                if (!first) result += ',';
                if (pretty) result += '\n' + std::string(static_cast<std::size_t>((indent + 1) * 2), ' ');
                result += '"' + k + "\":";
                if (pretty) result += ' ';
                result += render_json(val, pretty, indent + 1);
                first = false;
            }
            if (pretty && !v.empty()) result += '\n' + std::string(static_cast<std::size_t>(indent * 2), ' ');
            result += '}';
            return result;
        }
    }, value);
}

} // namespace claw::runtime
