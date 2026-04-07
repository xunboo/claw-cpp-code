#include "lane_events.hpp"
#include <map>

namespace claw::runtime {

// ── LaneEventName wire serialization ─────────────────────────────────────────

std::string_view lane_event_name_wire(LaneEventName e) noexcept {
    switch (e) {
        case LaneEventName::Started:               return "lane.started";
        case LaneEventName::Ready:                 return "lane.ready";
        case LaneEventName::PromptMisdelivery:     return "lane.prompt_misdelivery";
        case LaneEventName::Blocked:               return "lane.blocked";
        case LaneEventName::Red:                   return "lane.red";
        case LaneEventName::Green:                 return "lane.green";
        case LaneEventName::CommitCreated:         return "lane.commit.created";
        case LaneEventName::PrOpened:              return "lane.pr.opened";
        case LaneEventName::MergeReady:            return "lane.merge.ready";
        case LaneEventName::Finished:              return "lane.finished";
        case LaneEventName::Failed:                return "lane.failed";
        case LaneEventName::Reconciled:            return "lane.reconciled";
        case LaneEventName::Merged:                return "lane.merged";
        case LaneEventName::Superseded:            return "lane.superseded";
        case LaneEventName::Closed:                return "lane.closed";
        case LaneEventName::BranchStaleAgainstMain: return "branch.stale_against_main";
    }
    return "lane.started";
}

// ── LaneEventStatus wire serialization ───────────────────────────────────────

std::string_view lane_event_status_wire(LaneEventStatus s) noexcept {
    switch (s) {
        case LaneEventStatus::Running:    return "running";
        case LaneEventStatus::Ready:      return "ready";
        case LaneEventStatus::Blocked:    return "blocked";
        case LaneEventStatus::Red:        return "red";
        case LaneEventStatus::Green:      return "green";
        case LaneEventStatus::Completed:  return "completed";
        case LaneEventStatus::Failed:     return "failed";
        case LaneEventStatus::Reconciled: return "reconciled";
        case LaneEventStatus::Merged:     return "merged";
        case LaneEventStatus::Superseded: return "superseded";
        case LaneEventStatus::Closed:     return "closed";
    }
    return "running";
}

// ── LaneFailureClass wire serialization ──────────────────────────────────────

std::string_view lane_failure_class_wire(LaneFailureClass fc) noexcept {
    switch (fc) {
        case LaneFailureClass::PromptDelivery:   return "prompt_delivery";
        case LaneFailureClass::TrustGate:        return "trust_gate";
        case LaneFailureClass::BranchDivergence: return "branch_divergence";
        case LaneFailureClass::Compile:          return "compile";
        case LaneFailureClass::Test:             return "test";
        case LaneFailureClass::PluginStartup:    return "plugin_startup";
        case LaneFailureClass::McpStartup:       return "mcp_startup";
        case LaneFailureClass::McpHandshake:     return "mcp_handshake";
        case LaneFailureClass::GatewayRouting:   return "gateway_routing";
        case LaneFailureClass::ToolRuntime:      return "tool_runtime";
        case LaneFailureClass::Infra:            return "infra";
    }
    return "infra";
}

// ── LaneEventBlocker JSON ────────────────────────────────────────────────────

void to_json(nlohmann::json& j, const LaneEventBlocker& b) {
    j = nlohmann::json{
        {"failureClass", lane_failure_class_wire(b.failure_class)},
        {"detail",       b.detail}
    };
}

// ── LaneCommitProvenance JSON ────────────────────────────────────────────────

void to_json(nlohmann::json& j, const LaneCommitProvenance& p) {
    j = nlohmann::json{
        {"commit", p.commit},
        {"branch", p.branch}
    };
    if (p.worktree)
        j["worktree"] = *p.worktree;
    if (p.canonical_commit)
        j["canonicalCommit"] = *p.canonical_commit;
    if (p.superseded_by)
        j["supersededBy"] = *p.superseded_by;
    if (!p.lineage.empty())
        j["lineage"] = p.lineage;
}

// ── LaneEvent static constructors ────────────────────────────────────────────

LaneEvent LaneEvent::make(LaneEventName event,
                           LaneEventStatus status,
                           std::string emitted_at) {
    return LaneEvent{
        event, status, std::move(emitted_at),
        std::nullopt, std::nullopt, std::nullopt
    };
}

LaneEvent LaneEvent::started(std::string emitted_at) {
    return make(LaneEventName::Started, LaneEventStatus::Running, std::move(emitted_at));
}

LaneEvent LaneEvent::finished(std::string emitted_at,
                               std::optional<std::string> detail) {
    auto ev = make(LaneEventName::Finished, LaneEventStatus::Completed, std::move(emitted_at));
    ev.detail = std::move(detail);
    return ev;
}

LaneEvent LaneEvent::commit_created(std::string emitted_at,
                                     std::optional<std::string> detail,
                                     const LaneCommitProvenance& provenance) {
    auto ev = make(LaneEventName::CommitCreated, LaneEventStatus::Completed, std::move(emitted_at));
    ev.with_optional_detail(std::move(detail));
    nlohmann::json prov_json;
    to_json(prov_json, provenance);
    ev.with_data(std::move(prov_json));
    return ev;
}

LaneEvent LaneEvent::superseded(std::string emitted_at,
                                 std::optional<std::string> detail,
                                 const LaneCommitProvenance& provenance) {
    auto ev = make(LaneEventName::Superseded, LaneEventStatus::Superseded, std::move(emitted_at));
    ev.with_optional_detail(std::move(detail));
    nlohmann::json prov_json;
    to_json(prov_json, provenance);
    ev.with_data(std::move(prov_json));
    return ev;
}

LaneEvent LaneEvent::blocked(std::string emitted_at,
                              const LaneEventBlocker& blocker) {
    auto ev = make(LaneEventName::Blocked, LaneEventStatus::Blocked, std::move(emitted_at));
    ev.failure_class = blocker.failure_class;
    ev.detail = blocker.detail;
    return ev;
}

LaneEvent LaneEvent::failed(std::string emitted_at,
                             const LaneEventBlocker& blocker) {
    auto ev = make(LaneEventName::Failed, LaneEventStatus::Failed, std::move(emitted_at));
    ev.failure_class = blocker.failure_class;
    ev.detail = blocker.detail;
    return ev;
}

// ── Builder methods ──────────────────────────────────────────────────────────

LaneEvent& LaneEvent::with_failure_class(LaneFailureClass fc) {
    failure_class = fc;
    return *this;
}

LaneEvent& LaneEvent::with_detail(std::string d) {
    detail = std::move(d);
    return *this;
}

LaneEvent& LaneEvent::with_optional_detail(std::optional<std::string> d) {
    detail = std::move(d);
    return *this;
}

LaneEvent& LaneEvent::with_data(nlohmann::json d) {
    data = std::move(d);
    return *this;
}

// ── LaneEvent JSON ───────────────────────────────────────────────────────────

void to_json(nlohmann::json& j, const LaneEvent& ev) {
    j = nlohmann::json{
        {"event",     lane_event_name_wire(ev.event)},
        {"status",    lane_event_status_wire(ev.status)},
        {"emittedAt", ev.emitted_at}
    };
    if (ev.failure_class)
        j["failureClass"] = lane_failure_class_wire(*ev.failure_class);
    if (ev.detail)
        j["detail"] = *ev.detail;
    if (ev.data)
        j["data"] = *ev.data;
}

// ── Dedup helper ─────────────────────────────────────────────────────────────

std::vector<LaneEvent> dedupe_superseded_commit_events(
    const std::vector<LaneEvent>& events) {
    std::vector<bool> keep(events.size(), true);
    std::map<std::string, std::size_t> latest_by_key;

    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (event.event != LaneEventName::CommitCreated) {
            continue;
        }
        if (!event.data.has_value()) {
            continue;
        }
        const auto& data = *event.data;

        // Extract the key: canonicalCommit or commit
        std::string key;
        if (data.contains("canonicalCommit") && data["canonicalCommit"].is_string()) {
            key = data["canonicalCommit"].get<std::string>();
        } else if (data.contains("commit") && data["commit"].is_string()) {
            key = data["commit"].get<std::string>();
        }

        // Check if superseded
        bool superseded = data.contains("supersededBy")
                       && data["supersededBy"].is_string();
        if (superseded) {
            keep[index] = false;
            continue;
        }

        if (!key.empty()) {
            auto it = latest_by_key.find(key);
            if (it != latest_by_key.end()) {
                keep[it->second] = false;
                it->second = index;
            } else {
                latest_by_key.emplace(key, index);
            }
        }
    }

    std::vector<LaneEvent> result;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (keep[i]) {
            result.push_back(events[i]);
        }
    }
    return result;
}

} // namespace claw::runtime
