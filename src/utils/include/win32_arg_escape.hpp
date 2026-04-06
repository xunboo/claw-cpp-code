// win32_arg_escape.hpp — Shared Windows argument escaping utility
//
// Implements the CommandLineToArgvW escaping convention for a single argument.
// Use this when building cmdlines for CreateProcess.
// Do NOT use this to wrap an entire shell command string — for shell commands
// use "cmd /C <command>" directly.
#pragma once

#include <string>

namespace claw::util {

/// Escape a single command-line argument for Windows CreateProcess.
///
/// Implements the escaping rules expected by CommandLineToArgvW:
///   - Empty args become ""
///   - Args containing spaces/tabs/quotes/newlines are quoted
///   - Backslashes before quotes are doubled
///
/// IMPORTANT: This is for individual arguments only. Do NOT pass entire
/// shell command strings through this function.
inline std::string escape_win32_arg(const std::string& arg) {
    if (arg.empty()) return "\"\"";
    if (arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    for (size_t i = 0; i < arg.length(); ++i) {
        int backslashes = 0;
        while (i < arg.length() && arg[i] == '\\') { ++backslashes; ++i; }
        if (i == arg.length()) {
            out.append(backslashes * 2, '\\');
            break;
        } else if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(arg[i]);
        }
    }
    out += "\"";
    return out;
}

} // namespace claw::util
