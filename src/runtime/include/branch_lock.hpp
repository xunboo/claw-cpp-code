// branch_lock.hpp - C++20 faithful conversion of branch_lock.rs
#pragma once
#include <string>
#include <vector>
#include <optional>

namespace claw::runtime {

struct BranchLockIntent {
    std::string lane_id;
    std::string branch;
    std::optional<std::string> worktree;
    std::vector<std::string> modules;

    bool operator==(const BranchLockIntent& other) const = default;
};

struct BranchLockCollision {
    std::string branch;
    std::string module_;
    std::vector<std::string> lane_ids;

    bool operator==(const BranchLockCollision& other) const = default;
};

[[nodiscard]] std::vector<BranchLockCollision> detect_branch_lock_collisions(
    const std::vector<BranchLockIntent>& intents);

} // namespace claw::runtime
