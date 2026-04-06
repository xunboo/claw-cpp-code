// branch_lock.cpp - C++20 faithful conversion of branch_lock.rs
#include "branch_lock.hpp"
#include <algorithm>
#include <tuple>

namespace claw::runtime {

static bool modules_overlap(const std::string& left, const std::string& right) {
    if (left == right) return true;
    if (left.starts_with(right + "/")) return true;
    if (right.starts_with(left + "/")) return true;
    return false;
}

static std::string shared_scope(const std::string& left, const std::string& right) {
    if (left.starts_with(right + "/") || left == right) {
        return right;
    } else {
        return left;
    }
}

static std::vector<std::string> overlapping_modules(
    const std::vector<std::string>& left,
    const std::vector<std::string>& right) {
    std::vector<std::string> overlaps;
    for (const auto& left_module : left) {
        for (const auto& right_module : right) {
            if (modules_overlap(left_module, right_module)) {
                overlaps.push_back(shared_scope(left_module, right_module));
            }
        }
    }
    std::sort(overlaps.begin(), overlaps.end());
    auto it = std::unique(overlaps.begin(), overlaps.end());
    overlaps.erase(it, overlaps.end());
    return overlaps;
}

std::vector<BranchLockCollision> detect_branch_lock_collisions(
    const std::vector<BranchLockIntent>& intents) {
    std::vector<BranchLockCollision> collisions;

    for (std::size_t i = 0; i < intents.size(); ++i) {
        const auto& left = intents[i];
        for (std::size_t j = i + 1; j < intents.size(); ++j) {
            const auto& right = intents[j];
            if (left.branch != right.branch) continue;

            auto overlaps = overlapping_modules(left.modules, right.modules);
            for (const auto& module_ : overlaps) {
                collisions.push_back(BranchLockCollision{
                    left.branch,
                    module_,
                    {left.lane_id, right.lane_id}
                });
            }
        }
    }

    std::sort(collisions.begin(), collisions.end(),
        [](const BranchLockCollision& a, const BranchLockCollision& b) {
            return std::tie(a.branch, a.module_, a.lane_ids) <
                   std::tie(b.branch, b.module_, b.lane_ids);
        });
    auto it = std::unique(collisions.begin(), collisions.end());
    collisions.erase(it, collisions.end());

    return collisions;
}

} // namespace claw::runtime
