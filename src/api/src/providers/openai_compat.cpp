// ---------------------------------------------------------------------------
// providers/openai_compat.cpp  -  OpenAI-compatible provider implementation
// ---------------------------------------------------------------------------
//
// HTTP transport uses libcurl (synchronous).  The streaming path accumulates
// the full response body into a string and then drives OpenAiSseParser over
// it in OpenAiCompatMessageStream::next_event().
//
// Retry / back-off logic mirrors the Rust send_with_retry / backoff_for_attempt
// exactly, including the BackoffOverflow sentinel for 1u << attempt overflow.
// ---------------------------------------------------------------------------

#include "providers/openai_compat.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#if __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define HAVE_CURL 1
#else
#define HAVE_CURL 0
#endif
#include <nlohmann/json.hpp>

#if !HAVE_CURL
namespace claw::api {} // stub
#else

namespace claw::api {

// ── Internal HTTP result ───────────────────────────────────────────────────────

struct HttpResult {
    long        status{0};
    std::string body;
    std::string request_id;   // value of request-id or x-request-id header
};

// ── libcurl write / header callbacks ─────────────────────────────────────────

static size_t curl_write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t curl_write_header(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* result = static_cast<HttpResult*>(userdata);
    std::string_view line(ptr, size * nmemb);
    // Strip trailing CRLF
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.remove_suffix(1);

    auto lower_starts = [&](std::string_view prefix) -> bool {
        if (line.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(line[i])) !=
                static_cast<unsigned char>(prefix[i]))
                return false;
        return true;
    };

    auto extract_value = [&](size_t colon_pos) -> std::string {
        auto v = line.substr(colon_pos + 1);
        while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
        return std::string(v);
    };

    // request-id header (takes priority over x-request-id)
    if (lower_starts("request-id:")) {
        result->request_id = extract_value(10);
    } else if (lower_starts("x-request-id:") && result->request_id.empty()) {
        result->request_id = extract_value(12);
    }

    return size * nmemb;
}

// ── Perform a single synchronous HTTP POST via libcurl ────────────────────────

static HttpResult curl_post(const std::string& url,
                             const std::string& bearer_token,
                             const std::string& json_body) {
    CURL* curl = curl_easy_init();
    if (!curl)
        throw std::runtime_error("curl_easy_init() failed");

    HttpResult result;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer_token).c_str());
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode rc = curl_easy_perform(curl);

    if (rc != CURLE_OK) {
        std::string detail = curl_easy_strerror(rc);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        bool is_connect = (rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST);
        bool is_timeout = (rc == CURLE_OPERATION_TIMEDOUT);
        throw std::runtime_error("curl: " + detail); // caller converts to ApiError
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

// ── Retryable HTTP status codes (mirrors Rust is_retryable_status) ────────────

static bool is_retryable_status(long status) {
    switch (status) {
        case 408: case 409: case 429:
        case 500: case 502: case 503: case 504:
            return true;
        default:
            return false;
    }
}

// ── Parse error envelope and turn a non-2xx response into ApiError ────────────

static ApiError api_error_from_response(long status, const std::string& body) {
    std::string error_type;
    std::string message;
    try {
        auto j = nlohmann::json::parse(body);
        if (j.contains("error") && j["error"].is_object()) {
            auto& e = j["error"];
            if (e.contains("type") && !e["type"].is_null())
                error_type = e["type"].get<std::string>();
            if (e.contains("message") && !e["message"].is_null())
                message = e["message"].get<std::string>();
        }
    } catch (...) {}
    return ApiError::api(static_cast<int>(status), std::move(error_type),
                         std::move(message), body, is_retryable_status(status));
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::optional<std::string> read_env_compat(const char* key) {
    const char* v = std::getenv(key);
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

static std::string normalize_finish_reason_impl(std::string_view v) {
    if (v == "stop")       return "end_turn";
    if (v == "tool_calls") return "tool_use";
    return std::string(v);
}

static std::string chat_completions_endpoint_impl(std::string_view base) {
    std::string s{base};
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.size() >= 17 && s.substr(s.size() - 17) == "/chat/completions") return s;
    return s + "/chat/completions";
}

static std::string flatten_tool_result(const std::vector<ToolResultContentBlock>& blocks) {
    std::string out;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out += '\n';
        if (blocks[i].kind == ToolResultContentBlock::Kind::Text)
            out += blocks[i].text;
        else
            out += blocks[i].value.dump();
    }
    return out;
}

static nlohmann::json translate_message_impl(const InputMessage& msg) {
    nlohmann::json result = nlohmann::json::array();
    if (msg.role == "assistant") {
        std::string text;
        nlohmann::json tool_calls = nlohmann::json::array();
        for (auto& b : msg.content) {
            if (b.kind == InputContentBlock::Kind::Text)
                text += b.text;
            else if (b.kind == InputContentBlock::Kind::ToolUse)
                tool_calls.push_back({{"id",   b.id},
                                      {"type", "function"},
                                      {"function", {{"name", b.name},
                                                    {"arguments", b.input.dump()}}}});
        }
        if (!text.empty() || !tool_calls.empty()) {
            nlohmann::json m = {{"role", "assistant"}};
            if (!text.empty()) m["content"] = text;
            m["tool_calls"] = tool_calls;
            result.push_back(m);
        }
    } else {
        for (auto& b : msg.content) {
            if (b.kind == InputContentBlock::Kind::Text)
                result.push_back({{"role", "user"}, {"content", b.text}});
            else if (b.kind == InputContentBlock::Kind::ToolResult)
                result.push_back({{"role",         "tool"},
                                   {"tool_call_id", b.tool_use_id},
                                   {"content",      flatten_tool_result(b.content)},
                                   {"is_error",     b.is_error}});
        }
    }
    return result;
}

nlohmann::json build_chat_completion_request(const MessageRequest& req,
                                              OpenAiCompatConfig cfg) {
    nlohmann::json messages = nlohmann::json::array();
    if (req.system && !req.system->empty())
        messages.push_back({{"role", "system"}, {"content", *req.system}});
    for (auto& m : req.messages)
        for (auto& item : translate_message_impl(m))
            messages.push_back(item);

    nlohmann::json payload = {{"model",      req.model},
                               {"max_tokens", req.max_tokens},
                               {"messages",   messages},
                               {"stream",     req.stream}};

    // OpenAI-specific streaming usage opt-in
    if (req.stream && std::string_view(cfg.provider_name) == "OpenAI")
        payload["stream_options"] = {{"include_usage", true}};

    if (req.tools) {
        nlohmann::json tools = nlohmann::json::array();
        for (auto& t : *req.tools)
            tools.push_back(
                {{"type", "function"},
                 {"function", {{"name",        t.name},
                               {"description", t.description
                                               ? nlohmann::json(*t.description)
                                               : nlohmann::json(nullptr)},
                               {"parameters",  t.input_schema}}}});
        payload["tools"] = tools;
    }
    if (req.tool_choice) {
        auto& tc = *req.tool_choice;
        if (tc.kind == ToolChoice::Kind::Auto)
            payload["tool_choice"] = "auto";
        else if (tc.kind == ToolChoice::Kind::Any)
            payload["tool_choice"] = "required";
        else
            payload["tool_choice"] = {{"type", "function"},
                                       {"function", {{"name", tc.name}}}};
    }
    return payload;
}

// ── OpenAiCompatConfig ────────────────────────────────────────────────────────

std::vector<std::string_view> OpenAiCompatConfig::credential_env_vars() const noexcept {
    if (provider_name == "xAI")    return {"XAI_API_KEY"};
    if (provider_name == "OpenAI") return {"OPENAI_API_KEY"};
    return {};
}

// ── OpenAiSseParser ───────────────────────────────────────────────────────────

static std::optional<std::string> next_sse_frame_compat(std::string& buf) {
    size_t best_pos  = std::string::npos;
    size_t best_sl   = 0;

    auto try_sep = [&](std::string_view sep) {
        auto p = buf.find(sep);
        if (p == std::string::npos) return;
        if (best_pos == std::string::npos || p < best_pos) {
            best_pos = p;
            best_sl  = sep.size();
        }
    };
    try_sep("\r\n\r\n");
    try_sep("\n\n");

    if (best_pos == std::string::npos) return std::nullopt;
    std::string frame = buf.substr(0, best_pos);
    buf.erase(0, best_pos + best_sl);
    return frame;
}

static std::optional<ChatCompletionChunk> parse_chunk_frame(std::string_view frame) {
    // Trim
    while (!frame.empty() && (frame.front() == ' ' || frame.front() == '\r' ||
                               frame.front() == '\n'))
        frame.remove_prefix(1);
    while (!frame.empty() && (frame.back()  == ' ' || frame.back()  == '\r' ||
                               frame.back()  == '\n'))
        frame.remove_suffix(1);
    if (frame.empty()) return std::nullopt;

    std::vector<std::string_view> data_lines;
    size_t start = 0;
    while (start <= frame.size()) {
        size_t nl = frame.find('\n', start);
        auto line = (nl == std::string_view::npos) ? frame.substr(start)
                                                    : frame.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        start = (nl == std::string_view::npos) ? frame.size() + 1 : nl + 1;

        if (line.empty() || line.front() == ':') continue;
        if (line.substr(0, 5) == "data:") {
            auto d = line.substr(5);
            if (!d.empty() && d.front() == ' ') d.remove_prefix(1);
            data_lines.push_back(d);
        }
    }
    if (data_lines.empty()) return std::nullopt;

    std::string payload;
    for (size_t i = 0; i < data_lines.size(); ++i) {
        if (i) payload += '\n';
        payload += data_lines[i];
    }
    if (payload == "[DONE]") return std::nullopt;

    try {
        auto j = nlohmann::json::parse(payload);
        ChatCompletionChunk chunk;
        chunk.id = j.value("id", std::string{});
        if (j.contains("model") && !j["model"].is_null())
            chunk.model = j["model"].get<std::string>();
        if (j.contains("usage") && !j["usage"].is_null()) {
            OpenAiUsage u;
            u.prompt_tokens     = j["usage"].value("prompt_tokens",     0u);
            u.completion_tokens = j["usage"].value("completion_tokens", 0u);
            chunk.usage = u;
        }
        if (j.contains("choices")) {
            for (auto& cj : j["choices"]) {
                ChunkChoice ch;
                if (cj.contains("finish_reason") && !cj["finish_reason"].is_null())
                    ch.finish_reason = cj["finish_reason"].get<std::string>();
                auto& dj = cj.at("delta");
                if (dj.contains("content") && !dj["content"].is_null())
                    ch.delta.content = dj["content"].get<std::string>();
                if (dj.contains("tool_calls")) {
                    for (auto& tj : dj["tool_calls"]) {
                        DeltaToolCall dtc;
                        dtc.index = tj.value("index", 0u);
                        if (tj.contains("id") && !tj["id"].is_null())
                            dtc.id = tj["id"].get<std::string>();
                        if (tj.contains("function")) {
                            auto& fj = tj["function"];
                            if (fj.contains("name") && !fj["name"].is_null())
                                dtc.function.name = fj["name"].get<std::string>();
                            if (fj.contains("arguments") && !fj["arguments"].is_null())
                                dtc.function.arguments = fj["arguments"].get<std::string>();
                        }
                        ch.delta.tool_calls.push_back(std::move(dtc));
                    }
                }
                chunk.choices.push_back(std::move(ch));
            }
        }
        return chunk;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<ChatCompletionChunk> OpenAiSseParser::push(std::string_view chunk) {
    buffer_.append(chunk);
    std::vector<ChatCompletionChunk> out;
    while (auto frame = next_sse_frame_compat(buffer_))
        if (auto c = parse_chunk_frame(*frame); c)
            out.push_back(std::move(*c));
    return out;
}

// ── ToolCallState ─────────────────────────────────────────────────────────────

void ToolCallState::apply(const DeltaToolCall& tc) {
    openai_index = tc.index;
    if (tc.id)                 id   = tc.id;
    if (tc.function.name)      name = tc.function.name;
    if (tc.function.arguments) arguments += *tc.function.arguments;
}

std::optional<ContentBlockStartEvent> ToolCallState::start_event() const {
    if (!name) return std::nullopt;
    std::string eid = id ? *id : ("tool_call_" + std::to_string(openai_index));
    OutputContentBlock block;
    block.kind  = OutputContentBlock::Kind::ToolUse;
    block.id    = std::move(eid);
    block.name  = *name;
    block.input = nlohmann::json::object();
    ContentBlockStartEvent e;
    e.index         = block_index();
    e.content_block = std::move(block);
    return e;
}

std::optional<ContentBlockDeltaEvent> ToolCallState::delta_event() {
    if (emitted_len >= arguments.size()) return std::nullopt;
    std::string delta = arguments.substr(emitted_len);
    emitted_len = arguments.size();
    ContentBlockDelta d;
    d.kind         = ContentBlockDelta::Kind::InputJsonDelta;
    d.partial_json = std::move(delta);
    ContentBlockDeltaEvent e;
    e.index = block_index();
    e.delta = std::move(d);
    return e;
}

// ── StreamState ───────────────────────────────────────────────────────────────

StreamState::StreamState(std::string model) : model_(std::move(model)) {}

std::vector<StreamEvent> StreamState::ingest_chunk(const ChatCompletionChunk& chunk) {
    std::vector<StreamEvent> events;

    if (!message_started_) {
        message_started_ = true;
        MessageResponse mr;
        mr.id    = chunk.id;
        mr.kind  = "message";
        mr.role  = "assistant";
        mr.model = chunk.model.value_or(model_);
        mr.usage = Usage{};
        StreamEvent e;
        e.kind                    = StreamEvent::Kind::MessageStart;
        e.message_start.message   = std::move(mr);
        events.push_back(std::move(e));
    }

    if (chunk.usage) {
        usage_ = Usage{};
        usage_->input_tokens  = chunk.usage->prompt_tokens;
        usage_->output_tokens = chunk.usage->completion_tokens;
    }

    for (auto& choice : chunk.choices) {
        // ── text content delta ───────────────────────────────────────────────
        if (choice.delta.content && !choice.delta.content->empty()) {
            if (!text_started_) {
                text_started_ = true;
                OutputContentBlock tb;
                tb.kind = OutputContentBlock::Kind::Text;
                StreamEvent e;
                e.kind                             = StreamEvent::Kind::ContentBlockStart;
                e.content_block_start.index        = 0;
                e.content_block_start.content_block = std::move(tb);
                events.push_back(std::move(e));
            }
            ContentBlockDelta d;
            d.kind = ContentBlockDelta::Kind::TextDelta;
            d.text = *choice.delta.content;
            StreamEvent e;
            e.kind                       = StreamEvent::Kind::ContentBlockDelta;
            e.content_block_delta.index  = 0;
            e.content_block_delta.delta  = std::move(d);
            events.push_back(std::move(e));
        }

        // ── tool call deltas ─────────────────────────────────────────────────
        for (auto& dtc : choice.delta.tool_calls) {
            auto& state = tool_calls_[dtc.index];
            state.apply(dtc);
            if (!state.started) {
                if (auto se = state.start_event()) {
                    state.started = true;
                    StreamEvent e;
                    e.kind                = StreamEvent::Kind::ContentBlockStart;
                    e.content_block_start = *se;
                    events.push_back(std::move(e));
                } else {
                    continue;
                }
            }
            if (auto de = state.delta_event()) {
                StreamEvent e;
                e.kind                = StreamEvent::Kind::ContentBlockDelta;
                e.content_block_delta = *de;
                events.push_back(std::move(e));
            }
            if (choice.finish_reason.value_or("") == "tool_calls" && !state.stopped) {
                state.stopped = true;
                StreamEvent e;
                e.kind                     = StreamEvent::Kind::ContentBlockStop;
                e.content_block_stop.index = state.block_index();
                events.push_back(std::move(e));
            }
        }

        // ── finish reason ────────────────────────────────────────────────────
        if (choice.finish_reason) {
            stop_reason_ = normalize_finish_reason_impl(*choice.finish_reason);
            if (*choice.finish_reason == "tool_calls") {
                for (auto& [idx, st] : tool_calls_) {
                    if (st.started && !st.stopped) {
                        st.stopped = true;
                        StreamEvent e;
                        e.kind                     = StreamEvent::Kind::ContentBlockStop;
                        e.content_block_stop.index = st.block_index();
                        events.push_back(std::move(e));
                    }
                }
            }
        }
    }
    return events;
}

std::vector<StreamEvent> StreamState::finish() {
    if (finished_) return {};
    finished_ = true;

    std::vector<StreamEvent> events;

    // Close text block
    if (text_started_ && !text_finished_) {
        text_finished_ = true;
        StreamEvent e;
        e.kind                     = StreamEvent::Kind::ContentBlockStop;
        e.content_block_stop.index = 0;
        events.push_back(std::move(e));
    }

    // Flush any tool call states that never emitted start/stop
    for (auto& [idx, st] : tool_calls_) {
        if (!st.started) {
            if (auto se = st.start_event()) {
                st.started = true;
                StreamEvent e;
                e.kind                = StreamEvent::Kind::ContentBlockStart;
                e.content_block_start = *se;
                events.push_back(std::move(e));
                if (auto de = st.delta_event()) {
                    StreamEvent e2;
                    e2.kind                = StreamEvent::Kind::ContentBlockDelta;
                    e2.content_block_delta = *de;
                    events.push_back(std::move(e2));
                }
            }
        }
        if (st.started && !st.stopped) {
            st.stopped = true;
            StreamEvent e;
            e.kind                     = StreamEvent::Kind::ContentBlockStop;
            e.content_block_stop.index = st.block_index();
            events.push_back(std::move(e));
        }
    }

    // MessageDelta + MessageStop
    if (message_started_) {
        MessageDelta md;
        md.stop_reason = stop_reason_.value_or("end_turn");
        Usage u = usage_.value_or(Usage{});
        StreamEvent e1;
        e1.kind                   = StreamEvent::Kind::MessageDelta;
        e1.message_delta.delta    = md;
        e1.message_delta.usage    = u;
        events.push_back(std::move(e1));

        StreamEvent e2;
        e2.kind = StreamEvent::Kind::MessageStop;
        events.push_back(std::move(e2));
    }
    return events;
}

// ── OpenAiCompatMessageStream ─────────────────────────────────────────────────

OpenAiCompatMessageStream::OpenAiCompatMessageStream(
    std::optional<std::string> rid, std::string raw, StreamState state)
    : request_id_(std::move(rid)),
      raw_body_(std::move(raw)),
      state_(std::move(state)) {}

std::optional<std::string_view> OpenAiCompatMessageStream::request_id() const noexcept {
    return request_id_ ? std::optional<std::string_view>(*request_id_) : std::nullopt;
}

std::optional<StreamEvent> OpenAiCompatMessageStream::next_event() {
    for (;;) {
        if (!pending_.empty()) {
            auto e = std::move(pending_.front());
            pending_.pop_front();
            return e;
        }
        if (done_) {
            for (auto& e : state_.finish())
                pending_.push_back(std::move(e));
            if (!pending_.empty()) continue;
            return std::nullopt;
        }
        // Feed the next 4 KiB slice of the raw body into the SSE parser
        constexpr size_t CHUNK = 4096;
        size_t chunk_sz = std::min(CHUNK, raw_body_.size() - body_pos_);
        if (chunk_sz == 0) {
            done_ = true;
            continue;
        }
        auto chunks = parser_.push(
            std::string_view(raw_body_.data() + body_pos_, chunk_sz));
        body_pos_ += chunk_sz;
        for (auto& c : chunks)
            for (auto& e : state_.ingest_chunk(c))
                pending_.push_back(std::move(e));
    }
}

// ── OpenAiCompatClient ────────────────────────────────────────────────────────

OpenAiCompatClient::OpenAiCompatClient(std::string api_key, OpenAiCompatConfig cfg)
    : api_key_(std::move(api_key)),
      config_(cfg),
      base_url_(read_base_url(cfg)) {}

OpenAiCompatClient OpenAiCompatClient::from_env(OpenAiCompatConfig cfg) {
    auto key = read_env_compat(std::string(cfg.api_key_env).c_str());
    if (!key) {
        throw ApiError::missing_credentials(cfg.provider_name, cfg.credential_env_vars());
    }
    return OpenAiCompatClient(std::move(*key), cfg);
}

OpenAiCompatClient OpenAiCompatClient::with_base_url(std::string url) && {
    base_url_ = std::move(url);
    return std::move(*this);
}

OpenAiCompatClient OpenAiCompatClient::with_retry_policy(
    uint32_t mr,
    std::chrono::milliseconds ib,
    std::chrono::milliseconds mb) && {
    max_retries_     = mr;
    initial_backoff_ = ib;
    max_backoff_     = mb;
    return std::move(*this);
}

std::optional<std::string_view>
OpenAiCompatClient::request_id_from_last_response() const noexcept {
    if (last_request_id_.empty()) return std::nullopt;
    return std::string_view(last_request_id_);
}

// ── backoff_for_attempt ───────────────────────────────────────────────────────
// Mirrors Rust:
//   let Some(multiplier) = 1_u32.checked_shl(attempt.saturating_sub(1)) else {
//       return Err(ApiError::BackoffOverflow {...});
//   };
//   Ok(initial_backoff.checked_mul(multiplier).map_or(max_backoff, |d| d.min(max_backoff)))

std::chrono::milliseconds OpenAiCompatClient::backoff_for_attempt(uint32_t attempt) const {
    uint32_t shift = (attempt > 0) ? attempt - 1 : 0;
    // checked_shl: overflow if shift >= 32
    if (shift >= 32) {
        throw ApiError::backoff_overflow(attempt, initial_backoff_);
    }
    uint32_t multiplier = 1u << shift;
    // checked_mul: if multiplier * initial_backoff_ms would overflow, cap at max
    uint64_t ms = static_cast<uint64_t>(initial_backoff_.count())
                * static_cast<uint64_t>(multiplier);
    if (ms > static_cast<uint64_t>(max_backoff_.count()))
        return max_backoff_;
    auto delay = std::chrono::milliseconds(static_cast<long long>(ms));
    return delay < max_backoff_ ? delay : max_backoff_;
}

// ── send_raw_request ──────────────────────────────────────────────────────────

static HttpResult do_curl_post(const std::string& url,
                                const std::string& token,
                                const nlohmann::json& body_json) {
    return curl_post(url, token, body_json.dump());
}

// ── normalize_response (non-streaming) ────────────────────────────────────────

static nlohmann::json parse_tool_arguments_impl(const std::string& arguments) {
    try {
        return nlohmann::json::parse(arguments);
    } catch (...) {
        return {{"raw", arguments}};
    }
}

static MessageResponse normalize_response(const std::string& model,
                                          const nlohmann::json& j) {
    // Must have at least one choice
    if (!j.contains("choices") || j["choices"].empty())
        throw ApiError::invalid_sse_frame("chat completion response missing choices");

    auto& choice_j = j["choices"][0];
    auto& msg_j    = choice_j.at("message");

    std::vector<OutputContentBlock> content;

    // Text content
    if (msg_j.contains("content") && !msg_j["content"].is_null()) {
        std::string text = msg_j["content"].get<std::string>();
        if (!text.empty()) {
            OutputContentBlock b;
            b.kind = OutputContentBlock::Kind::Text;
            b.text = std::move(text);
            content.push_back(std::move(b));
        }
    }

    // Tool calls
    if (msg_j.contains("tool_calls") && msg_j["tool_calls"].is_array()) {
        for (auto& tc_j : msg_j["tool_calls"]) {
            OutputContentBlock b;
            b.kind  = OutputContentBlock::Kind::ToolUse;
            b.id    = tc_j.at("id").get<std::string>();
            b.name  = tc_j.at("function").at("name").get<std::string>();
            b.input = parse_tool_arguments_impl(
                tc_j.at("function").at("arguments").get<std::string>());
            content.push_back(std::move(b));
        }
    }

    std::string response_model = j.value("model", model);
    if (response_model.empty()) response_model = model;

    std::optional<std::string> stop_reason;
    if (choice_j.contains("finish_reason") && !choice_j["finish_reason"].is_null())
        stop_reason = normalize_finish_reason_impl(
            choice_j["finish_reason"].get<std::string>());

    Usage usage{};
    if (j.contains("usage") && j["usage"].is_object()) {
        usage.input_tokens  = j["usage"].value("prompt_tokens",     0u);
        usage.output_tokens = j["usage"].value("completion_tokens", 0u);
    }

    MessageResponse resp;
    resp.id          = j.value("id", std::string{});
    resp.kind        = "message";
    resp.role        = msg_j.value("role", std::string{"assistant"});
    resp.content     = std::move(content);
    resp.model       = std::move(response_model);
    resp.stop_reason = std::move(stop_reason);
    resp.usage       = usage;
    return resp;
}

// ── send_with_retry ───────────────────────────────────────────────────────────
// Mirrors Rust send_with_retry: attempts starts at 1; we retry up to
// max_retries_ additional times (total max_retries_ + 1 attempts).

MessageResponse OpenAiCompatClient::send_message(const MessageRequest& req) {
    // Force stream=false
    MessageRequest nr = req;
    nr.stream = false;
    auto url  = chat_completions_endpoint_impl(base_url_);
    auto body = build_chat_completion_request(nr, config_);

    uint32_t attempts = 0;
    std::optional<ApiError> last_error;

    for (;;) {
        ++attempts;

        // ── single attempt ───────────────────────────────────────────────────
        std::optional<ApiError> attempt_error;
        try {
            HttpResult hr = do_curl_post(url, api_key_, body);
            if (hr.status >= 200 && hr.status < 300) {
                last_request_id_ = hr.request_id;
                auto j   = nlohmann::json::parse(hr.body);
                auto resp = normalize_response(nr.model, j);
                if (resp.request_id.value_or("").empty() && !hr.request_id.empty())
                    resp.request_id = hr.request_id;
                return resp;
            }
            attempt_error = api_error_from_response(hr.status, hr.body);
        } catch (const ApiError& ae) {
            attempt_error = ae;
        } catch (const std::exception& ex) {
            attempt_error = ApiError::http(ex.what(), false, false);
        }

        // ── decide whether to retry ──────────────────────────────────────────
        if (!attempt_error->is_retryable() || attempts > max_retries_ + 1) {
            throw std::move(*attempt_error);
        }
        last_error = std::move(attempt_error);

        if (attempts > max_retries_)
            break;

        std::this_thread::sleep_for(backoff_for_attempt(attempts));
    }

    auto last = std::move(last_error).value_or(ApiError::http("unknown error"));
    throw ApiError::retries_exhausted(
        attempts, std::make_unique<ApiError>(std::move(last)));
}

OpenAiCompatMessageStream OpenAiCompatClient::stream_message(const MessageRequest& req) {
    MessageRequest sr = req;
    sr.stream = true;
    auto url  = chat_completions_endpoint_impl(base_url_);
    auto body = build_chat_completion_request(sr, config_);

    uint32_t attempts = 0;
    std::optional<ApiError> last_error;

    for (;;) {
        ++attempts;

        std::optional<ApiError> attempt_error;
        try {
            HttpResult hr = do_curl_post(url, api_key_, body);
            if (hr.status >= 200 && hr.status < 300) {
                last_request_id_ = hr.request_id;
                std::optional<std::string> rid;
                if (!hr.request_id.empty()) rid = hr.request_id;
                return OpenAiCompatMessageStream(
                    std::move(rid),
                    std::move(hr.body),
                    StreamState(sr.model));
            }
            attempt_error = api_error_from_response(hr.status, hr.body);
        } catch (const ApiError& ae) {
            attempt_error = ae;
        } catch (const std::exception& ex) {
            attempt_error = ApiError::http(ex.what(), false, false);
        }

        if (!attempt_error->is_retryable() || attempts > max_retries_ + 1) {
            throw std::move(*attempt_error);
        }
        last_error = std::move(attempt_error);

        if (attempts > max_retries_)
            break;

        std::this_thread::sleep_for(backoff_for_attempt(attempts));
    }

    auto last = std::move(last_error).value_or(ApiError::http("unknown error"));
    throw ApiError::retries_exhausted(
        attempts, std::make_unique<ApiError>(std::move(last)));
}

// ── Static utilities ──────────────────────────────────────────────────────────

bool OpenAiCompatClient::has_api_key(std::string_view key) {
    return read_env_compat(std::string(key).c_str()).has_value();
}

std::string OpenAiCompatClient::read_base_url(OpenAiCompatConfig cfg) {
    auto v = read_env_compat(std::string(cfg.base_url_env).c_str());
    return v ? *v : std::string(cfg.default_base_url);
}

// Exposed as static member to match header declaration; impl delegates to free fn
std::string OpenAiCompatClient::chat_completions_endpoint(std::string_view base) {
    return chat_completions_endpoint_impl(base);
}
std::string OpenAiCompatClient::normalize_finish_reason(std::string_view v) {
    return normalize_finish_reason_impl(v);
}

// ── Free helpers ──────────────────────────────────────────────────────────────

bool openai_has_api_key(std::string_view key) {
    return OpenAiCompatClient::has_api_key(key);
}

std::string read_openai_compat_base_url(OpenAiCompatConfig cfg) {
    return OpenAiCompatClient::read_base_url(cfg);
}

std::string read_xai_base_url() {
    return read_openai_compat_base_url(OpenAiCompatConfig::xai());
}

} // namespace claw::api
#endif // HAVE_CURL
