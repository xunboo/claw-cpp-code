#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <map>
#include <nlohmann/json.hpp>

namespace claw::runtime {

// ---------------------------------------------------------------------------
// PermissionMode — ordered so that >= comparisons work as in Rust (Ord derive)
// ReadOnly < WorkspaceWrite < DangerFullAccess < Prompt < Allow
// ---------------------------------------------------------------------------

/// Permission level assigned to a tool invocation or runtime session.
enum class PermissionMode {
    ReadOnly         = 0,
    WorkspaceWrite   = 1,
    DangerFullAccess = 2,
    Prompt           = 3,
    Allow            = 4,
};

[[nodiscard]] std::string_view permission_mode_as_str(PermissionMode mode) noexcept;

// ---------------------------------------------------------------------------
// PermissionOverride — used by PermissionContext (hook pre-decisions)
// ---------------------------------------------------------------------------

/// Hook-provided override applied before standard permission evaluation.
enum class PermissionOverride {
    Allow,
    Deny,
    Ask,
};

// ---------------------------------------------------------------------------
// PermissionContext — carries an optional hook override decision + reason
// ---------------------------------------------------------------------------

/// Additional permission context supplied by hooks or higher-level orchestration.
struct PermissionContext {
    std::optional<PermissionOverride> override_decision;
    std::optional<std::string>        override_reason;

    PermissionContext() = default;
    PermissionContext(std::optional<PermissionOverride> decision,
                      std::optional<std::string>        reason)
        : override_decision(std::move(decision))
        , override_reason(std::move(reason)) {}

    [[nodiscard]] std::optional<PermissionOverride> get_override_decision() const noexcept {
        return override_decision;
    }
    [[nodiscard]] std::optional<std::string_view> get_override_reason() const noexcept {
        if (override_reason.has_value())
            return std::string_view(*override_reason);
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// PermissionRequest — passed to the prompter
// ---------------------------------------------------------------------------

/// Full authorization request presented to a permission prompt.
struct PermissionRequest {
    std::string    tool_name;
    std::string    input;
    PermissionMode current_mode;
    PermissionMode required_mode;
    std::optional<std::string> reason;
};

// ---------------------------------------------------------------------------
// PermissionPromptDecision — what the prompter decided
// ---------------------------------------------------------------------------

/// User-facing decision returned by a PermissionPrompter.
struct PermissionPromptDecision {
    bool        allowed{false};
    std::string reason;  // non-empty when denied

    static PermissionPromptDecision allow() { return {true, {}}; }
    static PermissionPromptDecision deny(std::string r) { return {false, std::move(r)}; }
};

// ---------------------------------------------------------------------------
// PermissionPrompter — abstract interface (mirrors Rust trait)
// ---------------------------------------------------------------------------

/// Prompting interface used when policy requires interactive approval.
class PermissionPrompter {
public:
    virtual ~PermissionPrompter() = default;
    virtual PermissionPromptDecision decide(const PermissionRequest& request) = 0;
};

// ---------------------------------------------------------------------------
// PermissionOutcome — result of a permission check
// ---------------------------------------------------------------------------

/// Final authorization result after evaluating static rules and prompts.
struct PermissionOutcome {
    bool        allowed{false};
    std::string deny_reason;

    [[nodiscard]] static PermissionOutcome allow() { return {true, {}}; }
    [[nodiscard]] static PermissionOutcome deny(std::string reason) {
        return {false, std::move(reason)};
    }
    [[nodiscard]] bool is_allow() const noexcept { return allowed; }
    [[nodiscard]] bool is_deny()  const noexcept { return !allowed; }

    bool operator==(const PermissionOutcome&) const = default;
};

// ---------------------------------------------------------------------------
// PermissionRuleMatcher — inner detail, exposed for testing
// ---------------------------------------------------------------------------
enum class PermissionRuleMatcherKind { Any, Exact, Prefix };

struct PermissionRuleMatcher {
    PermissionRuleMatcherKind kind{PermissionRuleMatcherKind::Any};
    std::string               value;  // used for Exact / Prefix

    bool operator==(const PermissionRuleMatcher&) const = default;
};

// ---------------------------------------------------------------------------
// PermissionRule — a parsed allow/deny/ask rule
// ---------------------------------------------------------------------------
struct PermissionRule {
    std::string            raw;
    std::string            tool_name;
    PermissionRuleMatcher  matcher;

    [[nodiscard]] bool matches(std::string_view tool_name_in,
                               std::string_view input) const;

    bool operator==(const PermissionRule&) const = default;

    // Parse a raw rule string (e.g. "bash(git:*)")
    [[nodiscard]] static PermissionRule parse(std::string_view raw);
};

// ---------------------------------------------------------------------------
// RuntimePermissionRuleConfig — mirrors Rust struct (used by PermissionPolicy)
// ---------------------------------------------------------------------------
struct RuntimePermissionRuleConfig {
    std::vector<std::string> allow_rules;
    std::vector<std::string> deny_rules;
    std::vector<std::string> ask_rules;

    RuntimePermissionRuleConfig() = default;
    RuntimePermissionRuleConfig(std::vector<std::string> allow,
                                std::vector<std::string> deny,
                                std::vector<std::string> ask)
        : allow_rules(std::move(allow))
        , deny_rules(std::move(deny))
        , ask_rules(std::move(ask)) {}

    [[nodiscard]] const std::vector<std::string>& allow() const noexcept { return allow_rules; }
    [[nodiscard]] const std::vector<std::string>& deny()  const noexcept { return deny_rules;  }
    [[nodiscard]] const std::vector<std::string>& ask()   const noexcept { return ask_rules;   }
};

// ---------------------------------------------------------------------------
// PermissionPolicy — the central decision engine
// ---------------------------------------------------------------------------

/// Evaluates permission mode requirements plus allow/deny/ask rules.
class PermissionPolicy {
public:
    explicit PermissionPolicy(PermissionMode active_mode);

    // Builder-style configuration (mirrors Rust `with_*` methods)
    [[nodiscard]] PermissionPolicy with_tool_requirement(std::string tool_name,
                                                         PermissionMode required_mode) &&;
    [[nodiscard]] PermissionPolicy with_permission_rules(const RuntimePermissionRuleConfig& config) &&;

    [[nodiscard]] PermissionMode active_mode() const noexcept;
    [[nodiscard]] PermissionMode required_mode_for(std::string_view tool_name) const;

    [[nodiscard]] PermissionOutcome authorize(
        std::string_view  tool_name,
        std::string_view  input,
        PermissionPrompter* prompter = nullptr) const;

    [[nodiscard]] PermissionOutcome authorize_with_context(
        std::string_view         tool_name,
        std::string_view         input,
        const PermissionContext& context,
        PermissionPrompter*      prompter = nullptr) const;

private:
    PermissionMode                   active_mode_;
    std::map<std::string, PermissionMode> tool_requirements_;
    std::vector<PermissionRule>      allow_rules_;
    std::vector<PermissionRule>      deny_rules_;
    std::vector<PermissionRule>      ask_rules_;

    static PermissionOutcome prompt_or_deny(
        std::string_view        tool_name,
        std::string_view        input,
        PermissionMode          current_mode,
        PermissionMode          required_mode,
        std::optional<std::string> reason,
        PermissionPrompter*     prompter);

    static const PermissionRule* find_matching_rule(
        const std::vector<PermissionRule>& rules,
        std::string_view                   tool_name,
        std::string_view                   input);
};

// ---------------------------------------------------------------------------
// Free helpers (also tested directly)
// ---------------------------------------------------------------------------
[[nodiscard]] std::optional<std::string> extract_permission_subject(std::string_view input);

[[nodiscard]] std::size_t find_first_unescaped(std::string_view value, char needle);
[[nodiscard]] std::size_t find_last_unescaped(std::string_view value, char needle);

// sentinel returned when not found
inline constexpr std::size_t npos = std::string_view::npos;

} // namespace claw::runtime
