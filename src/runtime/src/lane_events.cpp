#include "lane_events.hpp"

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

} // namespace claw::runtime
