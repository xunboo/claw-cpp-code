// ---------------------------------------------------------------------------
// providers/anthropic.cpp  –  Complete C++20 implementation
//
// Rust-to-C++ mapping:
//   async fn            -> synchronous function (std::future wraps the call)
//   reqwest::Client     -> libcurl (RAII CurlHandle)
//   serde_json          -> nlohmann::json
//   Result<T,E>         -> tl::expected<T,E>  / exceptions on error paths
//   Option<T>           -> std::optional<T>
//   Vec<T>              -> std::vector<T>
//   Arc<Mutex<T>>       -> std::shared_ptr<std::mutex> + shared_ptr<T>
// ---------------------------------------------------------------------------

#include "providers/anthropic.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
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
// Stub: curl not available, provide empty implementations
namespace claw::api {
} // namespace claw::api
#else

namespace claw::api {

// ===========================================================================
// Internal helpers
// ===========================================================================

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

static std::optional<std::string> read_env_non_empty(const char* key) {
    const char* v = std::getenv(key);
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

static uint64_t now_unix_ts() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// OAuth credential file path  (mirrors runtime::oauth_creds_path)
// ---------------------------------------------------------------------------

static std::filesystem::path oauth_creds_path() {
    if (const char* e = std::getenv("CLAW_CONFIG_HOME"))
        return std::filesystem::path(e) / "claude_oauth_token.json";
    if (const char* h = std::getenv("HOME"))
        return std::filesystem::path(h) / ".claude" / "claude_oauth_token.json";
    // Windows fallback
    if (const char* appdata = std::getenv("APPDATA"))
        return std::filesystem::path(appdata) / "claude" / "claude_oauth_token.json";
    return std::filesystem::temp_directory_path() / "claude_oauth_token.json";
}

// ---------------------------------------------------------------------------
// load_saved_oauth_token  (mirrors Rust load_saved_oauth_token)
// ---------------------------------------------------------------------------

static std::optional<OAuthTokenSet> load_saved_oauth_token() {
    std::ifstream f(oauth_creds_path());
    if (!f) return std::nullopt;
    try {
        nlohmann::json j;
        f >> j;
        OAuthTokenSet t;
        t.access_token = j.value("access_token", std::string{});
        if (j.contains("refresh_token") && !j["refresh_token"].is_null())
            t.refresh_token = j["refresh_token"].get<std::string>();
        if (j.contains("expires_at") && !j["expires_at"].is_null())
            t.expires_at = j["expires_at"].get<uint64_t>();
        if (j.contains("scopes") && j["scopes"].is_array())
            t.scopes = j["scopes"].get<std::vector<std::string>>();
        return t;
    } catch (...) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// save_oauth_token  (mirrors runtime::save_oauth_credentials)
// ---------------------------------------------------------------------------

static void save_oauth_token(const OAuthTokenSet& t) {
    nlohmann::json j;
    j["access_token"]  = t.access_token;
    j["refresh_token"] = t.refresh_token
                             ? nlohmann::json(*t.refresh_token)
                             : nlohmann::json(nullptr);
    j["expires_at"]    = t.expires_at
                             ? nlohmann::json(*t.expires_at)
                             : nlohmann::json(nullptr);
    j["scopes"] = t.scopes;
    auto path = oauth_creds_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    if (out) out << j.dump(2);
}

// ===========================================================================
// RAII libcurl wrapper
// ===========================================================================

struct CurlHandle {
    CURL* h{nullptr};
    struct curl_slist* headers{nullptr};

    CurlHandle() : h(curl_easy_init()) {
        if (!h) throw std::runtime_error("curl_easy_init() failed");
    }
    ~CurlHandle() {
        if (headers) curl_slist_free_all(headers);
        if (h) curl_easy_cleanup(h);
    }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;

    void add_header(const std::string& hdr) {
        headers = curl_slist_append(headers, hdr.c_str());
    }
};

// libcurl write callback – appends received bytes into a std::string
static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// libcurl header callback – appends each header line into a std::string
static size_t curl_header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* hdrs = static_cast<std::string*>(userdata);
    hdrs->append(buffer, size * nitems);
    return size * nitems;
}

// ---------------------------------------------------------------------------
// parse_response_headers – extract a header value by name (case-insensitive)
// ---------------------------------------------------------------------------

static std::optional<std::string> get_header(
    const std::string& raw_headers,
    std::string_view   name)
{
    // Raw headers: "HTTP/1.1 200 OK\r\nHeader-Name: value\r\n..."
    std::istringstream ss(raw_headers);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.size() > name.size() + 1 &&
            line[name.size()] == ':' &&
            std::equal(line.begin(), line.begin() + static_cast<ptrdiff_t>(name.size()),
                       name.begin(), name.end(),
                       [](unsigned char a, unsigned char b){
                           return std::tolower(a) == std::tolower(b);
                       }))
        {
            auto value = line.substr(name.size() + 1);
            // trim leading/trailing whitespace and \r
            auto start = value.find_first_not_of(" \t\r");
            auto end   = value.find_last_not_of(" \t\r");
            if (start != std::string::npos)
                return value.substr(start, end - start + 1);
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// request_id_from_headers (mirrors Rust request_id_from_headers)
// request-id is primary, x-request-id is fallback
// ---------------------------------------------------------------------------

static std::optional<std::string> request_id_from_headers(const std::string& raw_headers) {
    auto rid = get_header(raw_headers, "request-id");
    if (rid) return rid;
    return get_header(raw_headers, "x-request-id");
}

// ---------------------------------------------------------------------------
// Retryable HTTP status codes (mirrors Rust is_retryable_status)
// ---------------------------------------------------------------------------

static bool is_retryable_status(long status) {
    switch (status) {
        case 408: case 409: case 429:
        case 500: case 502: case 503: case 504:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// HTTP result from a raw curl call
// ---------------------------------------------------------------------------

struct CurlResult {
    long        status{0};
    std::string body;
    std::string raw_headers;
};

// ---------------------------------------------------------------------------
// apply_auth_to_curl – mirrors AuthSource::apply(request_builder)
// ---------------------------------------------------------------------------

static void apply_auth_to_curl(CurlHandle& ch, const AuthSource& auth) {
    if (auto key = auth.get_api_key()) {
        ch.add_header("x-api-key: " + std::string(*key));
    }
    if (auto tok = auth.get_bearer_token()) {
        ch.add_header("Authorization: Bearer " + std::string(*tok));
    }
}

// ---------------------------------------------------------------------------
// do_post_json – perform an HTTP POST with a JSON body, return CurlResult
// ---------------------------------------------------------------------------

static CurlResult do_post_json(
    const std::string&           url,
    const std::string&           json_body,
    const std::vector<std::string>& extra_headers)
{
    CurlHandle ch;
    CurlResult result;

    ch.add_header("Content-Type: application/json");
    ch.add_header("Accept: application/json");
    for (const auto& h : extra_headers)
        ch.add_header(h);

    curl_easy_setopt(ch.h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(ch.h, CURLOPT_POST, 1L);
    curl_easy_setopt(ch.h, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(ch.h, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
    curl_easy_setopt(ch.h, CURLOPT_HTTPHEADER, ch.headers);
    curl_easy_setopt(ch.h, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(ch.h, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(ch.h, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(ch.h, CURLOPT_HEADERDATA, &result.raw_headers);
    curl_easy_setopt(ch.h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(ch.h, CURLOPT_TIMEOUT, 300L);

    CURLcode rc = curl_easy_perform(ch.h);
    if (rc != CURLE_OK) {
        throw ApiError(ApiError::http(curl_easy_strerror(rc),
                                      rc == CURLE_COULDNT_CONNECT,
                                      rc == CURLE_OPERATION_TIMEDOUT));
    }
    curl_easy_getinfo(ch.h, CURLINFO_RESPONSE_CODE, &result.status);
    return result;
}

// ---------------------------------------------------------------------------
// do_post_form – POST application/x-www-form-urlencoded
// ---------------------------------------------------------------------------

static CurlResult do_post_form(
    const std::string& url,
    const std::string& form_body,
    const std::vector<std::string>& extra_headers = {})
{
    CurlHandle ch;
    CurlResult result;

    ch.add_header("Content-Type: application/x-www-form-urlencoded");
    for (const auto& h : extra_headers)
        ch.add_header(h);

    curl_easy_setopt(ch.h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(ch.h, CURLOPT_POST, 1L);
    curl_easy_setopt(ch.h, CURLOPT_POSTFIELDS, form_body.c_str());
    curl_easy_setopt(ch.h, CURLOPT_POSTFIELDSIZE, static_cast<long>(form_body.size()));
    curl_easy_setopt(ch.h, CURLOPT_HTTPHEADER, ch.headers);
    curl_easy_setopt(ch.h, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(ch.h, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(ch.h, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(ch.h, CURLOPT_HEADERDATA, &result.raw_headers);
    curl_easy_setopt(ch.h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(ch.h, CURLOPT_TIMEOUT, 60L);

    CURLcode rc = curl_easy_perform(ch.h);
    if (rc != CURLE_OK) {
        throw ApiError(ApiError::http(curl_easy_strerror(rc),
                                      rc == CURLE_COULDNT_CONNECT,
                                      rc == CURLE_OPERATION_TIMEDOUT));
    }
    curl_easy_getinfo(ch.h, CURLINFO_RESPONSE_CODE, &result.status);
    return result;
}

// ---------------------------------------------------------------------------
// expect_success – mirrors Rust's expect_success async fn
//   Checks status; if not 2xx, throws ApiError::Api.
// ---------------------------------------------------------------------------

static void expect_success(const CurlResult& res) {
    if (res.status >= 200 && res.status < 300) return;

    std::string error_type;
    std::string error_msg;
    try {
        auto j = nlohmann::json::parse(res.body);
        if (j.contains("error") && j["error"].is_object()) {
            auto& e = j["error"];
            error_type = e.value("type", std::string{});
            error_msg  = e.value("message", std::string{});
        }
    } catch (...) {}

    bool retryable = is_retryable_status(res.status);
    throw ApiError(ApiError::api(static_cast<int>(res.status),
                                  error_type,
                                  error_msg,
                                  res.body,
                                  retryable));
}

// ---------------------------------------------------------------------------
// parse_oauth_token_set – deserialise an OAuthTokenSet from JSON text
// ---------------------------------------------------------------------------

static OAuthTokenSet parse_oauth_token_set(const std::string& json_body) {
    auto j = nlohmann::json::parse(json_body);
    OAuthTokenSet t;
    t.access_token = j.value("access_token", std::string{});
    if (j.contains("refresh_token") && !j["refresh_token"].is_null())
        t.refresh_token = j["refresh_token"].get<std::string>();
    if (j.contains("expires_at") && !j["expires_at"].is_null())
        t.expires_at = j["expires_at"].get<uint64_t>();
    if (j.contains("scopes") && j["scopes"].is_array())
        t.scopes = j["scopes"].get<std::vector<std::string>>();
    return t;
}

// ---------------------------------------------------------------------------
// url_encode_form_params – build application/x-www-form-urlencoded body
// ---------------------------------------------------------------------------

static std::string url_encode(const std::string& s) {
    // Use libcurl for correct percent-encoding
    CURL* tmp = curl_easy_init();
    if (!tmp) throw std::runtime_error("curl_easy_init() failed");
    char* enc = curl_easy_escape(tmp, s.c_str(), static_cast<int>(s.size()));
    std::string result(enc);
    curl_free(enc);
    curl_easy_cleanup(tmp);
    return result;
}

static std::string build_form_body(
    const std::vector<std::pair<std::string, std::string>>& params)
{
    std::string body;
    for (const auto& [k, v] : params) {
        if (!body.empty()) body += '&';
        body += url_encode(k) + '=' + url_encode(v);
    }
    return body;
}

// ===========================================================================
// oauth_token_is_expired  (global free function)
// ===========================================================================

bool oauth_token_is_expired(const OAuthTokenSet& t) {
    return t.expires_at.has_value() && *t.expires_at <= now_unix_ts();
}

// ===========================================================================
// read_base_url  (global free function)
// ===========================================================================

std::string read_base_url() {
    const char* e = std::getenv("ANTHROPIC_BASE_URL");
    return (e && *e) ? std::string(e) : std::string(ANTHROPIC_DEFAULT_BASE_URL);
}

// ===========================================================================
// AuthSource
// ===========================================================================

std::optional<std::string_view> AuthSource::get_api_key() const noexcept {
    if (kind == Kind::ApiKey || kind == Kind::ApiKeyAndBearer)
        return std::string_view(api_key);
    return std::nullopt;
}

std::optional<std::string_view> AuthSource::get_bearer_token() const noexcept {
    if (kind == Kind::BearerToken || kind == Kind::ApiKeyAndBearer)
        return std::string_view(bearer_token);
    return std::nullopt;
}

std::string_view AuthSource::masked_authorization_header() const noexcept {
    return get_bearer_token().has_value() ? "Bearer [REDACTED]" : "<absent>";
}

// ---------------------------------------------------------------------------
// AuthSource::from_env  (mirrors Rust AuthSource::from_env)
// ---------------------------------------------------------------------------

AuthSource AuthSource::from_env() {
    auto key   = read_env_non_empty("ANTHROPIC_API_KEY");
    auto token = read_env_non_empty("ANTHROPIC_AUTH_TOKEN");
    if (key && token) return {Kind::ApiKeyAndBearer, *key, *token};
    if (key)          return from_api_key(*key);
    if (token)        return from_bearer(*token);
    throw ApiError(ApiError::missing_credentials(
        "Anthropic", {"ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_API_KEY"}));
}

// ---------------------------------------------------------------------------
// AuthSource::from_env_or_saved  (mirrors Rust AuthSource::from_env_or_saved)
// ---------------------------------------------------------------------------

AuthSource AuthSource::from_env_or_saved() {
    // 1. Try ANTHROPIC_API_KEY
    auto key = read_env_non_empty("ANTHROPIC_API_KEY");
    if (key) {
        auto token = read_env_non_empty("ANTHROPIC_AUTH_TOKEN");
        if (token)
            return {Kind::ApiKeyAndBearer, *key, *token};
        return from_api_key(*key);
    }
    // 2. Try ANTHROPIC_AUTH_TOKEN
    auto token = read_env_non_empty("ANTHROPIC_AUTH_TOKEN");
    if (token) return from_bearer(*token);

    // 3. Try saved OAuth credentials file
    auto saved = load_saved_oauth_token();
    if (!saved) {
        throw ApiError(ApiError::missing_credentials(
            "Anthropic", {"ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_API_KEY"}));
    }
    if (oauth_token_is_expired(*saved)) {
        if (saved->refresh_token.has_value())
            throw ApiError(ApiError::auth(
                "saved OAuth token is expired; load runtime OAuth config to refresh it"));
        throw ApiError(ApiError::expired_oauth_token());
    }
    return from_bearer(saved->access_token);
}

// ===========================================================================
// resolve_saved_oauth_token_set  (private helper, mirrors Rust fn)
//   If the token is valid, return it as-is.
//   If expired but has a refresh_token, perform a refresh and persist.
// ===========================================================================

static OAuthTokenSet resolve_saved_oauth_token_set(
    const OAuthConfig& config,
    OAuthTokenSet      token_set)
{
    if (!oauth_token_is_expired(token_set)) return token_set;

    if (!token_set.refresh_token.has_value())
        throw ApiError(ApiError::expired_oauth_token());

    // Build form body for refresh_token grant
    std::vector<std::pair<std::string, std::string>> params;
    params.emplace_back("grant_type",    "refresh_token");
    params.emplace_back("refresh_token", *token_set.refresh_token);
    params.emplace_back("client_id",     config.client_id);
    // Include scopes if present
    if (!token_set.scopes.empty()) {
        std::string scope_str;
        for (const auto& s : token_set.scopes) {
            if (!scope_str.empty()) scope_str += ' ';
            scope_str += s;
        }
        params.emplace_back("scope", scope_str);
    }

    auto res = do_post_form(config.token_url, build_form_body(params));
    expect_success(res);

    OAuthTokenSet refreshed = parse_oauth_token_set(res.body);

    // Preserve original refresh_token if the response didn't include a new one
    OAuthTokenSet resolved;
    resolved.access_token  = refreshed.access_token;
    resolved.refresh_token = refreshed.refresh_token.has_value()
                                 ? refreshed.refresh_token
                                 : token_set.refresh_token;
    resolved.expires_at    = refreshed.expires_at;
    resolved.scopes        = refreshed.scopes.empty()
                                 ? token_set.scopes
                                 : refreshed.scopes;

    save_oauth_token(resolved);
    return resolved;
}

// ===========================================================================
// resolve_saved_oauth_token  (global free function)
// ===========================================================================

std::optional<OAuthTokenSet> resolve_saved_oauth_token(const OAuthConfig& config) {
    auto saved = load_saved_oauth_token();
    if (!saved) return std::nullopt;
    return resolve_saved_oauth_token_set(config, std::move(*saved));
}

// ===========================================================================
// resolve_startup_auth_source  (global free function)
// ===========================================================================

AuthSource resolve_startup_auth_source(
    std::function<std::optional<OAuthConfig>()> load_oauth_config)
{
    // 1. ANTHROPIC_API_KEY (optionally combined with ANTHROPIC_AUTH_TOKEN)
    auto key = read_env_non_empty("ANTHROPIC_API_KEY");
    if (key) {
        auto tok = read_env_non_empty("ANTHROPIC_AUTH_TOKEN");
        if (tok) return {AuthSource::Kind::ApiKeyAndBearer, *key, *tok};
        return AuthSource::from_api_key(*key);
    }
    // 2. Bearer-only
    auto tok = read_env_non_empty("ANTHROPIC_AUTH_TOKEN");
    if (tok) return AuthSource::from_bearer(*tok);

    // 3. Saved OAuth token
    auto saved = load_saved_oauth_token();
    if (!saved) {
        throw ApiError(ApiError::missing_credentials(
            "Anthropic", {"ANTHROPIC_AUTH_TOKEN", "ANTHROPIC_API_KEY"}));
    }
    if (!oauth_token_is_expired(*saved))
        return AuthSource::from_bearer(saved->access_token);

    if (!saved->refresh_token.has_value())
        throw ApiError(ApiError::expired_oauth_token());

    // 4. Token is expired and has a refresh_token – need config to refresh
    auto cfg = load_oauth_config();
    if (!cfg) {
        throw ApiError(ApiError::auth(
            "saved OAuth token is expired; runtime OAuth config is missing"));
    }
    auto resolved = resolve_saved_oauth_token_set(*cfg, std::move(*saved));
    return AuthSource::from_oauth(resolved);
}

// ===========================================================================
// AnthropicClient – constructors & factories
// ===========================================================================

AnthropicClient::AnthropicClient(std::string api_key)
    : auth_(AuthSource::from_api_key(std::move(api_key))) {}

AnthropicClient::AnthropicClient(AuthSource auth)
    : auth_(std::move(auth)) {}

AnthropicClient AnthropicClient::from_env() {
    return AnthropicClient(AuthSource::from_env_or_saved())
        .with_base_url(read_base_url());
}

// ===========================================================================
// AnthropicClient – builder methods
// ===========================================================================

AnthropicClient AnthropicClient::with_auth_source(AuthSource a) && {
    auth_ = std::move(a);
    return std::move(*this);
}

AnthropicClient AnthropicClient::with_auth_token(std::optional<std::string> t) && {
    // Mirrors Rust with_auth_token: combines existing api_key with new bearer
    std::string cur_key   = auth_.api_key;
    std::string cur_token = (t && !t->empty()) ? *t : std::string{};

    if (!cur_key.empty() && !cur_token.empty())
        auth_ = {AuthSource::Kind::ApiKeyAndBearer, cur_key, cur_token};
    else if (!cur_key.empty())
        auth_ = AuthSource::from_api_key(cur_key);
    else if (!cur_token.empty())
        auth_ = AuthSource::from_bearer(cur_token);
    else
        auth_ = AuthSource::none();

    return std::move(*this);
}

AnthropicClient AnthropicClient::with_base_url(std::string url) && {
    base_url_ = std::move(url);
    return std::move(*this);
}

AnthropicClient AnthropicClient::with_retry_policy(
    uint32_t mr,
    std::chrono::milliseconds ib,
    std::chrono::milliseconds mb) &&
{
    max_retries_    = mr;
    initial_backoff_ = ib;
    max_backoff_    = mb;
    return std::move(*this);
}

AnthropicClient AnthropicClient::with_prompt_cache(PromptCache pc) && {
    prompt_cache_ = std::move(pc);
    return std::move(*this);
}

// ===========================================================================
// AnthropicClient – accessors
// ===========================================================================

std::optional<PromptCacheStats> AnthropicClient::prompt_cache_stats() const {
    if (!prompt_cache_) return std::nullopt;
    return prompt_cache_->stats();
}

std::optional<PromptCacheRecord> AnthropicClient::take_last_prompt_cache_record() {
    std::lock_guard<std::mutex> lk(*record_mutex_);
    auto rec    = std::move(*last_record_);
    *last_record_ = std::nullopt;
    return rec;
}

void AnthropicClient::store_last_prompt_cache_record(PromptCacheRecord r) {
    std::lock_guard<std::mutex> lk(*record_mutex_);
    *last_record_ = std::move(r);
}

// ===========================================================================
// AnthropicClient – static utilities
// ===========================================================================

std::string AnthropicClient::read_base_url() {
    return api::read_base_url();
}

bool AnthropicClient::has_auth_from_env_or_saved() {
    if (read_env_non_empty("ANTHROPIC_API_KEY"))    return true;
    if (read_env_non_empty("ANTHROPIC_AUTH_TOKEN")) return true;
    return load_saved_oauth_token().has_value();
}

bool AnthropicClient::oauth_token_is_expired(const OAuthTokenSet& t) {
    return api::oauth_token_is_expired(t);
}

std::optional<OAuthTokenSet> AnthropicClient::resolve_saved_oauth_token(
    const OAuthConfig& config)
{
    return api::resolve_saved_oauth_token(config);
}

AuthSource AnthropicClient::resolve_startup_auth_source(
    std::function<std::optional<OAuthConfig>()> load_oauth_config)
{
    return api::resolve_startup_auth_source(std::move(load_oauth_config));
}

// ===========================================================================
// AnthropicClient – backoff
// Mirrors Rust backoff_for_attempt:
//   delay = min(initial_backoff * 2^(attempt-1), max_backoff)
// ===========================================================================

std::chrono::milliseconds AnthropicClient::backoff_for_attempt(uint32_t attempt) const {
    // Guard against shift overflow (same as Rust checked_shl)
    if (attempt == 0) return initial_backoff_;
    uint32_t shift = attempt - 1u;
    if (shift >= 32u) return max_backoff_; // overflow guard, clamp to max
    uint64_t mult   = uint64_t{1} << shift;
    auto     millis = initial_backoff_.count() * static_cast<int64_t>(mult);
    auto     delay  = std::chrono::milliseconds(millis);
    return delay < max_backoff_ ? delay : max_backoff_;
}

// ===========================================================================
// AnthropicClient – build_request_headers
//   Constructs the vector of header strings that every /v1/messages call sends.
//   Mirrors request_profile.header_pairs() from Rust.
// ===========================================================================

static std::vector<std::string> build_request_headers(
    const AuthSource& auth,
    std::string_view  api_version)
{
    std::vector<std::string> hdrs;
    hdrs.emplace_back(std::string("anthropic-version: ") + std::string(api_version));
    if (auto key = auth.get_api_key())
        hdrs.emplace_back("x-api-key: " + std::string(*key));
    if (auto tok = auth.get_bearer_token())
        hdrs.emplace_back("Authorization: Bearer " + std::string(*tok));
    return hdrs;
}

// ===========================================================================
// AnthropicClient – internal send_raw_request
//   Mirrors Rust send_raw_request; returns CurlResult.
// ===========================================================================

struct RawResponse {
    long                   status{0};
    std::string            body;
    std::optional<std::string> request_id;
};

static RawResponse send_raw_request_impl(
    const MessageRequest& request,
    const AuthSource&     auth,
    const std::string&    base_url)
{
    // Build URL
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/v1/messages";

    // Serialise body
    nlohmann::json body_json = request;
    std::string    json_body = body_json.dump();

    // Headers
    auto hdrs = build_request_headers(auth, ANTHROPIC_API_VERSION);

    auto res = do_post_json(url, json_body, hdrs);
    RawResponse rr;
    rr.status     = res.status;
    rr.body       = std::move(res.body);
    rr.request_id = request_id_from_headers(res.raw_headers);
    return rr;
}

// ===========================================================================
// AnthropicClient – send_message  (blocking, wraps retry loop)
// ===========================================================================

std::future<MessageResponse> AnthropicClient::send_message(const MessageRequest& req) {
    return std::async(std::launch::async, [this, req]() -> MessageResponse {
        // Force stream=false (mirrors Rust send_message)
        MessageRequest r = req;
        r.stream = false;

        // Prompt cache look-up
        if (prompt_cache_) {
            if (auto cached = prompt_cache_->lookup_completion(r))
                return *cached;
        }

        // Retry loop (mirrors Rust send_with_retry)
        uint32_t             attempts   = 0;
        std::optional<ApiError> last_error;

        while (true) {
            ++attempts;
            RawResponse rr;
            bool request_ok = false;

            try {
                rr         = send_raw_request_impl(r, auth_, base_url_);
                request_ok = true;
            } catch (const ApiError& e) {
                if (e.is_retryable() && attempts <= max_retries_ + 1u) {
                    last_error = e;
                } else {
                    throw;
                }
            } catch (const std::exception& e) {
                throw ApiError(ApiError::http(e.what()));
            }

            if (request_ok) {
                // Check HTTP-level success
                try {
                    // Build a local CurlResult to reuse expect_success
                    CurlResult cr;
                    cr.status = rr.status;
                    cr.body   = rr.body;
                    expect_success(cr);
                } catch (const ApiError& e) {
                    if (e.is_retryable() && attempts <= max_retries_ + 1u) {
                        last_error = e;
                        // fall through to backoff below
                    } else {
                        throw;
                    }
                }

                if (!last_error.has_value()) {
                    // Parse response
                    MessageResponse resp;
                    try {
                        resp = nlohmann::json::parse(rr.body).get<MessageResponse>();
                    } catch (const std::exception& ex) {
                        throw ApiError(ApiError::json(ex.what()));
                    }
                    if (!resp.request_id.has_value())
                        resp.request_id = rr.request_id;

                    // Prompt cache record
                    if (prompt_cache_) {
                        auto record = prompt_cache_->record_response(r, resp);
                        store_last_prompt_cache_record(std::move(record));
                    }
                    return resp;
                }
            }

            // If we've exhausted retries, break and throw
            if (attempts > max_retries_) break;

            // Exponential backoff
            auto delay = backoff_for_attempt(attempts);
            std::this_thread::sleep_for(delay);

            // Clear last_error flag so next iteration attempts fresh
            // (but keep last_error populated for RetriesExhausted below)
        }

        // Retries exhausted
        auto owned = std::make_unique<ApiError>(*last_error);
        throw ApiError(ApiError::retries_exhausted(attempts, std::move(owned)));
    });
}

// ===========================================================================
// AnthropicClient – stream_message
//   Performs the HTTP POST with stream=true, buffers the full response body,
//   and returns an AnthropicMessageStream that iterates SSE frames.
// ===========================================================================

std::future<AnthropicMessageStream> AnthropicClient::stream_message(
    const MessageRequest& req)
{
    return std::async(std::launch::async, [this, req]() -> AnthropicMessageStream {
        MessageRequest r = req.with_streaming(); // stream=true

        // We do not retry streaming requests (same behaviour as Rust: retry
        // happens inside send_with_retry which is called once; for streaming
        // the response object is returned immediately after a successful HTTP
        // exchange, and events are consumed by the caller).
        uint32_t             attempts   = 0;
        std::optional<ApiError> last_error;
        RawResponse          rr;

        while (true) {
            ++attempts;
            bool ok = false;
            try {
                rr = send_raw_request_impl(r, auth_, base_url_);
                // Check HTTP success
                CurlResult cr;
                cr.status = rr.status;
                cr.body   = rr.body;
                expect_success(cr);
                ok = true;
            } catch (const ApiError& e) {
                if (e.is_retryable() && attempts <= max_retries_ + 1u) {
                    last_error = e;
                } else {
                    throw;
                }
            } catch (const std::exception& e) {
                throw ApiError(ApiError::http(e.what()));
            }

            if (ok) break;
            if (attempts > max_retries_) break;
            std::this_thread::sleep_for(backoff_for_attempt(attempts));
        }

        if (!last_error.has_value() == false) {
            // last_error was set and we never got ok=true
            auto owned = std::make_unique<ApiError>(*last_error);
            throw ApiError(ApiError::retries_exhausted(attempts, std::move(owned)));
        }

        return AnthropicMessageStream(
            rr.request_id,
            {},           // initial_body (unused; raw_body holds the SSE text)
            std::move(rr.body),
            r,
            prompt_cache_,
            record_mutex_,
            last_record_);
    });
}

// ===========================================================================
// AnthropicClient – OAuth helpers
// ===========================================================================

// exchange_oauth_code
//   Mirrors Rust exchange_oauth_code; issues a code→token POST.
std::future<OAuthTokenSet> AnthropicClient::exchange_oauth_code(
    const OAuthConfig& config,
    const std::string& code,
    const std::string& redirect_uri)
{
    return std::async(std::launch::async,
        [config, code, redirect_uri]() -> OAuthTokenSet {
            std::vector<std::pair<std::string, std::string>> params;
            params.emplace_back("grant_type",   "authorization_code");
            params.emplace_back("code",          code);
            params.emplace_back("redirect_uri",  redirect_uri);
            params.emplace_back("client_id",     config.client_id);
            if (!config.scopes.empty()) {
                std::string scope_str;
                for (const auto& s : config.scopes) {
                    if (!scope_str.empty()) scope_str += ' ';
                    scope_str += s;
                }
                params.emplace_back("scope", scope_str);
            }

            auto res = do_post_form(config.token_url, build_form_body(params));
            expect_success(res);
            return parse_oauth_token_set(res.body);
        });
}

// refresh_oauth_token
//   Mirrors Rust refresh_oauth_token; issues a refresh_token grant POST.
std::future<OAuthTokenSet> AnthropicClient::refresh_oauth_token(
    const OAuthConfig&              config,
    const std::string&              refresh_token,
    const std::vector<std::string>& scopes)
{
    return std::async(std::launch::async,
        [config, refresh_token, scopes]() -> OAuthTokenSet {
            std::vector<std::pair<std::string, std::string>> params;
            params.emplace_back("grant_type",    "refresh_token");
            params.emplace_back("refresh_token",  refresh_token);
            params.emplace_back("client_id",      config.client_id);
            if (!scopes.empty()) {
                std::string scope_str;
                for (const auto& s : scopes) {
                    if (!scope_str.empty()) scope_str += ' ';
                    scope_str += s;
                }
                params.emplace_back("scope", scope_str);
            }

            auto res = do_post_form(config.token_url, build_form_body(params));
            expect_success(res);
            return parse_oauth_token_set(res.body);
        });
}

// ===========================================================================
// AnthropicMessageStream
// ===========================================================================

AnthropicMessageStream::AnthropicMessageStream(
    std::optional<std::string>                        rid,
    std::vector<uint8_t>                              /*initial_body*/,
    std::string                                       raw,
    MessageRequest                                    req,
    std::optional<PromptCache>                        pc,
    std::shared_ptr<std::mutex>                       rm,
    std::shared_ptr<std::optional<PromptCacheRecord>> lr)
    : request_id_(std::move(rid))
    , raw_body_(std::move(raw))
    , request_(std::move(req))
    , prompt_cache_(std::move(pc))
    , record_mutex_(std::move(rm))
    , last_record_(std::move(lr))
{}

std::optional<std::string_view> AnthropicMessageStream::request_id() const noexcept {
    if (request_id_.has_value())
        return std::string_view(*request_id_);
    return std::nullopt;
}

// pump_next_chunk – feeds the next slice of raw_body_ into the SSE parser.
// Returns false when the body has been fully consumed (stream done).
bool AnthropicMessageStream::pump_next_chunk() {
    if (body_pos_ >= raw_body_.size()) {
        done_ = true;
        return false;
    }
    // Feed up to 4 KiB at a time (same chunking as the original stub)
    size_t chunk_size = std::min(size_t{4096}, raw_body_.size() - body_pos_);
    auto   events     = parser_.push(
        std::string_view(raw_body_.data() + body_pos_, chunk_size));
    body_pos_ += chunk_size;
    for (auto& e : events) pending_.push_back(std::move(e));
    return true;
}

// observe_event – mirrors Rust MessageStream::observe_event
//   Tracks usage from MessageDelta; records prompt cache on MessageStop.
void AnthropicMessageStream::observe_event(const StreamEvent& event) {
    if (event.kind == StreamEvent::Kind::MessageDelta) {
        latest_usage_ = event.message_delta.usage;
    } else if (event.kind == StreamEvent::Kind::MessageStop) {
        if (!usage_recorded_) {
            if (prompt_cache_.has_value() && latest_usage_.has_value()) {
                auto rec = prompt_cache_->record_usage(request_, *latest_usage_);
                std::lock_guard<std::mutex> lk(*record_mutex_);
                *last_record_ = std::move(rec);
            }
            usage_recorded_ = true;
        }
    }
}

// next_event – mirrors Rust MessageStream::next_event (synchronous version)
//   Returns the next StreamEvent, or std::nullopt when the stream is done.
std::optional<StreamEvent> AnthropicMessageStream::next_event() {
    for (;;) {
        // Return any already-parsed events first
        if (!pending_.empty()) {
            auto evt = std::move(pending_.front());
            pending_.pop_front();
            observe_event(evt);
            return evt;
        }

        if (done_) {
            // Flush the parser's internal buffer on stream completion
            auto remaining = parser_.finish();
            for (auto& e : remaining) pending_.push_back(std::move(e));
            if (!pending_.empty()) continue; // loop to return them
            return std::nullopt;
        }

        // Feed the next chunk from the buffered body
        pump_next_chunk();
        // After pumping, loop back to check pending_ / done_
    }
}

} // namespace claw::api
#endif // HAVE_CURL
