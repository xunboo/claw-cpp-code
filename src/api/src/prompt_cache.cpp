// ---------------------------------------------------------------------------
// prompt_cache.cpp  -  Disk-backed prompt cache with FNV-1a fingerprinting
// ---------------------------------------------------------------------------
//
// Mirrors prompt_cache.rs exactly:
//   - FNV-1a hash over nlohmann::json::dump() output for all fingerprints
//   - Cache root: $CLAUDE_CONFIG_HOME/cache/prompt-cache, or $HOME/.claude/…,
//     or $TMPDIR/claude-prompt-cache
//   - Completion entries expire after completion_ttl seconds
//   - Cache-break detection: token-drop ≥ cache_break_min_drop triggers
//     classification as expected or unexpected
// ---------------------------------------------------------------------------

#include "prompt_cache.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace claw::api {

// ── FNV-1a constants (match Rust source verbatim) ─────────────────────────────

static constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
static constexpr uint64_t FNV_PRIME  = 0x00000100000001b3ULL;

static uint64_t fnv1a(std::string_view data) noexcept {
    uint64_t h = FNV_OFFSET;
    for (unsigned char c : data) { h ^= c; h *= FNV_PRIME; }
    return h;
}

// ── Time helpers ───────────────────────────────────────────────────────────────

static uint64_t now_unix_secs() noexcept {
    using namespace std::chrono;
    auto n = system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(duration_cast<seconds>(n).count());
}

// ── JSON hashing (mirrors hash_serializable: FNV-1a over compact JSON dump) ───

static uint64_t hash_json(const nlohmann::json& j) {
    return fnv1a(j.dump());
}

// ── Base cache root ────────────────────────────────────────────────────────────

static std::filesystem::path base_cache_root() {
    if (const char* e = std::getenv("CLAUDE_CONFIG_HOME"))
        return std::filesystem::path(e) / "cache" / "prompt-cache";
    if (const char* h = std::getenv("HOME"))
        return std::filesystem::path(h) / ".claude" / "cache" / "prompt-cache";
    return std::filesystem::temp_directory_path() / "claude-prompt-cache";
}

// ── sanitize_path_segment ──────────────────────────────────────────────────────

std::string sanitize_path_segment(std::string_view value) {
    std::string s;
    s.reserve(value.size());
    for (char c : value)
        s += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';

    if (s.size() <= MAX_SANITIZED_LENGTH) return s;

    // Append hex hash suffix and truncate prefix
    uint64_t h = fnv1a(value);
    std::ostringstream sfx;
    sfx << '-' << std::hex << h;
    std::string suffix = sfx.str();

    size_t prefix_len = (MAX_SANITIZED_LENGTH > suffix.size())
                        ? MAX_SANITIZED_LENGTH - suffix.size()
                        : 0;
    return s.substr(0, prefix_len) + suffix;
}

// ── PromptCachePaths ───────────────────────────────────────────────────────────

PromptCachePaths PromptCachePaths::for_session(std::string_view sid) {
    PromptCachePaths p;
    p.root               = base_cache_root();
    p.session_dir        = p.root / sanitize_path_segment(sid);
    p.completion_dir     = p.session_dir / "completions";
    p.session_state_path = p.session_dir / "session-state.json";
    p.stats_path         = p.session_dir / "stats.json";
    return p;
}

std::filesystem::path PromptCachePaths::completion_entry_path(std::string_view h) const {
    return completion_dir / (std::string(h) + ".json");
}

// ── JSON I/O helpers ───────────────────────────────────────────────────────────

template<typename T>
static std::optional<T> read_json(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    try {
        nlohmann::json j;
        f >> j;
        return j.get<T>();
    } catch (...) {
        return std::nullopt;
    }
}

template<typename T>
static void write_json(const std::filesystem::path& path, const T& val) {
    try {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream f(path);
        if (f) f << nlohmann::json(val).dump(2);
    } catch (...) {}
}

// ── request_hash_hex ───────────────────────────────────────────────────────────
// Mirrors Rust: format!("{REQUEST_FINGERPRINT_PREFIX}-{:016x}", hash_serializable(request))
// hash_serializable uses serde_json compact output; we use nlohmann::json::dump()

std::string request_hash_hex(const MessageRequest& req) {
    uint64_t h = fnv1a(nlohmann::json(req).dump());
    std::ostringstream oss;
    oss << REQUEST_FINGERPRINT_PREFIX << '-'
        << std::hex << std::setw(16) << std::setfill('0') << h;
    return oss.str();
}

// ── Internal types (private to this TU) ───────────────────────────────────────

using TrackedState = PromptCache::Inner::TrackedPromptState;

// nlohmann serialisation for TrackedState.
// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT can't be called inside a
// nested class definition, so we provide to_json/from_json manually.

static void to_json(nlohmann::json& j, const TrackedState& s) {
    j = {{"observed_at_unix_secs",   s.observed_at_unix_secs},
         {"fingerprint_version",      s.fingerprint_version},
         {"model_hash",               s.model_hash},
         {"system_hash",              s.system_hash},
         {"tools_hash",               s.tools_hash},
         {"messages_hash",            s.messages_hash},
         {"cache_read_input_tokens",  s.cache_read_input_tokens}};
}

static void from_json(const nlohmann::json& j, TrackedState& s) {
    s.observed_at_unix_secs  = j.value("observed_at_unix_secs",  uint64_t{0});
    s.fingerprint_version    = j.value("fingerprint_version",    REQUEST_FINGERPRINT_VERSION);
    s.model_hash             = j.value("model_hash",             uint64_t{0});
    s.system_hash            = j.value("system_hash",            uint64_t{0});
    s.tools_hash             = j.value("tools_hash",             uint64_t{0});
    s.messages_hash          = j.value("messages_hash",          uint64_t{0});
    s.cache_read_input_tokens= j.value("cache_read_input_tokens",uint32_t{0});
}

// CompletionCacheEntry

struct CompletionEntry {
    uint64_t cached_at_unix_secs{0};
    uint32_t fingerprint_version{REQUEST_FINGERPRINT_VERSION};
    MessageResponse response;
};

static void to_json(nlohmann::json& j, const CompletionEntry& e) {
    j = {{"cached_at_unix_secs",  e.cached_at_unix_secs},
         {"fingerprint_version",  e.fingerprint_version},
         {"response",             e.response}};
}

static void from_json(const nlohmann::json& j, CompletionEntry& e) {
    e.cached_at_unix_secs = j.value("cached_at_unix_secs", uint64_t{0});
    e.fingerprint_version = j.value("fingerprint_version", REQUEST_FINGERPRINT_VERSION);
    j.at("response").get_to(e.response);
}

// ── RequestFingerprints  (mirrors Rust RequestFingerprints::from_request) ──────
// Each field is hash_serializable(field) = FNV-1a over compact JSON of that field.

struct RequestFingerprints {
    uint64_t model{0};
    uint64_t system{0};
    uint64_t tools{0};
    uint64_t messages{0};

    static RequestFingerprints from_request(const MessageRequest& req) {
        RequestFingerprints fp;
        fp.model    = hash_json(nlohmann::json(req.model));
        fp.system   = hash_json(req.system ? nlohmann::json(*req.system)
                                           : nlohmann::json(nullptr));
        fp.tools    = hash_json(req.tools  ? nlohmann::json(*req.tools)
                                           : nlohmann::json(nullptr));
        fp.messages = hash_json(nlohmann::json(req.messages));
        return fp;
    }
};

// ── TrackedPromptState factory (mirrors TrackedPromptState::from_usage) ─────────

static TrackedState state_from_usage(const MessageRequest& req, const Usage& usage) {
    auto fp = RequestFingerprints::from_request(req);
    TrackedState s;
    s.observed_at_unix_secs   = now_unix_secs();
    s.fingerprint_version     = REQUEST_FINGERPRINT_VERSION;
    s.model_hash              = fp.model;
    s.system_hash             = fp.system;
    s.tools_hash              = fp.tools;
    s.messages_hash           = fp.messages;
    s.cache_read_input_tokens = usage.cache_read_input_tokens;
    return s;
}

// ── detect_cache_break (mirrors detect_cache_break fn) ────────────────────────

static std::optional<CacheBreakEvent> detect_break(
    const PromptCacheConfig& cfg,
    const std::optional<TrackedState>& prev_opt,
    const TrackedState& cur)
{
    if (!prev_opt) return std::nullopt;
    const auto& prev = *prev_opt;

    // Version mismatch → always report (unexpected=false)
    if (prev.fingerprint_version != cur.fingerprint_version) {
        uint32_t drop = (prev.cache_read_input_tokens > cur.cache_read_input_tokens)
                        ? prev.cache_read_input_tokens - cur.cache_read_input_tokens
                        : 0u;
        std::string reason = "fingerprint version changed (v"
            + std::to_string(prev.fingerprint_version)
            + " -> v"
            + std::to_string(cur.fingerprint_version)
            + ")";
        return CacheBreakEvent{false, std::move(reason),
                               prev.cache_read_input_tokens,
                               cur.cache_read_input_tokens, drop};
    }

    uint32_t drop = (prev.cache_read_input_tokens > cur.cache_read_input_tokens)
                    ? prev.cache_read_input_tokens - cur.cache_read_input_tokens
                    : 0u;
    if (drop < cfg.cache_break_min_drop) return std::nullopt;

    // Classify reasons
    std::vector<std::string> reasons;
    if (prev.model_hash    != cur.model_hash)    reasons.emplace_back("model changed");
    if (prev.system_hash   != cur.system_hash)   reasons.emplace_back("system prompt changed");
    if (prev.tools_hash    != cur.tools_hash)    reasons.emplace_back("tool definitions changed");
    if (prev.messages_hash != cur.messages_hash) reasons.emplace_back("message payload changed");

    uint64_t elapsed = (cur.observed_at_unix_secs > prev.observed_at_unix_secs)
                       ? cur.observed_at_unix_secs - prev.observed_at_unix_secs
                       : 0u;

    bool unexpected;
    std::string reason;
    if (reasons.empty()) {
        if (elapsed > static_cast<uint64_t>(cfg.prompt_ttl.count())) {
            unexpected = false;
            reason = "possible prompt cache TTL expiry after "
                     + std::to_string(elapsed) + "s";
        } else {
            unexpected = true;
            reason = "cache read tokens dropped while prompt fingerprint remained stable";
        }
    } else {
        unexpected = false;
        for (size_t i = 0; i < reasons.size(); ++i) {
            if (i) reason += ", ";
            reason += reasons[i];
        }
    }

    return CacheBreakEvent{unexpected, std::move(reason),
                           prev.cache_read_input_tokens,
                           cur.cache_read_input_tokens, drop};
}

// ── apply_usage_to_stats (mirrors apply_usage_to_stats fn) ────────────────────

static void apply_usage(PromptCacheStats& s,
                         const Usage& u,
                         std::string_view h,
                         std::string_view src) {
    s.total_cache_creation_input_tokens += u.cache_creation_input_tokens;
    s.total_cache_read_input_tokens     += u.cache_read_input_tokens;
    s.last_cache_creation_input_tokens   = u.cache_creation_input_tokens;
    s.last_cache_read_input_tokens       = u.cache_read_input_tokens;
    s.last_request_hash                  = std::string(h);
    s.last_cache_source                  = std::string(src);
}

// ── persist_state (mirrors persist_state fn) ──────────────────────────────────

static void persist(PromptCache::Inner& inn) {
    write_json(inn.paths.stats_path, inn.stats);
    if (inn.previous)
        write_json(inn.paths.session_state_path, *inn.previous);
}

// ── PromptCache ────────────────────────────────────────────────────────────────

PromptCache::PromptCache(PromptCacheConfig config) {
    auto paths  = PromptCachePaths::for_session(config.session_id);
    auto stats  = read_json<PromptCacheStats>(paths.stats_path).value_or(PromptCacheStats{});
    auto prev   = read_json<TrackedState>(paths.session_state_path);

    inner_->config   = std::move(config);
    inner_->paths    = std::move(paths);
    inner_->stats    = std::move(stats);
    inner_->previous = std::move(prev);
}

PromptCachePaths PromptCache::paths() const {
    std::lock_guard lk(*mutex_);
    return inner_->paths;
}

PromptCacheStats PromptCache::stats() const {
    std::lock_guard lk(*mutex_);
    return inner_->stats;
}

std::optional<MessageResponse> PromptCache::lookup_completion(const MessageRequest& req) {
    auto hash = request_hash_hex(req);

    // Read TTL and entry path without holding the lock during I/O
    std::chrono::seconds ttl;
    std::filesystem::path ep;
    {
        std::lock_guard lk(*mutex_);
        ttl = inner_->config.completion_ttl;
        ep  = inner_->paths.completion_entry_path(hash);
    }

    auto entry = read_json<CompletionEntry>(ep);

    std::lock_guard lk(*mutex_);
    inner_->stats.last_completion_cache_key = hash;

    // Miss: file missing or version mismatch
    if (!entry || entry->fingerprint_version != REQUEST_FINGERPRINT_VERSION) {
        inner_->stats.completion_cache_misses++;
        if (entry) {
            try { std::filesystem::remove(ep); } catch (...) {}
        }
        persist(*inner_);
        return std::nullopt;
    }

    // Miss: expired
    uint64_t age = now_unix_secs() - entry->cached_at_unix_secs;
    if (age >= static_cast<uint64_t>(ttl.count())) {
        inner_->stats.completion_cache_misses++;
        try { std::filesystem::remove(ep); } catch (...) {}
        persist(*inner_);
        return std::nullopt;
    }

    // Hit
    inner_->stats.completion_cache_hits++;
    apply_usage(inner_->stats, entry->response.usage, hash, "completion-cache");
    inner_->previous = state_from_usage(req, entry->response.usage);
    persist(*inner_);
    return entry->response;
}

PromptCacheRecord PromptCache::record_response(const MessageRequest& req,
                                                const MessageResponse& resp) {
    return record_usage_internal(req, resp.usage, &resp);
}

PromptCacheRecord PromptCache::record_usage(const MessageRequest& req, const Usage& usage) {
    return record_usage_internal(req, usage, nullptr);
}

PromptCacheRecord PromptCache::record_usage_internal(const MessageRequest& req,
                                                      const Usage& usage,
                                                      const MessageResponse* resp) {
    auto hash = request_hash_hex(req);

    std::lock_guard lk(*mutex_);
    auto cur = state_from_usage(req, usage);
    auto cb  = detect_break(inner_->config, inner_->previous, cur);

    inner_->stats.tracked_requests++;
    apply_usage(inner_->stats, usage, hash, "api-response");

    if (cb) {
        if (cb->unexpected)
            inner_->stats.unexpected_cache_breaks++;
        else
            inner_->stats.expected_invalidations++;
        inner_->stats.last_break_reason = cb->reason;
    }

    inner_->previous = cur;

    if (resp) {
        CompletionEntry e{now_unix_secs(), REQUEST_FINGERPRINT_VERSION, *resp};
        write_json(inner_->paths.completion_entry_path(hash), e);
        inner_->stats.completion_cache_writes++;
    }

    persist(*inner_);
    return {std::move(cb), inner_->stats};
}

} // namespace claw::api
