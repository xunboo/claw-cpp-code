#pragma once

// telemetry.hpp — C++20 port of the Rust `telemetry` crate

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <chrono>
#include <format>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace claw::telemetry {

// ── Constants ────────────────────────────────────────────────────────────────
inline constexpr std::string_view DEFAULT_ANTHROPIC_VERSION         = "2023-06-01";
inline constexpr std::string_view DEFAULT_APP_NAME                  = "claude-code";
inline constexpr std::string_view DEFAULT_RUNTIME                   = "rust";
inline constexpr std::string_view DEFAULT_AGENTIC_BETA              = "claude-code-20250219";
inline constexpr std::string_view DEFAULT_PROMPT_CACHING_SCOPE_BETA = "prompt-caching-scope-2026-01-05";

#ifndef TELEMETRY_PKG_VERSION
#  define TELEMETRY_PKG_VERSION "0.0.0"
#endif

using JsonObject = nlohmann::json::object_t;
using JsonValue  = nlohmann::json;

// ── ClientIdentity ────────────────────────────────────────────────────────────
struct ClientIdentity {
    std::string app_name;
    std::string app_version;
    std::string runtime;

    [[nodiscard]] static ClientIdentity make(
        std::string app_name,
        std::string app_version,
        std::string runtime = std::string{DEFAULT_RUNTIME});

    [[nodiscard]] static ClientIdentity make_default();

    [[nodiscard]] ClientIdentity with_runtime(std::string r) &&;
    [[nodiscard]] ClientIdentity with_runtime(std::string r) const &;

    [[nodiscard]] std::string user_agent() const;
    [[nodiscard]] JsonValue   to_json()    const;
    [[nodiscard]] static ClientIdentity from_json(const JsonValue& j);

    bool operator==(const ClientIdentity&) const noexcept = default;
};

// ── AnthropicRequestProfile ──────────────────────────────────────────────────
struct AnthropicRequestProfile {
    std::string              anthropic_version;
    ClientIdentity           client_identity;
    std::vector<std::string> betas;
    JsonObject               extra_body;

    [[nodiscard]] static AnthropicRequestProfile make(ClientIdentity identity);
    [[nodiscard]] static AnthropicRequestProfile make_default();

    [[nodiscard]] AnthropicRequestProfile with_beta(std::string beta) &&;
    [[nodiscard]] AnthropicRequestProfile with_beta(std::string beta) const &;

    [[nodiscard]] AnthropicRequestProfile with_extra_body(std::string key, JsonValue value) &&;
    [[nodiscard]] AnthropicRequestProfile with_extra_body(std::string key, JsonValue value) const &;

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> header_pairs() const;

    // `request` must be a JSON object; throws std::invalid_argument otherwise.
    [[nodiscard]] JsonValue render_json_body(JsonValue request) const;

    bool operator==(const AnthropicRequestProfile&) const noexcept = default;
};

// ── AnalyticsEvent ────────────────────────────────────────────────────────────
struct AnalyticsEvent {
    std::string namespace_;   // "namespace" is reserved in C++
    std::string action;
    JsonObject  properties;

    [[nodiscard]] static AnalyticsEvent make(std::string ns, std::string action);

    [[nodiscard]] AnalyticsEvent with_property(std::string key, JsonValue value) &&;
    [[nodiscard]] AnalyticsEvent with_property(std::string key, JsonValue value) const &;

    [[nodiscard]] JsonValue         to_json() const;
    [[nodiscard]] static AnalyticsEvent from_json(const JsonValue& j);

    bool operator==(const AnalyticsEvent&) const noexcept = default;
};

// ── SessionTraceRecord ────────────────────────────────────────────────────────
struct SessionTraceRecord {
    std::string session_id;
    uint64_t    sequence{0};
    std::string name;
    uint64_t    timestamp_ms{0};
    JsonObject  attributes;

    [[nodiscard]] JsonValue            to_json() const;
    [[nodiscard]] static SessionTraceRecord from_json(const JsonValue& j);

    bool operator==(const SessionTraceRecord&) const noexcept = default;
};

// ── TelemetryEvent variant ────────────────────────────────────────────────────
struct HttpRequestStarted {
    std::string session_id;
    uint32_t    attempt{0};
    std::string method;
    std::string path;
    JsonObject  attributes;
    bool operator==(const HttpRequestStarted&) const noexcept = default;
};

struct HttpRequestSucceeded {
    std::string                session_id;
    uint32_t                   attempt{0};
    std::string                method;
    std::string                path;
    uint16_t                   status{0};
    std::optional<std::string> request_id;
    JsonObject                 attributes;
    bool operator==(const HttpRequestSucceeded&) const noexcept = default;
};

struct HttpRequestFailed {
    std::string session_id;
    uint32_t    attempt{0};
    std::string method;
    std::string path;
    std::string error;
    bool        retryable{false};
    JsonObject  attributes;
    bool operator==(const HttpRequestFailed&) const noexcept = default;
};

using TelemetryEvent = std::variant<
    HttpRequestStarted,
    HttpRequestSucceeded,
    HttpRequestFailed,
    AnalyticsEvent,
    SessionTraceRecord>;

[[nodiscard]] JsonValue      telemetry_event_to_json(const TelemetryEvent& ev);
[[nodiscard]] TelemetryEvent telemetry_event_from_json(const JsonValue& j);

// ── TelemetrySink ─────────────────────────────────────────────────────────────
class TelemetrySink {
public:
    virtual ~TelemetrySink() = default;
    virtual void record(TelemetryEvent event) = 0;
    TelemetrySink(const TelemetrySink&)            = delete;
    TelemetrySink& operator=(const TelemetrySink&) = delete;
protected:
    TelemetrySink() = default;
};

// ── MemoryTelemetrySink ───────────────────────────────────────────────────────
class MemoryTelemetrySink final : public TelemetrySink {
public:
    MemoryTelemetrySink() = default;
    void record(TelemetryEvent event) override;
    [[nodiscard]] std::vector<TelemetryEvent> events() const;
private:
    mutable std::mutex          mutex_;
    std::vector<TelemetryEvent> events_;
};

// ── JsonlTelemetrySink ────────────────────────────────────────────────────────
class JsonlTelemetrySink final : public TelemetrySink {
public:
    explicit JsonlTelemetrySink(const std::filesystem::path& path);
    void record(TelemetryEvent event) override;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
    mutable std::mutex    mutex_;
    std::ofstream         file_;
};

// ── SessionTracer ─────────────────────────────────────────────────────────────
class SessionTracer {
public:
    SessionTracer(std::string session_id, std::shared_ptr<TelemetrySink> sink);

    SessionTracer(const SessionTracer&)            = default;
    SessionTracer& operator=(const SessionTracer&) = default;
    SessionTracer(SessionTracer&&)                 = default;
    SessionTracer& operator=(SessionTracer&&)      = default;

    [[nodiscard]] const std::string& session_id() const noexcept { return session_id_; }

    void record(std::string name, JsonObject attributes = {});

    void record_http_request_started(
        uint32_t    attempt,
        std::string method,
        std::string path,
        JsonObject  attributes = {});

    void record_http_request_succeeded(
        uint32_t                   attempt,
        std::string                method,
        std::string                path,
        uint16_t                   status,
        std::optional<std::string> request_id,
        JsonObject                 attributes = {});

    void record_http_request_failed(
        uint32_t    attempt,
        std::string method,
        std::string path,
        std::string error,
        bool        retryable,
        JsonObject  attributes = {});

    void record_analytics(AnalyticsEvent event);

private:
    std::string                            session_id_;
    std::shared_ptr<std::atomic<uint64_t>> sequence_;
    std::shared_ptr<TelemetrySink>         sink_;
};

// ── Helpers ───────────────────────────────────────────────────────────────────
[[nodiscard]] uint64_t  current_timestamp_ms() noexcept;
[[nodiscard]] JsonObject merge_trace_fields(
    std::string method,
    std::string path,
    uint32_t    attempt,
    JsonObject  attributes = {});

}  // namespace claw::telemetry
