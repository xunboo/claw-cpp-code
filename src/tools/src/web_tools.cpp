#include "web_tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <tl/expected.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// HTTP client via libcurl
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define HAVE_CURL 1
#else
#define HAVE_CURL 0
#endif

#if !HAVE_CURL
namespace claw::tools {} // stub
#else
namespace claw::tools {

// ── CURL helpers ──────────────────────────────────────────────────────────────

namespace {

static std::size_t curl_write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

struct CurlHandle {
    CURL* h{nullptr};
    explicit CurlHandle() : h(curl_easy_init()) {}
    ~CurlHandle() { if (h) curl_easy_cleanup(h); }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
};

struct CurlResponse {
    std::string body;
    long        status_code{0};
    std::string final_url;
    std::string content_type;
};

tl::expected<CurlResponse, std::string>
curl_get(const std::string& url, long timeout_secs = 20) {
    CurlHandle ch;
    if (!ch.h) return tl::unexpected("curl_easy_init failed");

    CurlResponse resp;
    curl_easy_setopt(ch.h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(ch.h, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(ch.h, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(ch.h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(ch.h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(ch.h, CURLOPT_TIMEOUT, timeout_secs);
    curl_easy_setopt(ch.h, CURLOPT_USERAGENT, "clawd-cpp-tools/0.1");
    curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode rc = curl_easy_perform(ch.h);
    if (rc != CURLE_OK)
        return tl::unexpected(curl_easy_strerror(rc));

    curl_easy_getinfo(ch.h, CURLINFO_RESPONSE_CODE, &resp.status_code);

    char* eff_url = nullptr;
    curl_easy_getinfo(ch.h, CURLINFO_EFFECTIVE_URL, &eff_url);
    if (eff_url) resp.final_url = eff_url;
    else         resp.final_url = url;

    char* ct = nullptr;
    curl_easy_getinfo(ch.h, CURLINFO_CONTENT_TYPE, &ct);
    if (ct) resp.content_type = ct;

    return resp;
}

// Upgrade http:// to https:// unless localhost
tl::expected<std::string, std::string>
normalize_fetch_url(const std::string& url) {
    if (url.starts_with("http://")) {
        // check for localhost
        auto host_start = url.find("://") + 3;
        auto host_end   = url.find_first_of("/:?#", host_start);
        auto host       = url.substr(host_start,
                              host_end == std::string::npos ? std::string::npos
                                                             : host_end - host_start);
        if (host != "localhost" && host != "127.0.0.1" && host != "::1")
            return "https://" + url.substr(7);
    }
    return url;
}

std::string build_search_url(const std::string& query) {
    if (const char* base = std::getenv("CLAWD_WEB_SEARCH_BASE_URL")) {
        std::string url = base;
        url += (url.find('?') == std::string::npos ? "?" : "&");
        // percent-encode the query
        char* enc = curl_easy_escape(nullptr, query.c_str(), static_cast<int>(query.size()));
        url += "q=";
        url += (enc ? enc : query);
        curl_free(enc);
        return url;
    }
    char* enc = curl_easy_escape(nullptr, query.c_str(), static_cast<int>(query.size()));
    std::string url = "https://html.duckduckgo.com/html/?q=";
    url += (enc ? enc : query);
    curl_free(enc);
    return url;
}

}  // namespace

// ── Text helpers ──────────────────────────────────────────────────────────────

std::string decode_html_entities(const std::string& input) {
    std::string out = input;
    struct { const char* from; const char* to; } table[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "}
    };
    for (auto& [from, to] : table) {
        std::size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, std::strlen(from), to);
            pos += std::strlen(to);
        }
    }
    return out;
}

std::string html_to_text(const std::string& html) {
    std::string text;
    text.reserve(html.size());
    bool in_tag = false;
    bool prev_space = false;

    for (char ch : html) {
        if (ch == '<')       { in_tag = true; continue; }
        if (ch == '>')       { in_tag = false; continue; }
        if (in_tag)          continue;
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!prev_space) { text += ' '; prev_space = true; }
        } else {
            text += ch;
            prev_space = false;
        }
    }
    return collapse_whitespace(decode_html_entities(text));
}

std::string collapse_whitespace(const std::string& input) {
    std::string out;
    bool prev_space = false;
    for (char ch : input) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!prev_space) { out += ' '; prev_space = true; }
        } else {
            out += ch;
            prev_space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string preview_text(const std::string& input, std::size_t max_chars) {
    // count Unicode code points
    std::size_t count = 0;
    std::size_t byte_pos = 0;
    for (; byte_pos < input.size() && count < max_chars; ++byte_pos) {
        if ((input[byte_pos] & 0xC0) != 0x80) ++count;
    }
    if (count <= max_chars) return input;
    // trim trailing space
    while (byte_pos > 0 && input[byte_pos - 1] == ' ') --byte_pos;
    return input.substr(0, byte_pos) + "\xe2\x80\xa6"; // UTF-8 ellipsis '…'
}

static std::optional<std::string>
extract_quoted_value(const std::string& s, std::size_t pos, std::string& value_out) {
    if (pos >= s.size()) return std::nullopt;
    char q = s[pos];
    if (q != '"' && q != '\'') return std::nullopt;
    auto end = s.find(q, pos + 1);
    if (end == std::string::npos) return std::nullopt;
    value_out = s.substr(pos + 1, end - pos - 1);
    return s.substr(end + 1); // rest
}

static std::optional<std::string>
decode_duckduckgo_redirect(const std::string& url) {
    if (url.starts_with("http://") || url.starts_with("https://"))
        return decode_html_entities(url);

    std::string joined;
    if (url.starts_with("//"))
        joined = "https:" + url;
    else if (!url.empty() && url[0] == '/')
        joined = "https://duckduckgo.com" + url;
    else
        return std::nullopt;

    // look for /l/ path with uddg param
    auto q_pos = joined.find('?');
    if (q_pos != std::string::npos) {
        auto path_end = joined.find('?');
        auto path = joined.substr(0, path_end);
        if (path.ends_with("/l/") || path.ends_with("/l")) {
            auto params = joined.substr(path_end + 1);
            std::istringstream ss(params);
            std::string kv;
            while (std::getline(ss, kv, '&')) {
                auto eq = kv.find('=');
                if (eq == std::string::npos) continue;
                if (kv.substr(0, eq) == "uddg") {
                    // URL-decode
                    auto val = kv.substr(eq + 1);
                    // simple percent-decode
                    std::string decoded;
                    for (std::size_t i = 0; i < val.size(); ++i) {
                        if (val[i] == '%' && i + 2 < val.size()) {
                            char hex[3] = { val[i+1], val[i+2], 0 };
                            decoded += static_cast<char>(std::strtol(hex, nullptr, 16));
                            i += 2;
                        } else if (val[i] == '+') {
                            decoded += ' ';
                        } else {
                            decoded += val[i];
                        }
                    }
                    return decode_html_entities(decoded);
                }
            }
        }
    }
    return joined;
}

std::vector<SearchHit> extract_search_hits(const std::string& html) {
    std::vector<SearchHit> hits;
    std::size_t pos = 0;
    while (true) {
        auto anchor_start = html.find("result__a", pos);
        if (anchor_start == std::string::npos) break;

        auto href_idx = html.find("href=", anchor_start);
        if (href_idx == std::string::npos) { pos = anchor_start + 1; continue; }

        std::string url_val;
        auto rest_opt = extract_quoted_value(html, href_idx + 5, url_val);
        if (!rest_opt) { pos = anchor_start + 1; continue; }
        auto& rest = *rest_opt;

        auto close_tag_idx = rest.find('>');
        if (close_tag_idx == std::string::npos) { pos = anchor_start + 1; continue; }
        auto after_tag = rest.substr(close_tag_idx + 1);

        auto end_anchor = after_tag.find("</a>");
        if (end_anchor == std::string::npos) { pos = anchor_start + 1; continue; }

        auto title = html_to_text(after_tag.substr(0, end_anchor));
        if (auto decoded = decode_duckduckgo_redirect(url_val)) {
            hits.push_back({title, *decoded});
        }
        pos = html.find("result__a", anchor_start + 1);
        if (pos == std::string::npos) break;
    }
    return hits;
}

std::vector<SearchHit> extract_search_hits_from_generic_links(const std::string& html) {
    std::vector<SearchHit> hits;
    std::size_t pos = 0;
    while (true) {
        auto anchor_start = html.find("<a", pos);
        if (anchor_start == std::string::npos) break;

        auto href_idx = html.find("href=", anchor_start);
        if (href_idx == std::string::npos) { pos = anchor_start + 2; continue; }

        std::string url_val;
        auto rest_opt = extract_quoted_value(html, href_idx + 5, url_val);
        if (!rest_opt) { pos = anchor_start + 2; continue; }
        auto& rest = *rest_opt;

        auto close_tag_idx = rest.find('>');
        if (close_tag_idx == std::string::npos) { pos = anchor_start + 2; continue; }
        auto after_tag = rest.substr(close_tag_idx + 1);

        auto end_anchor = after_tag.find("</a>");
        if (end_anchor == std::string::npos) { pos = anchor_start + 2; continue; }

        auto title = html_to_text(after_tag.substr(0, end_anchor));
        if (!title.empty()) {
            auto decoded = decode_duckduckgo_redirect(url_val).value_or(url_val);
            if (decoded.starts_with("http://") || decoded.starts_with("https://"))
                hits.push_back({title, decoded});
        }
        pos = after_tag.find("</a>") + 4;
    }
    return hits;
}

bool host_matches_list(const std::string& url, const std::vector<std::string>& domains) {
    // extract host from url
    auto host_start = url.find("://");
    if (host_start == std::string::npos) return false;
    host_start += 3;
    auto host_end = url.find_first_of("/:?#", host_start);
    std::string host = url.substr(host_start,
        host_end == std::string::npos ? std::string::npos : host_end - host_start);
    std::transform(host.begin(), host.end(), host.begin(),
        [](unsigned char c) { return std::tolower(c); });

    return std::any_of(domains.begin(), domains.end(), [&](const std::string& d) {
        auto norm = normalize_domain_filter(d);
        if (norm.empty()) return false;
        return host == norm || host.ends_with("." + norm);
    });
}

std::string normalize_domain_filter(const std::string& domain) {
    std::string trimmed = domain;
    // strip leading/trailing whitespace
    auto s = trimmed.find_first_not_of(" \t");
    auto e = trimmed.find_last_not_of(" \t/");
    if (s == std::string::npos) return {};
    trimmed = trimmed.substr(s, e - s + 1);
    // if it looks like a URL, extract host
    if (auto host_start = trimmed.find("://"); host_start != std::string::npos) {
        host_start += 3;
        auto host_end = trimmed.find_first_of("/:?#", host_start);
        trimmed = trimmed.substr(host_start,
            host_end == std::string::npos ? std::string::npos : host_end - host_start);
    }
    // trim leading dots
    while (!trimmed.empty() && trimmed[0] == '.') trimmed = trimmed.substr(1);
    std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return trimmed;
}

void dedupe_hits(std::vector<SearchHit>& hits) {
    std::set<std::string> seen;
    auto it = std::remove_if(hits.begin(), hits.end(),
        [&](const SearchHit& h) { return !seen.insert(h.url).second; });
    hits.erase(it, hits.end());
}

// ── Content analysis ──────────────────────────────────────────────────────────

static std::optional<std::string>
extract_title(const std::string& content, const std::string& raw_body, const std::string& ct) {
    if (ct.find("html") != std::string::npos) {
        std::string lowered = raw_body;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char c) { return std::tolower(c); });
        auto start = lowered.find("<title>");
        if (start != std::string::npos) {
            auto after = start + 7;
            auto end   = lowered.find("</title>", after);
            if (end != std::string::npos) {
                auto title = collapse_whitespace(
                    decode_html_entities(raw_body.substr(after, end - after)));
                if (!title.empty()) return title;
            }
        }
    }
    for (auto& line : std::vector<std::string_view>{}) {
        (void)line; // iterate lines via stream
    }
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        std::string trimmed = line;
        auto s = trimmed.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        trimmed = trimmed.substr(s);
        if (!trimmed.empty()) return trimmed;
    }
    return std::nullopt;
}

static std::string normalize_fetched_content(const std::string& body, const std::string& ct) {
    if (ct.find("html") != std::string::npos)
        return html_to_text(body);
    // trim
    auto s = body.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return {};
    auto e = body.find_last_not_of(" \t\r\n");
    return body.substr(s, e - s + 1);
}

static std::string summarize_web_fetch(const std::string& url,
                                       const std::string& prompt,
                                       const std::string& content,
                                       const std::string& raw_body,
                                       const std::string& ct) {
    std::string lower_prompt = prompt;
    std::transform(lower_prompt.begin(), lower_prompt.end(), lower_prompt.begin(),
        [](unsigned char c) { return std::tolower(c); });
    auto compact = collapse_whitespace(content);

    std::string detail;
    if (lower_prompt.find("title") != std::string::npos) {
        auto t = extract_title(content, raw_body, ct);
        detail = t ? ("Title: " + *t) : preview_text(compact, 600);
    } else if (lower_prompt.find("summary") != std::string::npos ||
               lower_prompt.find("summarize") != std::string::npos) {
        detail = preview_text(compact, 900);
    } else {
        detail = "Prompt: " + prompt + "\nContent preview:\n" + preview_text(compact, 900);
    }
    return "Fetched " + url + "\n" + detail;
}

// ── Public execute functions ──────────────────────────────────────────────────

tl::expected<WebFetchOutput, std::string>
execute_web_fetch(const WebFetchInput& input) {
    auto url_result = normalize_fetch_url(input.url);
    if (!url_result) return tl::unexpected(url_result.error());

    auto t0 = std::chrono::steady_clock::now();
    auto resp_result = curl_get(*url_result);
    if (!resp_result) return tl::unexpected(resp_result.error());
    auto& resp = *resp_result;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    const std::string& body = resp.body;
    auto normalized = normalize_fetched_content(body, resp.content_type);
    auto result = summarize_web_fetch(resp.final_url, input.prompt,
                                      normalized, body, resp.content_type);

    // HTTP status code text
    std::string code_text;
    switch (resp.status_code) {
        case 200: code_text = "OK"; break;
        case 301: code_text = "Moved Permanently"; break;
        case 302: code_text = "Found"; break;
        case 404: code_text = "Not Found"; break;
        case 500: code_text = "Internal Server Error"; break;
        default:  code_text = "Unknown"; break;
    }

    return WebFetchOutput{
        body.size(),
        static_cast<uint16_t>(resp.status_code),
        code_text,
        result,
        static_cast<uint64_t>(elapsed),
        resp.final_url,
    };
}

tl::expected<WebSearchOutput, std::string>
execute_web_search(const WebSearchInput& input) {
    auto t0 = std::chrono::steady_clock::now();
    auto search_url = build_search_url(input.query);

    auto resp_result = curl_get(search_url);
    if (!resp_result) return tl::unexpected(resp_result.error());
    const auto& html = resp_result->body;

    auto hits = extract_search_hits(html);
    if (hits.empty())
        hits = extract_search_hits_from_generic_links(html);

    if (input.allowed_domains) {
        auto& allowed = *input.allowed_domains;
        hits.erase(std::remove_if(hits.begin(), hits.end(),
            [&](const SearchHit& h) { return !host_matches_list(h.url, allowed); }),
            hits.end());
    }
    if (input.blocked_domains) {
        auto& blocked = *input.blocked_domains;
        hits.erase(std::remove_if(hits.begin(), hits.end(),
            [&](const SearchHit& h) { return host_matches_list(h.url, blocked); }),
            hits.end());
    }

    dedupe_hits(hits);
    if (hits.size() > 8) hits.resize(8);

    double duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    // Build summary
    std::string summary;
    if (hits.empty()) {
        summary = std::string("No web search results matched the query \"") + input.query + "\".";
    } else {
        std::string rendered;
        for (auto& h : hits)
            rendered += "- [" + h.title + "](" + h.url + ")\n";
        summary = "Search results for \"" + input.query +
                  "\". Include a Sources section in the final answer.\n" + rendered;
    }

    // Build JSON results array
    nlohmann::json commentary = summary;
    nlohmann::json search_result;
    search_result["tool_use_id"] = "web_search_1";
    nlohmann::json content_arr = nlohmann::json::array();
    for (auto& h : hits)
        content_arr.push_back({{"title", h.title}, {"url", h.url}});
    search_result["content"] = content_arr;

    return WebSearchOutput{
        input.query,
        {commentary, search_result},
        duration,
    };
}

}  // namespace claw::tools
#endif // HAVE_CURL
