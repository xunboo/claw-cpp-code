#pragma once
#include <tl/expected.hpp>

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>

namespace claw::tools {

// ── Input structs ─────────────────────────────────────────────────────────────

struct WebFetchInput {
    std::string url;
    std::string prompt;
};

struct WebSearchInput {
    std::string                        query;
    std::optional<std::vector<std::string>> allowed_domains;
    std::optional<std::vector<std::string>> blocked_domains;
};

// ── Output structs ────────────────────────────────────────────────────────────

struct WebFetchOutput {
    std::size_t bytes{0};
    uint16_t    code{0};
    std::string code_text;
    std::string result;
    uint64_t    duration_ms{0};
    std::string url;
};

struct SearchHit {
    std::string title;
    std::string url;
};

struct WebSearchOutput {
    std::string              query;
    std::vector<nlohmann::json> results; // Commentary string or SearchResult object
    double                   duration_seconds{0.0};
};

// ── Execution ─────────────────────────────────────────────────────────────────

[[nodiscard]] tl::expected<WebFetchOutput, std::string>
    execute_web_fetch(const WebFetchInput& input);

[[nodiscard]] tl::expected<WebSearchOutput, std::string>
    execute_web_search(const WebSearchInput& input);

// ── Helpers (used internally and in tests) ────────────────────────────────────

[[nodiscard]] std::string html_to_text(const std::string& html);
[[nodiscard]] std::string decode_html_entities(const std::string& input);
[[nodiscard]] std::string collapse_whitespace(const std::string& input);
[[nodiscard]] std::string preview_text(const std::string& input, std::size_t max_chars);
[[nodiscard]] std::vector<SearchHit> extract_search_hits(const std::string& html);
[[nodiscard]] std::vector<SearchHit> extract_search_hits_from_generic_links(const std::string& html);
[[nodiscard]] bool host_matches_list(const std::string& url, const std::vector<std::string>& domains);
[[nodiscard]] std::string normalize_domain_filter(const std::string& domain);
void dedupe_hits(std::vector<SearchHit>& hits);

}  // namespace claw::tools
