#pragma once

#include "types.hpp"
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace claw::api {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

inline constexpr uint64_t DEFAULT_COMPLETION_TTL_SECS = 30;
inline constexpr uint64_t DEFAULT_PROMPT_TTL_SECS     = 5 * 60;
inline constexpr uint32_t DEFAULT_BREAK_MIN_DROP       = 2'000;
inline constexpr size_t   MAX_SANITIZED_LENGTH         = 80;
inline constexpr uint32_t REQUEST_FINGERPRINT_VERSION  = 1;
inline constexpr std::string_view REQUEST_FINGERPRINT_PREFIX = "v1";

// ---------------------------------------------------------------------------
// PromptCacheConfig
// ---------------------------------------------------------------------------

struct PromptCacheConfig {
    std::string session_id{"default"};
    std::chrono::seconds completion_ttl{DEFAULT_COMPLETION_TTL_SECS};
    std::chrono::seconds prompt_ttl{DEFAULT_PROMPT_TTL_SECS};
    uint32_t cache_break_min_drop{DEFAULT_BREAK_MIN_DROP};

    [[nodiscard]] static PromptCacheConfig for_session(std::string session_id) {
        PromptCacheConfig cfg;
        cfg.session_id = std::move(session_id);
        return cfg;
    }
};

// ---------------------------------------------------------------------------
// PromptCachePaths
// ---------------------------------------------------------------------------

struct PromptCachePaths {
    std::filesystem::path root;
    std::filesystem::path session_dir;
    std::filesystem::path completion_dir;
    std::filesystem::path session_state_path;
    std::filesystem::path stats_path;

    [[nodiscard]] static PromptCachePaths for_session(std::string_view session_id);
    [[nodiscard]] std::filesystem::path completion_entry_path(std::string_view request_hash) const;
};

// ---------------------------------------------------------------------------
// PromptCacheStats
// ---------------------------------------------------------------------------

struct PromptCacheStats {
    uint64_t tracked_requests{0};
    uint64_t completion_cache_hits{0};
    uint64_t completion_cache_misses{0};
    uint64_t completion_cache_writes{0};
    uint64_t expected_invalidations{0};
    uint64_t unexpected_cache_breaks{0};
    uint64_t total_cache_creation_input_tokens{0};
    uint64_t total_cache_read_input_tokens{0};
    std::optional<uint32_t> last_cache_creation_input_tokens;
    std::optional<uint32_t> last_cache_read_input_tokens;
    std::optional<std::string> last_request_hash;
    std::optional<std::string> last_completion_cache_key;
    std::optional<std::string> last_break_reason;
    std::optional<std::string> last_cache_source;
};

inline void to_json(nlohmann::json& j, const PromptCacheStats& s) {
    j = nlohmann::json{
        {"tracked_requests", s.tracked_requests},
        {"completion_cache_hits", s.completion_cache_hits},
        {"completion_cache_misses", s.completion_cache_misses},
        {"completion_cache_writes", s.completion_cache_writes},
        {"expected_invalidations", s.expected_invalidations},
        {"unexpected_cache_breaks", s.unexpected_cache_breaks},
        {"total_cache_creation_input_tokens", s.total_cache_creation_input_tokens},
        {"total_cache_read_input_tokens", s.total_cache_read_input_tokens}
    };
    if (s.last_cache_creation_input_tokens) j["last_cache_creation_input_tokens"] = *s.last_cache_creation_input_tokens;
    if (s.last_cache_read_input_tokens)     j["last_cache_read_input_tokens"]     = *s.last_cache_read_input_tokens;
    if (s.last_request_hash)                j["last_request_hash"]                = *s.last_request_hash;
    if (s.last_completion_cache_key)        j["last_completion_cache_key"]        = *s.last_completion_cache_key;
    if (s.last_break_reason)                j["last_break_reason"]                = *s.last_break_reason;
    if (s.last_cache_source)                j["last_cache_source"]                = *s.last_cache_source;
}
inline void from_json(const nlohmann::json& j, PromptCacheStats& s) {
    s = PromptCacheStats{};
    if (j.contains("tracked_requests"))                  j.at("tracked_requests").get_to(s.tracked_requests);
    if (j.contains("completion_cache_hits"))              j.at("completion_cache_hits").get_to(s.completion_cache_hits);
    if (j.contains("completion_cache_misses"))            j.at("completion_cache_misses").get_to(s.completion_cache_misses);
    if (j.contains("completion_cache_writes"))            j.at("completion_cache_writes").get_to(s.completion_cache_writes);
    if (j.contains("expected_invalidations"))             j.at("expected_invalidations").get_to(s.expected_invalidations);
    if (j.contains("unexpected_cache_breaks"))            j.at("unexpected_cache_breaks").get_to(s.unexpected_cache_breaks);
    if (j.contains("total_cache_creation_input_tokens"))  j.at("total_cache_creation_input_tokens").get_to(s.total_cache_creation_input_tokens);
    if (j.contains("total_cache_read_input_tokens"))      j.at("total_cache_read_input_tokens").get_to(s.total_cache_read_input_tokens);
    if (j.contains("last_cache_creation_input_tokens"))   s.last_cache_creation_input_tokens = j.at("last_cache_creation_input_tokens").get<uint32_t>();
    if (j.contains("last_cache_read_input_tokens"))       s.last_cache_read_input_tokens     = j.at("last_cache_read_input_tokens").get<uint32_t>();
    if (j.contains("last_request_hash"))                  s.last_request_hash                = j.at("last_request_hash").get<std::string>();
    if (j.contains("last_completion_cache_key"))          s.last_completion_cache_key        = j.at("last_completion_cache_key").get<std::string>();
    if (j.contains("last_break_reason"))                  s.last_break_reason                = j.at("last_break_reason").get<std::string>();
    if (j.contains("last_cache_source"))                  s.last_cache_source                = j.at("last_cache_source").get<std::string>();
}

// ---------------------------------------------------------------------------
// CacheBreakEvent
// ---------------------------------------------------------------------------

struct CacheBreakEvent {
    bool unexpected{false};
    std::string reason;
    uint32_t previous_cache_read_input_tokens{0};
    uint32_t current_cache_read_input_tokens{0};
    uint32_t token_drop{0};
};

// ---------------------------------------------------------------------------
// PromptCacheRecord
// ---------------------------------------------------------------------------

struct PromptCacheRecord {
    std::optional<CacheBreakEvent> cache_break;
    PromptCacheStats stats;
};

// ---------------------------------------------------------------------------
// PromptCache
// ---------------------------------------------------------------------------

class PromptCache {
public:
    explicit PromptCache(std::string session_id)
        : PromptCache(PromptCacheConfig::for_session(std::move(session_id))) {}

    explicit PromptCache(PromptCacheConfig config);

    [[nodiscard]] PromptCachePaths paths() const;
    [[nodiscard]] PromptCacheStats stats() const;

    [[nodiscard]] std::optional<MessageResponse> lookup_completion(
        const MessageRequest& request);

    [[nodiscard]] PromptCacheRecord record_response(
        const MessageRequest& request,
        const MessageResponse& response);

    [[nodiscard]] PromptCacheRecord record_usage(
        const MessageRequest& request,
        const Usage& usage);

public:
    struct Inner {
        PromptCacheConfig config;
        PromptCachePaths  paths;
        PromptCacheStats  stats;

        struct TrackedPromptState {
            uint64_t observed_at_unix_secs{0};
            uint32_t fingerprint_version{REQUEST_FINGERPRINT_VERSION};
            uint64_t model_hash{0};
            uint64_t system_hash{0};
            uint64_t tools_hash{0};
            uint64_t messages_hash{0};
            uint32_t cache_read_input_tokens{0};
        };
        std::optional<TrackedPromptState> previous;
    };

    std::shared_ptr<std::mutex> mutex_{std::make_shared<std::mutex>()};
    std::shared_ptr<Inner>      inner_{std::make_shared<Inner>()};

    [[nodiscard]] PromptCacheRecord record_usage_internal(
        const MessageRequest& request,
        const Usage& usage,
        const MessageResponse* response);
};

// ---------------------------------------------------------------------------
// Free helpers (exposed for testing)
// ---------------------------------------------------------------------------

[[nodiscard]] std::string sanitize_path_segment(std::string_view value);
[[nodiscard]] std::string request_hash_hex(const MessageRequest& request);

} // namespace claw::api