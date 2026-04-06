// git_ref_validation.hpp — Shared git ref validation utility
//
// Validates that a string is a safe git ref before interpolating it into
// shell commands, preventing command injection via malicious branch names.
#pragma once

#include <string>

namespace claw::util {

/// Returns true if `ref` looks like a safe git ref name.
/// Rejects empty strings, strings starting with '-' (flags), and
/// strings containing characters outside the safe set.
inline bool is_safe_git_ref(const std::string& ref) {
    if (ref.empty() || ref[0] == '-') return false;
    return ref.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789_.-/") == std::string::npos;
}

} // namespace claw::util
