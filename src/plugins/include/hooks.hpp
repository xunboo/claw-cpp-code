#pragma once

#include <tl/expected.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "plugin_error.hpp"
#include "plugin_registry.hpp"
#include "plugin_types.hpp"

namespace claw::plugins {

// ─── HookEvent ────────────────────────────────────────────────────────────────
enum class HookEvent { PreToolUse, PostToolUse, PostToolUseFailure };

[[nodiscard]] inline constexpr const char* hook_event_str(HookEvent e) noexcept {
    switch (e) {
        case HookEvent::PreToolUse:        return "PreToolUse";
        case HookEvent::PostToolUse:       return "PostToolUse";
        case HookEvent::PostToolUseFailure:return "PostToolUseFailure";
    }
    return "PreToolUse";
}

// ─── HookRunResult ────────────────────────────────────────────────────────────
class HookRunResult {
public:
    [[nodiscard]] static HookRunResult allow(std::vector<std::string> messages);

    // Used internally for deny/fail
    static HookRunResult deny(std::vector<std::string> messages);
    static HookRunResult failed(std::vector<std::string> messages);

    [[nodiscard]] bool is_denied()  const noexcept { return denied_;  }
    [[nodiscard]] bool is_failed()  const noexcept { return failed_;  }
    [[nodiscard]] std::span<const std::string> messages() const noexcept {
        return std::span{messages_};
    }

    bool operator==(const HookRunResult&) const noexcept = default;

private:
    bool denied_{false};
    bool failed_{false};
    std::vector<std::string> messages_;
};

// ─── HookRunner ───────────────────────────────────────────────────────────────
// Corresponds to Rust HookRunner
class HookRunner {
public:
    HookRunner() = default;
    explicit HookRunner(PluginHooks hooks);

    [[nodiscard]] static tl::expected<HookRunner, PluginError>
        from_registry(const PluginRegistry& registry);

    [[nodiscard]] HookRunResult run_pre_tool_use(
        std::string_view tool_name, std::string_view tool_input) const;

    [[nodiscard]] HookRunResult run_post_tool_use(
        std::string_view tool_name,
        std::string_view tool_input,
        std::string_view tool_output,
        bool is_error) const;

    [[nodiscard]] HookRunResult run_post_tool_use_failure(
        std::string_view tool_name,
        std::string_view tool_input,
        std::string_view tool_error) const;

private:
    PluginHooks hooks_;

    // Outcome of running a single hook command
    struct CommandOutcome {
        enum class Tag { Allow, Deny, Failed } tag;
        std::optional<std::string> message;  // used for Allow and Deny
        std::string failure_message;         // used for Failed
    };

    [[nodiscard]] static HookRunResult run_commands(
        HookEvent event,
        const std::vector<std::string>& commands,
        std::string_view tool_name,
        std::string_view tool_input,
        std::optional<std::string_view> tool_output,
        bool is_error);

    [[nodiscard]] static CommandOutcome run_command(
        std::string_view command,
        HookEvent event,
        std::string_view tool_name,
        std::string_view tool_input,
        std::optional<std::string_view> tool_output,
        bool is_error,
        std::string_view payload);
};

}  // namespace claw::plugins
