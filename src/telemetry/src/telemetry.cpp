// telemetry.cpp — Complete C++20 implementation of every fn in the Rust
// `telemetry` crate (rust/crates/telemetry/src/lib.rs).
//
// Conventions applied:
//   namespace       → telemetry
//   Result<T,E>     → tl::expected<T,E>  (render_json_body throws; header
//                     already declares it as JsonValue, so we stay consistent)
//   Option<T>       → std::optional<T>
//   serde_json      → nlohmann::json
//   async fn        → regular function

#include "telemetry.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <ranges>
#include <stdexcept>

namespace claw::telemetry {

// ── Helpers ───────────────────────────────────────────────────────────────────

// Rust: fn current_timestamp_ms() -> u64
// SystemTime::now().duration_since(UNIX_EPOCH).as_millis().try_into()
//   .unwrap_or(u64::MAX)
uint64_t current_timestamp_ms() noexcept {
    using namespace std::chrono;
    const auto now = system_clock::now().time_since_epoch();
    const auto ms  = duration_cast<milliseconds>(now).count();
    // duration_cast returns signed; clamp negatives to 0, overflow to max.
    if (ms < 0) return 0;
    return static_cast<uint64_t>(ms);
}

// Rust: fn merge_trace_fields(method, path, attempt, mut attributes) -> Map<..>
// Inserts method / path / attempt into the attributes map and returns it.
JsonObject merge_trace_fields(std::string method, std::string path,
                               uint32_t attempt, JsonObject attributes) {
    attributes["method"]  = std::move(method);
    attributes["path"]    = std::move(path);
    attributes["attempt"] = attempt;
    return attributes;
}

// ── ClientIdentity ────────────────────────────────────────────────────────────

// Rust: fn new(app_name, app_version) -> Self  (runtime defaults to DEFAULT_RUNTIME)
ClientIdentity ClientIdentity::make(std::string app_name, std::string app_version,
                                    std::string runtime) {
    return ClientIdentity{std::move(app_name), std::move(app_version), std::move(runtime)};
}

// Rust: fn default() -> Self  { Self::new(DEFAULT_APP_NAME, env!("CARGO_PKG_VERSION")) }
ClientIdentity ClientIdentity::make_default() {
    return make(std::string{DEFAULT_APP_NAME}, TELEMETRY_PKG_VERSION);
}

// Rust: fn with_runtime(mut self, runtime) -> Self
ClientIdentity ClientIdentity::with_runtime(std::string r) && {
    runtime = std::move(r);
    return std::move(*this);
}

ClientIdentity ClientIdentity::with_runtime(std::string r) const & {
    auto copy = *this;
    copy.runtime = std::move(r);
    return copy;
}

// Rust: fn user_agent(&self) -> String  { format!("{}/{}", app_name, app_version) }
std::string ClientIdentity::user_agent() const {
    return std::format("{}/{}", app_name, app_version);
}

// Serialisation helpers (mirror serde Serialize / Deserialize derives)
JsonValue ClientIdentity::to_json() const {
    return JsonValue{{"app_name", app_name}, {"app_version", app_version}, {"runtime", runtime}};
}

ClientIdentity ClientIdentity::from_json(const JsonValue& j) {
    return ClientIdentity{
        j.at("app_name").get<std::string>(),
        j.at("app_version").get<std::string>(),
        j.value("runtime", std::string{DEFAULT_RUNTIME}),
    };
}

// ── AnthropicRequestProfile ───────────────────────────────────────────────────

// Rust: fn new(client_identity) -> Self
// Sets anthropic_version, betas (the two defaults), empty extra_body.
AnthropicRequestProfile AnthropicRequestProfile::make(ClientIdentity identity) {
    AnthropicRequestProfile p;
    p.anthropic_version = std::string{DEFAULT_ANTHROPIC_VERSION};
    p.client_identity   = std::move(identity);
    p.betas             = {std::string{DEFAULT_AGENTIC_BETA},
                           std::string{DEFAULT_PROMPT_CACHING_SCOPE_BETA}};
    // extra_body is default-constructed as an empty object
    return p;
}

// Rust: fn default() -> Self  { Self::new(ClientIdentity::default()) }
AnthropicRequestProfile AnthropicRequestProfile::make_default() {
    return make(ClientIdentity::make_default());
}

// Rust: fn with_beta(mut self, beta) -> Self
// Deduplicates: only appends if not already present.
AnthropicRequestProfile AnthropicRequestProfile::with_beta(std::string beta) && {
    auto it = std::ranges::find(betas, beta);
    if (it == betas.end()) betas.push_back(std::move(beta));
    return std::move(*this);
}

AnthropicRequestProfile AnthropicRequestProfile::with_beta(std::string beta) const & {
    auto copy = *this;
    return std::move(copy).with_beta(std::move(beta));
}

// Rust: fn with_extra_body(mut self, key, value) -> Self
AnthropicRequestProfile AnthropicRequestProfile::with_extra_body(
    std::string key, JsonValue value) && {
    extra_body[std::move(key)] = std::move(value);
    return std::move(*this);
}

AnthropicRequestProfile AnthropicRequestProfile::with_extra_body(
    std::string key, JsonValue value) const & {
    auto copy = *this;
    return std::move(copy).with_extra_body(std::move(key), std::move(value));
}

// Rust: fn header_pairs(&self) -> Vec<(String, String)>
// Always emits: anthropic-version, user-agent.
// If betas non-empty: anthropic-beta (comma-joined).
std::vector<std::pair<std::string, std::string>>
AnthropicRequestProfile::header_pairs() const {
    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("anthropic-version", anthropic_version);
    headers.emplace_back("user-agent", client_identity.user_agent());
    if (!betas.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < betas.size(); ++i) {
            if (i) joined += ',';
            joined += betas[i];
        }
        headers.emplace_back("anthropic-beta", std::move(joined));
    }
    return headers;
}

// Rust: fn render_json_body<T: Serialize>(&self, request: &T)
//         -> Result<Value, serde_json::Error>
//
// 1. Serialise request to a JSON object (throw if not an object).
// 2. Merge every key in extra_body into the object.
// 3. If betas is non-empty, insert "betas" array.
//
// The header already declares this as returning JsonValue (throwing on error),
// which is the correct C++20 equivalent of the early-return Err path.
JsonValue AnthropicRequestProfile::render_json_body(JsonValue request) const {
    if (!request.is_object())
        throw std::invalid_argument(
            "render_json_body: request body must serialize to a JSON object");

    // Merge extra_body fields
    for (auto& [k, v] : extra_body)
        request[k] = v;

    // Insert betas array (same key name "betas" as the Rust impl)
    if (!betas.empty())
        request["betas"] = betas;   // nlohmann::json converts vector<string> directly

    return request;
}

// ── AnalyticsEvent ────────────────────────────────────────────────────────────

// Rust: fn new(namespace, action) -> Self  { ... properties: Map::new() }
AnalyticsEvent AnalyticsEvent::make(std::string ns, std::string action) {
    return AnalyticsEvent{std::move(ns), std::move(action), {}};
}

// Rust: fn with_property(mut self, key, value) -> Self
AnalyticsEvent AnalyticsEvent::with_property(std::string key, JsonValue value) && {
    properties[std::move(key)] = std::move(value);
    return std::move(*this);
}

AnalyticsEvent AnalyticsEvent::with_property(std::string key, JsonValue value) const & {
    auto copy = *this;
    return std::move(copy).with_property(std::move(key), std::move(value));
}

// serde Serialize / Deserialize mirrors
// The Rust serde attribute: #[serde(default, skip_serializing_if = "Map::is_empty")]
// means "properties" is omitted when empty.
JsonValue AnalyticsEvent::to_json() const {
    JsonValue j = {{"namespace", namespace_}, {"action", action}};
    if (!properties.empty()) j["properties"] = properties;
    return j;
}

AnalyticsEvent AnalyticsEvent::from_json(const JsonValue& j) {
    AnalyticsEvent ev;
    ev.namespace_ = j.at("namespace").get<std::string>();
    ev.action     = j.at("action").get<std::string>();
    if (j.contains("properties") && j["properties"].is_object())
        ev.properties = j["properties"].get<JsonObject>();
    return ev;
}

// ── SessionTraceRecord ────────────────────────────────────────────────────────

// serde Serialize / Deserialize mirrors
// "attributes" is #[serde(skip_serializing_if = "Map::is_empty")]
JsonValue SessionTraceRecord::to_json() const {
    JsonValue j = {
        {"session_id",   session_id},
        {"sequence",     sequence},
        {"name",         name},
        {"timestamp_ms", timestamp_ms},
    };
    if (!attributes.empty()) j["attributes"] = attributes;
    return j;
}

SessionTraceRecord SessionTraceRecord::from_json(const JsonValue& j) {
    SessionTraceRecord r;
    r.session_id   = j.at("session_id").get<std::string>();
    r.sequence     = j.at("sequence").get<uint64_t>();
    r.name         = j.at("name").get<std::string>();
    r.timestamp_ms = j.at("timestamp_ms").get<uint64_t>();
    if (j.contains("attributes") && j["attributes"].is_object())
        r.attributes = j["attributes"].get<JsonObject>();
    return r;
}

// ── TelemetryEvent JSON round-trip ────────────────────────────────────────────

// Rust: #[serde(tag = "type", rename_all = "snake_case")] on TelemetryEvent
// Produces / consumes a discriminated-union JSON object with a "type" field.
JsonValue telemetry_event_to_json(const TelemetryEvent& ev) {
    return std::visit([](const auto& e) -> JsonValue {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, HttpRequestStarted>) {
            JsonValue j = {
                {"type",       "http_request_started"},
                {"session_id", e.session_id},
                {"attempt",    e.attempt},
                {"method",     e.method},
                {"path",       e.path},
            };
            if (!e.attributes.empty()) j["attributes"] = e.attributes;
            return j;

        } else if constexpr (std::is_same_v<T, HttpRequestSucceeded>) {
            JsonValue j = {
                {"type",       "http_request_succeeded"},
                {"session_id", e.session_id},
                {"attempt",    e.attempt},
                {"method",     e.method},
                {"path",       e.path},
                {"status",     e.status},
            };
            // #[serde(skip_serializing_if = "Option::is_none")]
            if (e.request_id) j["request_id"] = *e.request_id;
            if (!e.attributes.empty()) j["attributes"] = e.attributes;
            return j;

        } else if constexpr (std::is_same_v<T, HttpRequestFailed>) {
            JsonValue j = {
                {"type",       "http_request_failed"},
                {"session_id", e.session_id},
                {"attempt",    e.attempt},
                {"method",     e.method},
                {"path",       e.path},
                {"error",      e.error},
                {"retryable",  e.retryable},
            };
            if (!e.attributes.empty()) j["attributes"] = e.attributes;
            return j;

        } else if constexpr (std::is_same_v<T, AnalyticsEvent>) {
            // Rust variant: Analytics(AnalyticsEvent) → type = "analytics"
            JsonValue j = e.to_json();
            j["type"] = "analytics";
            return j;

        } else if constexpr (std::is_same_v<T, SessionTraceRecord>) {
            // Rust variant: SessionTrace(SessionTraceRecord) → type = "session_trace"
            JsonValue j = e.to_json();
            j["type"] = "session_trace";
            return j;
        }
    }, ev);
}

TelemetryEvent telemetry_event_from_json(const JsonValue& j) {
    const auto& type = j.at("type").get_ref<const std::string&>();

    if (type == "http_request_started") {
        HttpRequestStarted e;
        e.session_id = j.at("session_id").get<std::string>();
        e.attempt    = j.at("attempt").get<uint32_t>();
        e.method     = j.at("method").get<std::string>();
        e.path       = j.at("path").get<std::string>();
        if (j.contains("attributes") && j["attributes"].is_object())
            e.attributes = j["attributes"].get<JsonObject>();
        return e;
    }

    if (type == "http_request_succeeded") {
        HttpRequestSucceeded e;
        e.session_id = j.at("session_id").get<std::string>();
        e.attempt    = j.at("attempt").get<uint32_t>();
        e.method     = j.at("method").get<std::string>();
        e.path       = j.at("path").get<std::string>();
        e.status     = j.at("status").get<uint16_t>();
        if (j.contains("request_id") && !j["request_id"].is_null())
            e.request_id = j["request_id"].get<std::string>();
        if (j.contains("attributes") && j["attributes"].is_object())
            e.attributes = j["attributes"].get<JsonObject>();
        return e;
    }

    if (type == "http_request_failed") {
        HttpRequestFailed e;
        e.session_id = j.at("session_id").get<std::string>();
        e.attempt    = j.at("attempt").get<uint32_t>();
        e.method     = j.at("method").get<std::string>();
        e.path       = j.at("path").get<std::string>();
        e.error      = j.at("error").get<std::string>();
        e.retryable  = j.at("retryable").get<bool>();
        if (j.contains("attributes") && j["attributes"].is_object())
            e.attributes = j["attributes"].get<JsonObject>();
        return e;
    }

    if (type == "analytics")     return AnalyticsEvent::from_json(j);
    if (type == "session_trace") return SessionTraceRecord::from_json(j);

    throw std::invalid_argument(std::format("telemetry_event_from_json: unknown type '{}'", type));
}

// ── MemoryTelemetrySink ───────────────────────────────────────────────────────

// Rust: impl TelemetrySink for MemoryTelemetrySink
//   fn record(&self, event: TelemetryEvent) {
//       self.events.lock().unwrap_or_else(...).push(event);
//   }
void MemoryTelemetrySink::record(TelemetryEvent event) {
    std::lock_guard lock{mutex_};
    events_.push_back(std::move(event));
}

// Rust: fn events(&self) -> Vec<TelemetryEvent> { ... .clone() }
std::vector<TelemetryEvent> MemoryTelemetrySink::events() const {
    std::lock_guard lock{mutex_};
    return events_;   // copy, same as Rust .clone()
}

// ── JsonlTelemetrySink ────────────────────────────────────────────────────────

// Rust: fn new(path) -> Result<Self, std::io::Error>
//   Creates parent directories, opens file in create+append mode.
// C++: throws std::runtime_error on failure (matching the Err path).
JsonlTelemetrySink::JsonlTelemetrySink(const std::filesystem::path& path)
    : path_{path} {
    // Rust: if let Some(parent) = path.parent() { std::fs::create_dir_all(parent)?; }
    if (path_.has_parent_path())
        std::filesystem::create_directories(path_.parent_path());

    // Rust: OpenOptions::new().create(true).append(true).open(&path)?
    file_.open(path_, std::ios::app | std::ios::out);
    if (!file_.is_open())
        throw std::runtime_error(
            std::format("JsonlTelemetrySink: cannot open '{}'", path_.string()));
}

// Rust: fn record(&self, event: TelemetryEvent)
//   Serialises to JSON, writes a line + newline, flushes.
//   Silently swallows serialisation errors (let Ok(line) = ... else { return; }).
void JsonlTelemetrySink::record(TelemetryEvent event) {
    std::string line;
    try {
        line = telemetry_event_to_json(event).dump();
    } catch (...) {
        // Mirror Rust's: let Ok(line) = serde_json::to_string(&event) else { return; }
        return;
    }
    std::lock_guard lock{mutex_};
    file_ << line << '\n';
    file_.flush();
}

// ── SessionTracer ─────────────────────────────────────────────────────────────

// Rust: fn new(session_id, sink) -> Self
// sequence is initialised to 0 (AtomicU64::new(0)).
SessionTracer::SessionTracer(std::string session_id, std::shared_ptr<TelemetrySink> sink)
    : session_id_{std::move(session_id)}
    , sequence_{std::make_shared<std::atomic<uint64_t>>(0)}
    , sink_{std::move(sink)}
{}

// Rust: pub fn record(&self, name, attributes: Map<String, Value>)
// Builds a SessionTraceRecord with the next sequence number, current timestamp,
// then forwards it to the sink as TelemetryEvent::SessionTrace(record).
void SessionTracer::record(std::string name, JsonObject attributes) {
    SessionTraceRecord rec;
    rec.session_id   = session_id_;
    rec.sequence     = sequence_->fetch_add(1, std::memory_order_relaxed);
    rec.name         = std::move(name);
    rec.timestamp_ms = current_timestamp_ms();
    rec.attributes   = std::move(attributes);
    // variant implicit conversion: SessionTraceRecord → TelemetryEvent
    sink_->record(std::move(rec));
}

// Rust: pub fn record_http_request_started(&self, attempt, method, path, attributes)
// 1. Emits HttpRequestStarted to the sink.
// 2. Emits a SessionTrace with merged trace fields.
void SessionTracer::record_http_request_started(
    uint32_t attempt, std::string method, std::string path, JsonObject attributes) {
    // Rust clones method/path/attributes before moving into the struct
    sink_->record(HttpRequestStarted{
        session_id_, attempt, method, path, attributes});
    record("http_request_started",
           merge_trace_fields(std::move(method), std::move(path), attempt,
                              std::move(attributes)));
}

// Rust: pub fn record_http_request_succeeded(&self, attempt, method, path,
//                                             status, request_id, attributes)
// 1. Emits HttpRequestSucceeded to the sink.
// 2. Emits a SessionTrace with merged trace fields + status [+ request_id].
void SessionTracer::record_http_request_succeeded(
    uint32_t attempt, std::string method, std::string path,
    uint16_t status, std::optional<std::string> request_id, JsonObject attributes) {
    sink_->record(HttpRequestSucceeded{
        session_id_, attempt, method, path, status, request_id, attributes});
    auto trace_attrs = merge_trace_fields(std::move(method), std::move(path),
                                          attempt, std::move(attributes));
    trace_attrs["status"] = status;
    if (request_id) trace_attrs["request_id"] = *request_id;
    record("http_request_succeeded", std::move(trace_attrs));
}

// Rust: pub fn record_http_request_failed(&self, attempt, method, path,
//                                          error, retryable, attributes)
// 1. Emits HttpRequestFailed to the sink.
// 2. Emits a SessionTrace with merged trace fields + error + retryable.
void SessionTracer::record_http_request_failed(
    uint32_t attempt, std::string method, std::string path,
    std::string error, bool retryable, JsonObject attributes) {
    sink_->record(HttpRequestFailed{
        session_id_, attempt, method, path, error, retryable, attributes});
    auto trace_attrs = merge_trace_fields(std::move(method), std::move(path),
                                          attempt, std::move(attributes));
    trace_attrs["error"]     = std::move(error);
    trace_attrs["retryable"] = retryable;
    record("http_request_failed", std::move(trace_attrs));
}

// Rust: pub fn record_analytics(&self, event: AnalyticsEvent)
// 1. Builds a flat attributes map with namespace + action + all properties.
// 2. Emits TelemetryEvent::Analytics(event) to the sink.
// 3. Emits a SessionTrace named "analytics" with those attributes.
void SessionTracer::record_analytics(AnalyticsEvent event) {
    // Build trace attributes first (before moving event fields)
    JsonObject attrs = event.properties;
    attrs["namespace"] = event.namespace_;
    attrs["action"]    = event.action;
    // variant implicit conversion: AnalyticsEvent → TelemetryEvent
    sink_->record(event);
    record("analytics", std::move(attrs));
}

}  // namespace claw::telemetry
