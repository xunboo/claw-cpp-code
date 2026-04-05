#pragma once
#include <string>
#include <vector>
#include <optional>
#include <tl/expected.hpp>
#include <nlohmann/json.hpp>

namespace claw::runtime {

/// Simplified task packet: all fields are plain strings.
/// Mirrors Rust TaskPacket after simplification.
struct TaskPacket {
    std::string objective;
    std::string scope;
    std::string repo;
    std::string branch_policy;
    std::vector<std::string> acceptance_tests;
    std::string commit_policy;
    std::string reporting_contract;
    std::string escalation_policy;

    bool operator==(const TaskPacket&) const = default;
};

/// Validation error listing all field-level issues.
struct TaskPacketValidationError {
    std::vector<std::string> errors;
    [[nodiscard]] std::string to_string() const;
};

/// Newtype wrapper for validated packets.
class ValidatedPacket {
public:
    explicit ValidatedPacket(TaskPacket packet) : packet_(std::move(packet)) {}
    [[nodiscard]] const TaskPacket& packet() const noexcept { return packet_; }
    [[nodiscard]] TaskPacket into_inner() && { return std::move(packet_); }

private:
    TaskPacket packet_;
};

/// Validate a packet; returns validated wrapper or list of error strings.
[[nodiscard]] tl::expected<ValidatedPacket, TaskPacketValidationError>
    validate_packet(const TaskPacket& packet);

// JSON serialization helpers (mirrors Rust serde Serialize/Deserialize)
void to_json(nlohmann::json& j, const TaskPacket& p);
void from_json(const nlohmann::json& j, TaskPacket& p);

} // namespace claw::runtime
