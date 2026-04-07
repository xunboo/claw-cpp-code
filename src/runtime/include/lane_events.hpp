#pragma once
// lane_events.hpp -- C++20 port of runtime/src/lane_events.rs
// Canonical lane lifecycle events, failure classes, and blocker types.

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace claw::runtime {

// ── LaneEventName ────────────────────────────────────────────────────────────
// Wire values match Rust serde rename attributes exactly.

enum class LaneEventName {
    Started,
    Ready,
    PromptMisdelivery,
    Blocked,
    Red,
    Green,
    CommitCreated,
    PrOpened,
    MergeReady,
    Finished,
    Failed,
    Reconciled,
    Merged,
    Superseded,
    Closed,
    BranchStaleAgainstMain,
};

[[nodiscard]] std::string_view lane_event_name_wire(LaneEventName e) noexcept;

// ── LaneEventStatus ──────────────────────────────────────────────────────────

enum class LaneEventStatus {
    Running,
    Ready,
    Blocked,
    Red,
    Green,
    Completed,
    Failed,
    Reconciled,
    Merged,
    Superseded,
    Closed,
};

[[nodiscard]] std::string_view lane_event_status_wire(LaneEventStatus s) noexcept;

// ── LaneFailureClass ─────────────────────────────────────────────────────────

enum class LaneFailureClass {
    PromptDelivery,
    TrustGate,
    BranchDivergence,
    Compile,
    Test,
    PluginStartup,
    McpStartup,
    McpHandshake,
    GatewayRouting,
    ToolRuntime,
    Infra,
};

[[nodiscard]] std::string_view lane_failure_class_wire(LaneFailureClass fc) noexcept;

// ── LaneEventBlocker ─────────────────────────────────────────────────────────

struct LaneEventBlocker {
    LaneFailureClass failure_class;
    std::string      detail;
};

void to_json(nlohmann::json& j, const LaneEventBlocker& b);

// ── LaneCommitProvenance ─────────────────────────────────────────────────────

struct LaneCommitProvenance {
    std::string                commit;
    std::string                branch;
    std::optional<std::string> worktree;
    std::optional<std::string> canonical_commit;
    std::optional<std::string> superseded_by;
    std::vector<std::string>   lineage;
};

void to_json(nlohmann::json& j, const LaneCommitProvenance& p);

// ── LaneEvent ────────────────────────────────────────────────────────────────

struct LaneEvent {
    LaneEventName                  event;
    LaneEventStatus                status;
    std::string                    emitted_at;
    std::optional<LaneFailureClass> failure_class;
    std::optional<std::string>     detail;
    std::optional<nlohmann::json>  data;

    // Named constructors (mirror Rust impl methods)
    [[nodiscard]] static LaneEvent make(LaneEventName event,
                                        LaneEventStatus status,
                                        std::string emitted_at);
    [[nodiscard]] static LaneEvent started(std::string emitted_at);
    [[nodiscard]] static LaneEvent finished(std::string emitted_at,
                                            std::optional<std::string> detail = std::nullopt);
    [[nodiscard]] static LaneEvent commit_created(std::string emitted_at,
                                                  std::optional<std::string> detail,
                                                  const LaneCommitProvenance& provenance);
    [[nodiscard]] static LaneEvent superseded(std::string emitted_at,
                                              std::optional<std::string> detail,
                                              const LaneCommitProvenance& provenance);
    [[nodiscard]] static LaneEvent blocked(std::string emitted_at,
                                           const LaneEventBlocker& blocker);
    [[nodiscard]] static LaneEvent failed(std::string emitted_at,
                                          const LaneEventBlocker& blocker);

    // Builder methods
    LaneEvent& with_failure_class(LaneFailureClass fc);
    LaneEvent& with_detail(std::string d);
    LaneEvent& with_optional_detail(std::optional<std::string> d);
    LaneEvent& with_data(nlohmann::json d);
};

void to_json(nlohmann::json& j, const LaneEvent& ev);

// ── Dedup helper ─────────────────────────────────────────────────────────────

/// Remove superseded commit events and keep only the latest per canonical commit.
/// Mirrors Rust dedupe_superseded_commit_events().
[[nodiscard]] std::vector<LaneEvent> dedupe_superseded_commit_events(
    const std::vector<LaneEvent>& events);

} // namespace claw::runtime
