#pragma once
#include <string>
#include <vector>
#include <optional>
#include <tl/expected.hpp>
#include <chrono>

namespace claw::runtime {

inline constexpr std::size_t BASH_OUTPUT_TRUNCATE_BYTES = 16384;

struct BashCommandInput {
    std::string command;
    std::optional<std::string> cwd;
    std::optional<std::chrono::milliseconds> timeout;
    std::vector<std::string> extra_env; // "KEY=VALUE" pairs
    bool sandbox{false};
};

struct BashCommandOutput {
    std::string stdout_output;
    std::string stderr_output;
    int exit_code{0};
    bool timed_out{false};
    bool truncated{false};
};

// Synchronous bash execution
[[nodiscard]] tl::expected<BashCommandOutput, std::string>
    execute_bash(const BashCommandInput& input);

// Truncate output to BASH_OUTPUT_TRUNCATE_BYTES if needed
[[nodiscard]] std::string truncate_output(std::string output);

// Build the shell command string with optional sandbox dirs
[[nodiscard]] std::string prepare_command(const BashCommandInput& input,
                                          const std::vector<std::string>& sandbox_dirs);

} // namespace claw::runtime
