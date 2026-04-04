#include "trust_resolver.hpp"
#include <algorithm>
#include <cctype>

namespace claw::runtime {

namespace {

std::filesystem::path normalize_path(const std::filesystem::path& p) {
    std::error_code ec;
    auto canonical = std::filesystem::canonical(p, ec);
    if (ec) return p;
    return canonical;
}

bool path_matches(std::string_view candidate_sv, const std::filesystem::path& root) {
    std::filesystem::path candidate(candidate_sv);
    auto norm_candidate = normalize_path(candidate);
    auto norm_root = normalize_path(root);
    auto root_str = norm_root.string();
    root_str += static_cast<char>(std::filesystem::path::preferred_separator);
    return norm_candidate == norm_root || norm_candidate.string().starts_with(root_str);
}

} // anonymous namespace

bool detect_trust_prompt(std::string_view screen_text) {
    // Lowercase the input for comparison
    std::string lowered(screen_text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (auto cue : TRUST_PROMPT_CUES) {
        if (lowered.find(cue) != std::string::npos) return true;
    }
    return false;
}

bool path_matches_trusted_root(std::string_view cwd, std::string_view trusted_root) {
    return path_matches(cwd, std::filesystem::path(trusted_root));
}

std::optional<TrustPolicy> trust_decision_policy(const TrustDecision& d) {
    return std::visit([](const auto& v) -> std::optional<TrustPolicy> {
        if constexpr (std::is_same_v<std::decay_t<decltype(v)>, TrustDecisionNotRequired>) {
            return std::nullopt;
        } else {
            return v.policy;
        }
    }, d);
}

const std::vector<TrustEvent>* trust_decision_events(const TrustDecision& d) {
    static const std::vector<TrustEvent> empty{};
    return std::visit([&](const auto& v) -> const std::vector<TrustEvent>* {
        if constexpr (std::is_same_v<std::decay_t<decltype(v)>, TrustDecisionNotRequired>) {
            return &empty;
        } else {
            return &v.events;
        }
    }, d);
}

TrustDecision TrustResolver::resolve(std::string_view cwd, std::string_view screen_text) const {
    if (!detect_trust_prompt(screen_text)) {
        return TrustDecisionNotRequired{};
    }

    std::vector<TrustEvent> events;
    events.push_back(TrustRequired{std::string(cwd)});

    // Check denied list first
    for (const auto& root : config_.denied) {
        if (path_matches(cwd, root)) {
            std::string reason = std::format("cwd matches denied trust root: {}", root.string());
            events.push_back(TrustDenied{std::string(cwd), reason});
            return TrustDecisionRequired{TrustPolicy::Deny, std::move(events)};
        }
    }

    // Check allowlist
    for (const auto& root : config_.allowlisted) {
        if (path_matches(cwd, root)) {
            events.push_back(TrustResolved{std::string(cwd), TrustPolicy::AutoTrust});
            return TrustDecisionRequired{TrustPolicy::AutoTrust, std::move(events)};
        }
    }

    return TrustDecisionRequired{TrustPolicy::RequireApproval, std::move(events)};
}

bool TrustResolver::trusts(std::string_view cwd) const {
    for (const auto& root : config_.denied) {
        if (path_matches(cwd, root)) return false;
    }
    for (const auto& root : config_.allowlisted) {
        if (path_matches(cwd, root)) return true;
    }
    return false;
}

} // namespace claw::runtime
