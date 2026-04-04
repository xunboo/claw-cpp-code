#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <variant>
#include <optional>

namespace claw::runtime {

inline constexpr std::string_view TRUST_PROMPT_CUES[] = {
    "do you trust the files in this folder",
    "trust the files in this folder",
    "trust this folder",
    "allow and continue",
    "yes, proceed",
};

enum class TrustPolicy {
    AutoTrust,
    RequireApproval,
    Deny,
};

struct TrustRequired { std::string cwd; };
struct TrustResolved { std::string cwd; TrustPolicy policy; };
struct TrustDenied   { std::string cwd; std::string reason; };

using TrustEvent = std::variant<TrustRequired, TrustResolved, TrustDenied>;

struct TrustConfig {
    std::vector<std::filesystem::path> allowlisted;
    std::vector<std::filesystem::path> denied;

    TrustConfig& with_allowlisted(std::filesystem::path path) {
        allowlisted.push_back(std::move(path));
        return *this;
    }
    TrustConfig& with_denied(std::filesystem::path path) {
        denied.push_back(std::move(path));
        return *this;
    }
};

struct TrustDecisionNotRequired {};
struct TrustDecisionRequired {
    TrustPolicy policy;
    std::vector<TrustEvent> events;
};

using TrustDecision = std::variant<TrustDecisionNotRequired, TrustDecisionRequired>;

[[nodiscard]] std::optional<TrustPolicy> trust_decision_policy(const TrustDecision& d);
[[nodiscard]] const std::vector<TrustEvent>* trust_decision_events(const TrustDecision& d);

[[nodiscard]] bool detect_trust_prompt(std::string_view screen_text);
[[nodiscard]] bool path_matches_trusted_root(std::string_view cwd, std::string_view trusted_root);

class TrustResolver {
public:
    explicit TrustResolver(TrustConfig config) : config_(std::move(config)) {}

    [[nodiscard]] TrustDecision resolve(std::string_view cwd, std::string_view screen_text) const;
    [[nodiscard]] bool trusts(std::string_view cwd) const;

private:
    TrustConfig config_;
};

} // namespace claw::runtime
