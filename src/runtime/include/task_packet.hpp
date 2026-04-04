#pragma once
#include "green_contract.hpp"
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <map>
#include <tl/expected.hpp>
#include <nlohmann/json.hpp>

namespace claw::runtime {

struct RepoConfig {
    std::string repo_root;
    std::string worktree_root;
};

struct TaskScopeGlobal {};
struct TaskScopeFiles { std::vector<std::string> paths; };
struct TaskScopeDirectory { std::string directory; };

using TaskScope = std::variant<TaskScopeGlobal, TaskScopeFiles, TaskScopeDirectory>;

// Resolve relative paths in scope to absolute using dispatch_root
[[nodiscard]] TaskScope resolve_task_scope_paths(const TaskScope& scope, const std::string& dispatch_root);

// Branch policies
struct BranchPolicyCreateNew { std::string prefix; };
struct BranchPolicyUseExisting { std::string name; };
struct BranchPolicyWorktreeIsolated {};

using BranchPolicy = std::variant<BranchPolicyCreateNew, BranchPolicyUseExisting, BranchPolicyWorktreeIsolated>;

// Commit policies
struct CommitPolicyCommitPerTask {};
struct CommitPolicySquashOnMerge {};
struct CommitPolicyNoAutoCommit {};

using CommitPolicy = std::variant<CommitPolicyCommitPerTask, CommitPolicySquashOnMerge, CommitPolicyNoAutoCommit>;

// Acceptance tests
struct AccTestCargoTest { std::optional<std::string> filter; };
struct AccTestCustomCommand { std::string cmd; };
struct AccTestGreenLevel { GreenLevel level; };

using AcceptanceTest = std::variant<AccTestCargoTest, AccTestCustomCommand, AccTestGreenLevel>;

// Reporting
enum class ReportingContract { EventStream, Summary, Silent };

// Escalation
struct EscalationPolicyRetryThenEscalate { uint32_t max_retries{3}; };
struct EscalationPolicyAutoEscalate {};
struct EscalationPolicyNeverEscalate {};

using EscalationPolicy = std::variant<EscalationPolicyRetryThenEscalate, EscalationPolicyAutoEscalate, EscalationPolicyNeverEscalate>;

struct TaskPacket {
    std::string id;
    std::string objective;
    RepoConfig repo;
    TaskScope scope{TaskScopeGlobal{}};
    BranchPolicy branch_policy{BranchPolicyCreateNew{"task/"}};
    CommitPolicy commit_policy{CommitPolicyCommitPerTask{}};
    std::vector<AcceptanceTest> acceptance_tests;
    ReportingContract reporting{ReportingContract::Summary};
    EscalationPolicy escalation{EscalationPolicyRetryThenEscalate{3}};
    std::map<std::string, nlohmann::json> metadata;
};

// Newtype wrapper for validated packets
class ValidatedPacket {
public:
    explicit ValidatedPacket(TaskPacket packet) : packet_(std::move(packet)) {}
    [[nodiscard]] const TaskPacket& packet() const noexcept { return packet_; }

private:
    TaskPacket packet_;
};

// Validate a packet; returns list of error strings (empty = valid)
[[nodiscard]] tl::expected<ValidatedPacket, std::vector<std::string>>
    validate_packet(const TaskPacket& packet);

} // namespace claw::runtime
