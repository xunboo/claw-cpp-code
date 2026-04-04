// permissions.cpp — faithful C++20 conversion of
// rust/crates/runtime/src/permissions.rs
//
// Rust conventions mapped to C++20:
//   Option<T>          → std::optional<T>
//   Vec<T>             → std::vector<T>
//   BTreeMap<K,V>      → std::map<K,V>  (ordered, like BTreeMap)
//   serde_json::Value  → nlohmann::json
//   &'static str       → std::string_view (constexpr where possible)
//   Result<T,E>        → tl::expected<T,E>  (unused here; all infallible)

#include "permissions.hpp"

#include <algorithm>
#include <cassert>
#include <format>
#include <nlohmann/json.hpp>
#include <string>

namespace claw::runtime {

// ============================================================================
// PermissionMode helpers
// ============================================================================

std::string_view permission_mode_as_str(PermissionMode mode) noexcept {
    switch (mode) {
        case PermissionMode::ReadOnly:         return "read-only";
        case PermissionMode::WorkspaceWrite:   return "workspace-write";
        case PermissionMode::DangerFullAccess: return "danger-full-access";
        case PermissionMode::Prompt:           return "prompt";
        case PermissionMode::Allow:            return "allow";
    }
    return "unknown";
}

// ============================================================================
// Free helpers — find_first_unescaped / find_last_unescaped / unescape
// ============================================================================

// Returns the byte index of the first occurrence of `needle` in `value` that
// is not preceded by a backslash escape.  Returns npos if not found.
//
// Mirrors:
//   fn find_first_unescaped(value: &str, needle: char) -> Option<usize>
std::size_t find_first_unescaped(std::string_view value, char needle) {
    bool escaped = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        char ch = value[i];
        if (ch == '\\') {
            escaped = !escaped;
            continue;
        }
        if (ch == needle && !escaped) {
            return i;
        }
        escaped = false;
    }
    return npos;
}

// Returns the byte index of the last occurrence of `needle` in `value` that
// is not preceded by an odd number of backslashes.  Returns npos if not found.
//
// Mirrors:
//   fn find_last_unescaped(value: &str, needle: char) -> Option<usize>
std::size_t find_last_unescaped(std::string_view value, char needle) {
    // Collect (index, char) pairs, mirroring Rust's char_indices().collect()
    struct CharEntry { std::size_t idx; char ch; };
    std::vector<CharEntry> chars;
    chars.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        chars.push_back({i, value[i]});
    }

    // Iterate in reverse
    for (std::size_t pos = chars.size(); pos-- > 0;) {
        if (chars[pos].ch != needle) continue;

        // Count consecutive backslashes immediately before `pos`
        std::size_t backslashes = 0;
        std::size_t prev = pos;
        while (prev-- > 0 && chars[prev].ch == '\\') {
            ++backslashes;
        }

        if (backslashes % 2 == 0) {
            return chars[pos].idx;
        }
    }
    return npos;
}

// Unescape the content inside parentheses: \( → (, \) → ), \\ → \.
// Mirrors:
//   fn unescape_rule_content(content: &str) -> String
static std::string unescape_rule_content(std::string_view content) {
    std::string result;
    result.reserve(content.size());
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\\' && i + 1 < content.size()) {
            char next = content[i + 1];
            if (next == '(' || next == ')' || next == '\\') {
                result += next;
                ++i;
                continue;
            }
        }
        result += content[i];
    }
    return result;
}

// ============================================================================
// parse_rule_matcher — builds the inner PermissionRuleMatcher from content
//
// Mirrors:
//   fn parse_rule_matcher(content: &str) -> PermissionRuleMatcher
// ============================================================================
static PermissionRuleMatcher parse_rule_matcher(std::string_view content) {
    // Trim leading/trailing whitespace manually (std::string_view::trim not in C++20)
    while (!content.empty() && content.front() == ' ') content.remove_prefix(1);
    while (!content.empty() && content.back()  == ' ') content.remove_suffix(1);

    std::string unescaped = unescape_rule_content(content);

    if (unescaped.empty() || unescaped == "*") {
        return PermissionRuleMatcher{PermissionRuleMatcherKind::Any, {}};
    }

    // strip_suffix(":*")
    if (unescaped.size() >= 2 &&
        unescaped[unescaped.size() - 2] == ':' &&
        unescaped[unescaped.size() - 1] == '*') {
        std::string prefix = unescaped.substr(0, unescaped.size() - 2);
        return PermissionRuleMatcher{PermissionRuleMatcherKind::Prefix, std::move(prefix)};
    }

    return PermissionRuleMatcher{PermissionRuleMatcherKind::Exact, std::move(unescaped)};
}

// ============================================================================
// PermissionRule::parse
//
// Mirrors:
//   fn parse(raw: &str) -> Self
// ============================================================================
PermissionRule PermissionRule::parse(std::string_view raw) {
    // Trim leading/trailing whitespace
    while (!raw.empty() && raw.front() == ' ') raw.remove_prefix(1);
    while (!raw.empty() && raw.back()  == ' ') raw.remove_suffix(1);

    std::string trimmed{raw};

    std::size_t open  = find_first_unescaped(trimmed, '(');
    std::size_t close = find_last_unescaped (trimmed, ')');

    if (open != npos && close != npos) {
        // close must be the very last character, and open < close
        if (close == trimmed.size() - 1 && open < close) {
            // Extract tool_name = trimmed[..open].trim()
            std::string_view tool_part{trimmed.data(), open};
            while (!tool_part.empty() && tool_part.front() == ' ') tool_part.remove_prefix(1);
            while (!tool_part.empty() && tool_part.back()  == ' ') tool_part.remove_suffix(1);

            if (!tool_part.empty()) {
                std::string_view content{trimmed.data() + open + 1, close - open - 1};
                PermissionRuleMatcher m = parse_rule_matcher(content);
                return PermissionRule{
                    .raw       = trimmed,
                    .tool_name = std::string(tool_part),
                    .matcher   = std::move(m),
                };
            }
        }
    }

    // Fallback: the whole trimmed string is the tool name, matcher = Any
    return PermissionRule{
        .raw       = trimmed,
        .tool_name = trimmed,
        .matcher   = PermissionRuleMatcher{PermissionRuleMatcherKind::Any, {}},
    };
}

// ============================================================================
// extract_permission_subject
//
// Parses `input` as JSON and looks for the first matching key from a priority
// list.  Falls back to the raw input string if non-empty.
//
// Mirrors:
//   fn extract_permission_subject(input: &str) -> Option<String>
// ============================================================================
std::optional<std::string> extract_permission_subject(std::string_view input) {
    try {
        auto parsed = nlohmann::json::parse(input);
        if (parsed.is_object()) {
            static constexpr std::string_view KEYS[] = {
                "command",
                "path",
                "file_path",
                "filePath",
                "notebook_path",
                "notebookPath",
                "url",
                "pattern",
                "code",
                "message",
            };
            for (std::string_view key : KEYS) {
                auto it = parsed.find(std::string(key));
                if (it != parsed.end() && it->is_string()) {
                    return it->get<std::string>();
                }
            }
        }
    } catch (...) {
        // Not valid JSON — fall through
    }

    // (!input.trim().is_empty()).then(|| input.to_string())
    std::string_view trimmed = input;
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.remove_prefix(1);
    while (!trimmed.empty() && trimmed.back()  == ' ') trimmed.remove_suffix(1);

    if (!trimmed.empty()) {
        return std::string(input);
    }
    return std::nullopt;
}

// ============================================================================
// PermissionRule::matches
//
// Mirrors:
//   fn matches(&self, tool_name: &str, input: &str) -> bool
// ============================================================================
bool PermissionRule::matches(std::string_view tool_name_in,
                             std::string_view input) const {
    if (tool_name != tool_name_in) {
        return false;
    }

    switch (matcher.kind) {
        case PermissionRuleMatcherKind::Any:
            return true;

        case PermissionRuleMatcherKind::Exact: {
            auto candidate = extract_permission_subject(input);
            return candidate.has_value() && *candidate == matcher.value;
        }

        case PermissionRuleMatcherKind::Prefix: {
            auto candidate = extract_permission_subject(input);
            return candidate.has_value() &&
                   candidate->starts_with(matcher.value);
        }
    }
    return false;
}

// ============================================================================
// PermissionPolicy — constructor
// ============================================================================
PermissionPolicy::PermissionPolicy(PermissionMode active_mode)
    : active_mode_(active_mode) {}

// ============================================================================
// PermissionPolicy::with_tool_requirement
//
// Mirrors:
//   pub fn with_tool_requirement(mut self, tool_name, required_mode) -> Self
// ============================================================================
PermissionPolicy PermissionPolicy::with_tool_requirement(
        std::string    tool_name,
        PermissionMode required_mode) && {
    tool_requirements_.insert_or_assign(std::move(tool_name), required_mode);
    return std::move(*this);
}

// ============================================================================
// PermissionPolicy::with_permission_rules
//
// Mirrors:
//   pub fn with_permission_rules(mut self, config: &RuntimePermissionRuleConfig) -> Self
// ============================================================================
PermissionPolicy PermissionPolicy::with_permission_rules(
        const RuntimePermissionRuleConfig& config) && {
    allow_rules_.clear();
    for (const auto& raw : config.allow()) {
        allow_rules_.push_back(PermissionRule::parse(raw));
    }
    deny_rules_.clear();
    for (const auto& raw : config.deny()) {
        deny_rules_.push_back(PermissionRule::parse(raw));
    }
    ask_rules_.clear();
    for (const auto& raw : config.ask()) {
        ask_rules_.push_back(PermissionRule::parse(raw));
    }
    return std::move(*this);
}

// ============================================================================
// PermissionPolicy::active_mode / required_mode_for
// ============================================================================
PermissionMode PermissionPolicy::active_mode() const noexcept {
    return active_mode_;
}

PermissionMode PermissionPolicy::required_mode_for(
        std::string_view tool_name) const {
    auto it = tool_requirements_.find(std::string(tool_name));
    if (it != tool_requirements_.end()) {
        return it->second;
    }
    // Mirrors: .unwrap_or(PermissionMode::DangerFullAccess)
    return PermissionMode::DangerFullAccess;
}

// ============================================================================
// PermissionPolicy::find_matching_rule  (private static)
//
// Mirrors:
//   fn find_matching_rule<'a>(rules, tool_name, input) -> Option<&'a PermissionRule>
// ============================================================================
const PermissionRule* PermissionPolicy::find_matching_rule(
        const std::vector<PermissionRule>& rules,
        std::string_view                   tool_name,
        std::string_view                   input) {
    for (const auto& rule : rules) {
        if (rule.matches(tool_name, input)) {
            return &rule;
        }
    }
    return nullptr;
}

// ============================================================================
// PermissionPolicy::prompt_or_deny  (private static)
//
// Mirrors:
//   fn prompt_or_deny(tool_name, input, current_mode, required_mode,
//                     reason, prompter) -> PermissionOutcome
// ============================================================================
PermissionOutcome PermissionPolicy::prompt_or_deny(
        std::string_view        tool_name,
        std::string_view        input,
        PermissionMode          current_mode,
        PermissionMode          required_mode,
        std::optional<std::string> reason,
        PermissionPrompter*     prompter) {

    PermissionRequest req{
        .tool_name     = std::string(tool_name),
        .input         = std::string(input),
        .current_mode  = current_mode,
        .required_mode = required_mode,
        .reason        = reason,
    };

    if (prompter != nullptr) {
        auto decision = prompter->decide(req);
        if (decision.allowed) {
            return PermissionOutcome::allow();
        }
        return PermissionOutcome::deny(std::move(decision.reason));
    }

    // No prompter — deny with the provided reason or a default message.
    // Mirrors:
    //   None => PermissionOutcome::Deny {
    //       reason: reason.unwrap_or_else(|| format!(...))
    //   }
    if (reason.has_value()) {
        return PermissionOutcome::deny(std::move(*reason));
    }
    return PermissionOutcome::deny(std::format(
        "tool '{}' requires approval to run while mode is {}",
        tool_name,
        permission_mode_as_str(current_mode)));
}

// ============================================================================
// PermissionPolicy::authorize
//
// Mirrors:
//   pub fn authorize(&self, tool_name, input, prompter) -> PermissionOutcome
// ============================================================================
PermissionOutcome PermissionPolicy::authorize(
        std::string_view    tool_name,
        std::string_view    input,
        PermissionPrompter* prompter) const {
    PermissionContext default_ctx;
    return authorize_with_context(tool_name, input, default_ctx, prompter);
}

// ============================================================================
// PermissionPolicy::authorize_with_context  — the main decision engine
//
// Mirrors:
//   pub fn authorize_with_context(&self, tool_name, input, context, prompter)
//       -> PermissionOutcome
// ============================================================================
PermissionOutcome PermissionPolicy::authorize_with_context(
        std::string_view         tool_name,
        std::string_view         input,
        const PermissionContext& context,
        PermissionPrompter*      prompter) const {

    // ── 1. Deny rules short-circuit first ──────────────────────────────────
    if (const auto* rule = find_matching_rule(deny_rules_, tool_name, input)) {
        return PermissionOutcome::deny(std::format(
            "Permission to use {} has been denied by rule '{}'",
            tool_name,
            rule->raw));
    }

    const PermissionMode current_mode  = active_mode();
    const PermissionMode required_mode = required_mode_for(tool_name);
    const PermissionRule* ask_rule   = find_matching_rule(ask_rules_,   tool_name, input);
    const PermissionRule* allow_rule  = find_matching_rule(allow_rules_, tool_name, input);

    // ── 2. Context override (hook pre-decision) ────────────────────────────
    auto override_decision = context.get_override_decision();

    if (override_decision.has_value()) {
        switch (*override_decision) {

            case PermissionOverride::Deny: {
                // Mirrors: Some(PermissionOverride::Deny) => return Deny { reason }
                std::string reason;
                if (auto r = context.get_override_reason(); r.has_value()) {
                    reason = std::string(*r);
                } else {
                    reason = std::format("tool '{}' denied by hook", tool_name);
                }
                return PermissionOutcome::deny(std::move(reason));
            }

            case PermissionOverride::Ask: {
                // Mirrors: Some(PermissionOverride::Ask) => prompt_or_deny(...)
                std::string reason;
                if (auto r = context.get_override_reason(); r.has_value()) {
                    reason = std::string(*r);
                } else {
                    reason = std::format(
                        "tool '{}' requires approval due to hook guidance",
                        tool_name);
                }
                return prompt_or_deny(
                    tool_name, input,
                    current_mode, required_mode,
                    std::move(reason),
                    prompter);
            }

            case PermissionOverride::Allow: {
                // Mirrors: Some(PermissionOverride::Allow) => {
                //   if ask_rule matched → prompt
                //   else if allow_rule || mode >= required → Allow
                // }
                if (ask_rule != nullptr) {
                    std::string reason = std::format(
                        "tool '{}' requires approval due to ask rule '{}'",
                        tool_name,
                        ask_rule->raw);
                    return prompt_or_deny(
                        tool_name, input,
                        current_mode, required_mode,
                        std::move(reason),
                        prompter);
                }
                if (allow_rule != nullptr ||
                    current_mode == PermissionMode::Allow ||
                    static_cast<int>(current_mode) >= static_cast<int>(required_mode)) {
                    return PermissionOutcome::allow();
                }
                break; // fall through to normal flow
            }
        }
    }

    // ── 3. Ask-rules check ─────────────────────────────────────────────────
    // Mirrors: if let Some(rule) = ask_rule { prompt_or_deny(...) }
    if (ask_rule != nullptr) {
        std::string reason = std::format(
            "tool '{}' requires approval due to ask rule '{}'",
            tool_name,
            ask_rule->raw);
        return prompt_or_deny(
            tool_name, input,
            current_mode, required_mode,
            std::move(reason),
            prompter);
    }

    // ── 4. Allow-rule or mode check ────────────────────────────────────────
    // Mirrors:
    //   if allow_rule.is_some()
    //       || current_mode == Allow
    //       || current_mode >= required_mode
    //   { return Allow; }
    if (allow_rule != nullptr ||
        current_mode == PermissionMode::Allow ||
        static_cast<int>(current_mode) >= static_cast<int>(required_mode)) {
        return PermissionOutcome::allow();
    }

    // ── 5. Prompt modes: Prompt, or WorkspaceWrite→DangerFullAccess ────────
    // Mirrors:
    //   if current_mode == Prompt
    //       || (current_mode == WorkspaceWrite && required_mode == DangerFullAccess)
    //   { prompt_or_deny(...) }
    if (current_mode == PermissionMode::Prompt ||
        (current_mode == PermissionMode::WorkspaceWrite &&
         required_mode == PermissionMode::DangerFullAccess)) {
        std::string reason = std::format(
            "tool '{}' requires approval to escalate from {} to {}",
            tool_name,
            permission_mode_as_str(current_mode),
            permission_mode_as_str(required_mode));
        return prompt_or_deny(
            tool_name, input,
            current_mode, required_mode,
            std::move(reason),
            prompter);
    }

    // ── 6. Hard deny — mode is insufficient and no prompt path ────────────
    // Mirrors:
    //   PermissionOutcome::Deny {
    //       reason: format!("tool '{}' requires {} permission; current mode is {}",
    //                       tool_name, required_mode.as_str(), current_mode.as_str())
    //   }
    return PermissionOutcome::deny(std::format(
        "tool '{}' requires {} permission; current mode is {}",
        tool_name,
        permission_mode_as_str(required_mode),
        permission_mode_as_str(current_mode)));
}

} // namespace claw::runtime
