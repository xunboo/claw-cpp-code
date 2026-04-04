#include "mcp.hpp"
#include <algorithm>
#include <format>
#include <cctype>
#include <array>

namespace claw::runtime {

std::string normalize_name_for_mcp(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == '-' || c == ' ' || c == '.') {
            result += '_';
        }
    }
    return result;
}

std::string mcp_tool_prefix(std::string_view server_name) {
    return std::string(normalize_name_for_mcp(server_name)) + "__";
}

std::string mcp_tool_name(std::string_view server_name, std::string_view tool_name) {
    return mcp_tool_prefix(server_name) + normalize_name_for_mcp(tool_name);
}

std::string stable_hex_hash(std::string_view input) {
    // FNV-1a 32-bit
    uint32_t hash = 2166136261u;
    for (unsigned char c : input) {
        hash ^= static_cast<uint32_t>(c);
        hash *= 16777619u;
    }
    return std::format("{:08x}", hash);
}

std::string percent_encode(std::string_view input) {
    static constexpr std::string_view UNRESERVED =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::string result;
    result.reserve(input.size() * 3);
    for (unsigned char c : input) {
        if (UNRESERVED.find(static_cast<char>(c)) != std::string_view::npos) {
            result += static_cast<char>(c);
        } else {
            result += std::format("%{:02X}", static_cast<unsigned>(c));
        }
    }
    return result;
}

std::string percent_decode(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ) {
        if (input[i] == '%' && i + 2 < input.size()) {
            auto hi = input[i+1], lo = input[i+2];
            auto hex_val = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex_val(hi), l = hex_val(lo);
            if (h >= 0 && l >= 0) {
                result += static_cast<char>((h << 4) | l);
                i += 3;
                continue;
            }
        } else if (input[i] == '+') {
            result += ' ';
            ++i;
            continue;
        }
        result += input[i++];
    }
    return result;
}

} // namespace claw::runtime
