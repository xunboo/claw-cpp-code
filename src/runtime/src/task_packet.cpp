// task_packet.cpp -- translated from Rust task_packet.rs
// Simplified task packet: all fields are plain strings with required-field validation.

#include "task_packet.hpp"
#include <algorithm>
#include <format>

namespace claw::runtime {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Returns true when the string is empty or contains only whitespace.
static bool is_blank(const std::string& s) {
    return s.empty() ||
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

static void validate_required(const char* field, const std::string& value,
                               std::vector<std::string>& errors) {
    if (is_blank(value)) {
        errors.push_back(std::format("{} must not be empty", field));
    }
}

// ---------------------------------------------------------------------------
// TaskPacketValidationError
// ---------------------------------------------------------------------------

std::string TaskPacketValidationError::to_string() const {
    std::string result;
    for (std::size_t i = 0; i < errors.size(); ++i) {
        if (i > 0) result += "; ";
        result += errors[i];
    }
    return result;
}

// ---------------------------------------------------------------------------
// validate_packet
// ---------------------------------------------------------------------------

tl::expected<ValidatedPacket, TaskPacketValidationError>
validate_packet(const TaskPacket& packet) {
    std::vector<std::string> errors;

    validate_required("objective", packet.objective, errors);
    validate_required("scope", packet.scope, errors);
    validate_required("repo", packet.repo, errors);
    validate_required("branch_policy", packet.branch_policy, errors);
    validate_required("commit_policy", packet.commit_policy, errors);
    validate_required("reporting_contract", packet.reporting_contract, errors);
    validate_required("escalation_policy", packet.escalation_policy, errors);

    for (std::size_t i = 0; i < packet.acceptance_tests.size(); ++i) {
        if (is_blank(packet.acceptance_tests[i])) {
            errors.push_back(
                std::format("acceptance_tests contains an empty value at index {}", i));
        }
    }

    if (!errors.empty()) {
        return tl::unexpected(TaskPacketValidationError{std::move(errors)});
    }
    return ValidatedPacket{packet};
}

// ---------------------------------------------------------------------------
// JSON serialization
// ---------------------------------------------------------------------------

void to_json(nlohmann::json& j, const TaskPacket& p) {
    j = nlohmann::json{
        {"objective", p.objective},
        {"scope", p.scope},
        {"repo", p.repo},
        {"branch_policy", p.branch_policy},
        {"acceptance_tests", p.acceptance_tests},
        {"commit_policy", p.commit_policy},
        {"reporting_contract", p.reporting_contract},
        {"escalation_policy", p.escalation_policy},
    };
}

void from_json(const nlohmann::json& j, TaskPacket& p) {
    j.at("objective").get_to(p.objective);
    j.at("scope").get_to(p.scope);
    j.at("repo").get_to(p.repo);
    j.at("branch_policy").get_to(p.branch_policy);
    j.at("acceptance_tests").get_to(p.acceptance_tests);
    j.at("commit_policy").get_to(p.commit_policy);
    j.at("reporting_contract").get_to(p.reporting_contract);
    j.at("escalation_policy").get_to(p.escalation_policy);
}

} // namespace claw::runtime
