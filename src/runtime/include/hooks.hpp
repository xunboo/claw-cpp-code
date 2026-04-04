#pragma once
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <atomic>
#include <memory>
#include <variant>
#include <nlohmann/json.hpp>
#include "permissions.hpp"

// ---------------------------------------------------------------------------
// C++20 faithful translation of crates/runtime/src/hooks.rs
// namespace: runtime  (alias exposed as ::runtime for legacy callers)
// ---------------------------------------------------------------------------

namespace claw::runtime {

// ---------------------------------------------------------------------------
// RuntimeHookConfig  (mirrors Rust RuntimeHookConfig in config.rs)
// ---------------------------------------------------------------------------
struct RuntimeHookConfig {
    std::vector<std::string> pre_tool_use;
    std::vector<std::string> post_tool_use;
    std::vector<std::string> post_tool_use_failure;

    RuntimeHookConfig() = default;
    RuntimeHookConfig(std::vector<std::string> pre,
                      std::vector<std::string> post,
                      std::vector<std::string> post_failure)
        : pre_tool_use(std::move(pre))
        , post_tool_use(std::move(post))
        , post_tool_use_failure(std::move(post_failure)) {}

    [[nodiscard]] const std::vector<std::string>& get_pre_tool_use()         const noexcept { return pre_tool_use; }
    [[nodiscard]] const std::vector<std::string>& get_post_tool_use()        const noexcept { return post_tool_use; }
    [[nodiscard]] const std::vector<std::string>& get_post_tool_use_failure() const noexcept { return post_tool_use_failure; }
};

// ---------------------------------------------------------------------------
// RuntimeFeatureConfig  (minimal subset used by HookRunner)
// ---------------------------------------------------------------------------
struct RuntimeFeatureConfig {
    RuntimeHookConfig hooks;

    [[nodiscard]] const RuntimeHookConfig& get_hooks() const noexcept { return hooks; }

    [[nodiscard]] RuntimeFeatureConfig with_hooks(RuntimeHookConfig h) const {
        RuntimeFeatureConfig copy = *this;
        copy.hooks = std::move(h);
        return copy;
    }
};

// ---------------------------------------------------------------------------
// HookEvent  (the three hook lifecycle moments)
// ---------------------------------------------------------------------------
enum class HookEvent {
    PreToolUse,
    PostToolUse,
    PostToolUseFailure,
};

[[nodiscard]] inline const char* hook_event_as_str(HookEvent e) noexcept {
    switch (e) {
        case HookEvent::PreToolUse:          return "PreToolUse";
        case HookEvent::PostToolUse:         return "PostToolUse";
        case HookEvent::PostToolUseFailure:  return "PostToolUseFailure";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// HookProgressEvent  (mirrors Rust enum HookProgressEvent)
// ---------------------------------------------------------------------------
enum class HookProgressKind { Started, Completed, Cancelled };

struct HookProgressEvent {
    HookProgressKind kind;
    HookEvent        event;
    std::string      tool_name;
    std::string      command;
};

// ---------------------------------------------------------------------------
// HookProgressReporter  (mirrors Rust trait HookProgressReporter)
// ---------------------------------------------------------------------------
class HookProgressReporter {
public:
    virtual ~HookProgressReporter() = default;
    virtual void on_event(const HookProgressEvent& event) = 0;
};

// ---------------------------------------------------------------------------
// HookAbortSignal  (mirrors Rust HookAbortSignal with Arc<AtomicBool>)
// ---------------------------------------------------------------------------
class HookAbortSignal {
public:
    HookAbortSignal()
        : aborted_(std::make_shared<std::atomic<bool>>(false)) {}

    // Copyable/movable: shared ownership just like Arc in Rust
    HookAbortSignal(const HookAbortSignal&)            = default;
    HookAbortSignal& operator=(const HookAbortSignal&) = default;
    HookAbortSignal(HookAbortSignal&&)                 = default;
    HookAbortSignal& operator=(HookAbortSignal&&)      = default;

    void abort() noexcept {
        aborted_->store(true, std::memory_order_seq_cst);
    }

    [[nodiscard]] bool is_aborted() const noexcept {
        return aborted_->load(std::memory_order_seq_cst);
    }

private:
    std::shared_ptr<std::atomic<bool>> aborted_;
};

// ---------------------------------------------------------------------------
// HookRunResult  (mirrors Rust struct HookRunResult)
// ---------------------------------------------------------------------------
struct HookRunResult {
    bool denied{false};
    bool failed{false};
    bool cancelled{false};
    std::vector<std::string>     messages;
    std::optional<PermissionOverride> permission_override;
    std::optional<std::string>   permission_reason;
    std::optional<std::string>   updated_input;

    // Factory: allow result with given messages
    [[nodiscard]] static HookRunResult allow(std::vector<std::string> msgs = {}) {
        HookRunResult r;
        r.messages = std::move(msgs);
        return r;
    }

    [[nodiscard]] bool is_denied()    const noexcept { return denied; }
    [[nodiscard]] bool is_failed()    const noexcept { return failed; }
    [[nodiscard]] bool is_cancelled() const noexcept { return cancelled; }

    [[nodiscard]] const std::vector<std::string>& get_messages() const noexcept { return messages; }

    [[nodiscard]] std::optional<PermissionOverride> get_permission_override() const noexcept {
        return permission_override;
    }
    [[nodiscard]] std::optional<PermissionOverride> permission_decision() const noexcept {
        return permission_override;
    }
    [[nodiscard]] std::optional<std::string_view> get_permission_reason() const noexcept {
        if (permission_reason) return std::string_view{*permission_reason};
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string_view> get_updated_input() const noexcept {
        if (updated_input) return std::string_view{*updated_input};
        return std::nullopt;
    }
    // Alias matching Rust updated_input_json()
    [[nodiscard]] std::optional<std::string_view> updated_input_json() const noexcept {
        return get_updated_input();
    }

    bool operator==(const HookRunResult&) const = default;
};

// HookPermissionDecision is a type alias (mirrors Rust pub type)
using HookPermissionDecision = PermissionOverride;

// ---------------------------------------------------------------------------
// HookRunner  (mirrors Rust struct HookRunner + impl)
// ---------------------------------------------------------------------------
class HookRunner {
public:
    HookRunner() = default;
    explicit HookRunner(RuntimeHookConfig config) : config_(std::move(config)) {}

    [[nodiscard]] static HookRunner from_feature_config(const RuntimeFeatureConfig& fc) {
        return HookRunner{fc.get_hooks()};
    }

    // --- PreToolUse ---------------------------------------------------------
    [[nodiscard]] HookRunResult run_pre_tool_use(
        const std::string& tool_name,
        const std::string& tool_input) const;

    [[nodiscard]] HookRunResult run_pre_tool_use_with_signal(
        const std::string& tool_name,
        const std::string& tool_input,
        const HookAbortSignal* abort_signal) const;

    [[nodiscard]] HookRunResult run_pre_tool_use_with_context(
        const std::string& tool_name,
        const std::string& tool_input,
        const HookAbortSignal*  abort_signal,
        HookProgressReporter*   reporter) const;

    // --- PostToolUse --------------------------------------------------------
    [[nodiscard]] HookRunResult run_post_tool_use(
        const std::string& tool_name,
        const std::string& tool_input,
        const std::string& tool_output,
        bool is_error) const;

    [[nodiscard]] HookRunResult run_post_tool_use_with_signal(
        const std::string& tool_name,
        const std::string& tool_input,
        const std::string& tool_output,
        bool is_error,
        const HookAbortSignal* abort_signal) const;

    [[nodiscard]] HookRunResult run_post_tool_use_with_context(
        const std::string& tool_name,
        const std::string& tool_input,
        const std::string& tool_output,
        bool is_error,
        const HookAbortSignal*  abort_signal,
        HookProgressReporter*   reporter) const;

    // --- PostToolUseFailure -------------------------------------------------
    [[nodiscard]] HookRunResult run_post_tool_use_failure(
        const std::string& tool_name,
        const std::string& tool_input,
        const std::string& tool_error) const;

    [[nodiscard]] HookRunResult run_post_tool_use_failure_with_signal(
        const std::string& tool_name,
        const std::string& tool_input,
        const std::string& tool_error,
        const HookAbortSignal* abort_signal) const;

    [[nodiscard]] HookRunResult run_post_tool_use_failure_with_context(
        const std::string& tool_name,
        const std::string& tool_input,
        const std::string& tool_error,
        const HookAbortSignal*  abort_signal,
        HookProgressReporter*   reporter) const;

private:
    RuntimeHookConfig config_;

    [[nodiscard]] static HookRunResult run_commands(
        HookEvent event,
        const std::vector<std::string>& commands,
        const std::string& tool_name,
        const std::string& tool_input,
        const std::optional<std::string>& tool_output,
        bool is_error,
        const HookAbortSignal* abort_signal,
        HookProgressReporter*  reporter);
};

// ---------------------------------------------------------------------------
// HookEventKind — used by conversation.cpp's RuntimeHookRunner to describe
// the type of hook event being dispatched to a HookRunner.
// ---------------------------------------------------------------------------
enum class HookEventKind {
    PreToolCall,
    PostToolCall,
};

// ---------------------------------------------------------------------------
// HookEventData — a structured event passed from RuntimeHookRunner to
// HookRunner::run().  Carries tool name, input, and optional output.
// ---------------------------------------------------------------------------
struct HookEventData {
    HookEventKind     kind{HookEventKind::PreToolCall};
    std::string       tool_name;
    nlohmann::json    tool_input;
    std::optional<std::string> tool_output;
};

// ---------------------------------------------------------------------------
// Hook result types for the variant returned by HookRunner::run()
// ---------------------------------------------------------------------------
struct HookAllow {};
struct HookDeny   { std::string reason; };
struct HookModify { nlohmann::json modified_input; };

using HookRunVariant = std::variant<HookAllow, HookDeny, HookModify>;

} // namespace claw::runtime
