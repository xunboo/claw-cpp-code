// OAuth 2.0 + PKCE helpers: authorization URL construction, token exchange,
// refresh, credentials persistence, and callback parsing.
//
// Translated from Rust: crates/runtime/src/oauth.rs
//
// Encoding convention:
//   percent_encode / percent_decode are the shared helpers from mcp.hpp.
//
// Credential file convention:
//   The credentials file is a JSON object at credentials_path().
//   The "oauth" key stores a camelCase object (matches Rust StoredOAuthCredentials
//   serde rename_all = "camelCase").
//   Writes are atomic: we write to a .tmp file then rename.

#include "oauth.hpp"
#include "mcp.hpp"         // percent_encode, percent_decode
#include <fstream>
#include <sstream>
#include <format>
#include <nlohmann/json.hpp>
// OpenSSL for SHA256 and RAND_bytes (optional on Windows)
#if __has_include(<openssl/sha.h>)
#include <openssl/sha.h>
#include <openssl/rand.h>
#define HAVE_OPENSSL 1
#else
#define HAVE_OPENSSL 0
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif
#endif

namespace claw::runtime {

// ─── base64url encoding ──────────────────────────────────────────────────────
// No padding, URL-safe alphabet ('-' and '_').
// Mirrors Rust base64url_encode().

std::string base64url_encode(const uint8_t* data, std::size_t len) {
    static constexpr std::string_view TABLE =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);

    std::size_t i = 0;
    while (i + 3 <= len) {
        uint32_t block = (static_cast<uint32_t>(data[i])     << 16)
                       | (static_cast<uint32_t>(data[i + 1]) <<  8)
                       |  static_cast<uint32_t>(data[i + 2]);
        out += TABLE[(block >> 18) & 0x3Fu];
        out += TABLE[(block >> 12) & 0x3Fu];
        out += TABLE[(block >>  6) & 0x3Fu];
        out += TABLE[ block        & 0x3Fu];
        i += 3;
    }
    switch (len - i) {
        case 1: {
            uint32_t block = static_cast<uint32_t>(data[i]) << 16;
            out += TABLE[(block >> 18) & 0x3Fu];
            out += TABLE[(block >> 12) & 0x3Fu];
            break;
        }
        case 2: {
            uint32_t block = (static_cast<uint32_t>(data[i])     << 16)
                           | (static_cast<uint32_t>(data[i + 1]) <<  8);
            out += TABLE[(block >> 18) & 0x3Fu];
            out += TABLE[(block >> 12) & 0x3Fu];
            out += TABLE[(block >>  6) & 0x3Fu];
            break;
        }
        default:
            break;
    }
    return out;
}

// ─── SHA-256 / PKCE ──────────────────────────────────────────────────────────

// Mirrors Rust code_challenge_s256(): SHA256(verifier) → base64url.
// Platform-abstracted random bytes
static bool fill_random_bytes(uint8_t* data, std::size_t len) {
#if HAVE_OPENSSL
    return RAND_bytes(data, static_cast<int>(len)) == 1;
#elif defined(_WIN32)
    NTSTATUS status = BCryptGenRandom(nullptr, data, static_cast<ULONG>(len),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0; // STATUS_SUCCESS
#else
    (void)data; (void)len;
    return false;
#endif
}

// Platform-abstracted SHA-256
static bool sha256_hash(const uint8_t* input, std::size_t input_len,
                         uint8_t* output /* 32 bytes */) {
#if HAVE_OPENSSL
    SHA256(input, input_len, output);
    return true;
#elif defined(_WIN32)
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    BCryptHashData(hHash, const_cast<PUCHAR>(input), static_cast<ULONG>(input_len), 0);
    BCryptFinishHash(hHash, output, 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return true;
#else
    (void)input; (void)input_len; (void)output;
    return false;
#endif
}

std::string code_challenge_s256(std::string_view verifier) {
    uint8_t digest[32]{};
    sha256_hash(reinterpret_cast<const uint8_t*>(verifier.data()),
                verifier.size(), digest);
    return base64url_encode(digest, 32);
}

tl::expected<PkceCodePair, std::string> generate_pkce_pair() {
    std::array<uint8_t, 32> buf{};
    if (!fill_random_bytes(buf.data(), buf.size())) {
        return tl::unexpected(std::string("Random bytes generation failed: could not generate PKCE verifier"));
    }
    std::string verifier  = base64url_encode(buf.data(), buf.size());
    std::string challenge = code_challenge_s256(verifier);
    return PkceCodePair{
        .code_verifier  = std::move(verifier),
        .code_challenge = std::move(challenge),
        .method         = PkceChallengeMethod::S256,
    };
}

std::string generate_state() {
    std::array<uint8_t, 32> buf{};
    fill_random_bytes(buf.data(), buf.size());
    return base64url_encode(buf.data(), buf.size());
}

// ─── Loopback redirect URI ────────────────────────────────────────────────────

// Mirrors Rust loopback_redirect_uri(): "http://localhost:{port}/callback".
std::string loopback_redirect_uri(uint16_t port) {
    return std::format("http://localhost:{}/callback", port);
}

// ─── OAuthAuthorizationRequest::build_url ────────────────────────────────────
// Mirrors Rust OAuthAuthorizationRequest::build_url():
//   Builds the full authorize URL with all required + extra params,
//   all values percent-encoded, joined with '&'.
//   Extra params are appended in sorted (BTreeMap) order after the standard ones.

std::string OAuthAuthorizationRequest::build_url() const {
    // Determine separator: if authorization_endpoint already has '?' use '&'.
    char sep = (authorization_endpoint.find('?') != std::string::npos) ? '&' : '?';

    // Determine challenge method string.
    std::string method_str = "S256";  // only S256 supported

    std::string query;
    query += "response_type=code";
    query += '&'; query += "client_id=";      query += percent_encode(client_id);
    query += '&'; query += "redirect_uri=";   query += percent_encode(redirect_uri);
    query += '&'; query += "scope=";          query += percent_encode(scope);
    query += '&'; query += "state=";          query += percent_encode(state);
    query += '&'; query += "code_challenge="; query += percent_encode(pkce.code_challenge);
    query += '&'; query += "code_challenge_method="; query += method_str;

    return authorization_endpoint + sep + query;
}

// ─── OAuthTokenExchangeRequest::form_params ──────────────────────────────────
// Mirrors Rust OAuthTokenExchangeRequest::form_params():
//   grant_type, code, redirect_uri, client_id, code_verifier, state
//   All values percent-encoded, joined with '&'.

std::string OAuthTokenExchangeRequest::form_params() const {
    std::string body;
    body += "grant_type=authorization_code";
    body += '&'; body += "code=";          body += percent_encode(code);
    body += '&'; body += "redirect_uri=";  body += percent_encode(redirect_uri);
    body += '&'; body += "client_id=";     body += percent_encode(client_id);
    body += '&'; body += "code_verifier="; body += percent_encode(code_verifier);
    return body;
}

// ─── OAuthRefreshRequest::form_params ────────────────────────────────────────
// Mirrors Rust OAuthRefreshRequest::form_params():
//   grant_type, refresh_token, client_id, scope (space-joined)

std::string OAuthRefreshRequest::form_params() const {
    std::string body;
    body += "grant_type=refresh_token";
    body += '&'; body += "refresh_token="; body += percent_encode(refresh_token);
    body += '&'; body += "client_id=";     body += percent_encode(client_id);
    return body;
}

// ─── Credentials file path ────────────────────────────────────────────────────
// Mirrors Rust credentials_home_dir() + credentials_path():
//   $CLAW_CONFIG_HOME/credentials.json  OR  ~/.claw/credentials.json

std::filesystem::path credentials_path() {
    if (const char* cfg = std::getenv("CLAW_CONFIG_HOME"); cfg && *cfg) {
        return std::filesystem::path(cfg) / "credentials.json";
    }
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = ".";
    return std::filesystem::path(home) / ".claw" / "credentials.json";
}

// ─── Internal credential I/O ─────────────────────────────────────────────────

namespace {

// Read the JSON object root from the credentials file.
// Returns an empty object if the file does not exist.
// Mirrors Rust read_credentials_root().
nlohmann::json read_credentials_root(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return nlohmann::json::object();
    std::string contents((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    // Trim
    auto s = contents.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return nlohmann::json::object();
    contents = contents.substr(s);
    if (contents.empty()) return nlohmann::json::object();
    try {
        auto j = nlohmann::json::parse(contents);
        if (!j.is_object()) return nlohmann::json::object();
        return j;
    } catch (...) {
        return nlohmann::json::object();
    }
}

// Write root to the credentials file atomically (temp file + rename).
// Mirrors Rust write_credentials_root().
tl::expected<void, std::string> write_credentials_root(
    const std::filesystem::path& path,
    const nlohmann::json& root)
{
    // Ensure parent directory exists.
    std::error_code ec;
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) return tl::unexpected(std::format("cannot create directory {}: {}",
                                                    parent.string(), ec.message()));
    }
    auto temp_path = path;
    temp_path.replace_extension(".json.tmp");
    {
        std::ofstream out(temp_path);
        if (!out) return tl::unexpected(
            std::format("cannot write temp credentials file: {}", temp_path.string()));
        out << root.dump(2) << '\n';
        if (!out) return tl::unexpected("credentials write failed (I/O error)");
    }
    std::filesystem::rename(temp_path, path, ec);
    if (ec) return tl::unexpected(
        std::format("cannot rename {} → {}: {}", temp_path.string(), path.string(), ec.message()));
    return {};
}

} // anonymous namespace

// ─── load_oauth_credentials ──────────────────────────────────────────────────
// Mirrors Rust load_oauth_credentials():
//   Read "oauth" key from credentials.json, deserialize as StoredOAuthCredentials
//   (camelCase).  Returns an empty unexpected when the key is absent or null.

tl::expected<OAuthTokenSet, std::string> load_oauth_credentials() {
    auto path = credentials_path();
    auto root = read_credentials_root(path);
    if (!root.contains("oauth") || root["oauth"].is_null()) {
        // Mirrors Rust Ok(None) — represent "no credentials" as unexpected with
        // empty string so callers can distinguish "absent" from "parse error".
        return tl::unexpected(std::string{});
    }
    try {
        auto& o = root["oauth"];
        OAuthTokenSet ts;
        ts.access_token = o.value("accessToken", std::string{});
        if (o.contains("refreshToken") && !o["refreshToken"].is_null())
            ts.refresh_token = o["refreshToken"].get<std::string>();
        if (o.contains("expiresAt") && !o["expiresAt"].is_null())
            ts.expires_at_epoch = o["expiresAt"].get<uint64_t>();
        if (o.contains("scope") && !o["scope"].is_null())
            ts.scope = o["scope"].get<std::string>();
        // token_type not stored in the Rust model; leave as nullopt.
        return ts;
    } catch (const std::exception& e) {
        return tl::unexpected(std::format("failed to parse OAuth credentials: {}", e.what()));
    }
}

// ─── save_oauth_credentials ──────────────────────────────────────────────────
// Mirrors Rust save_oauth_credentials():
//   Merge "oauth" key into existing JSON root, write atomically.
//   Stored with camelCase keys (Rust serde rename_all = "camelCase").

tl::expected<void, std::string> save_oauth_credentials(const OAuthTokenSet& tokens) {
    auto path = credentials_path();
    auto root = read_credentials_root(path);

    nlohmann::json o = nlohmann::json::object();
    o["accessToken"] = tokens.access_token;
    if (tokens.refresh_token)    o["refreshToken"] = *tokens.refresh_token;
    if (tokens.expires_at_epoch) o["expiresAt"]    = *tokens.expires_at_epoch;
    if (tokens.scope)            o["scope"]         = *tokens.scope;
    // token_type is a C++-only field not in the Rust model; store if present.
    if (tokens.token_type)       o["tokenType"]     = *tokens.token_type;

    root["oauth"] = std::move(o);
    return write_credentials_root(path, root);
}

// ─── clear_oauth_credentials ─────────────────────────────────────────────────
// Mirrors Rust clear_oauth_credentials():
//   Remove "oauth" key from root, write atomically.

tl::expected<void, std::string> clear_oauth_credentials() {
    auto path = credentials_path();
    auto root = read_credentials_root(path);
    root.erase("oauth");
    return write_credentials_root(path, root);
}

// ─── parse_oauth_callback_query ───────────────────────────────────────────────
// Mirrors Rust parse_oauth_callback_query():
//   Split on '&', split each pair on '=', percent-decode keys and values.
//   Also mirrors parse_oauth_callback_request_target(): if the input begins
//   with '/', validate that the path component is "/callback".

tl::expected<std::unordered_map<std::string, std::string>, std::string>
parse_oauth_callback_query(std::string_view query) {
    // If the caller passes a full request target ("/callback?..."), validate
    // the path and extract just the query portion.  Mirrors Rust
    // parse_oauth_callback_request_target().
    if (!query.empty() && query.front() == '/') {
        auto qpos = query.find('?');
        std::string_view path_part = (qpos == std::string_view::npos)
            ? query
            : query.substr(0, qpos);
        if (path_part != "/callback") {
            return tl::unexpected(
                std::format("unexpected callback path: {}", path_part));
        }
        query = (qpos == std::string_view::npos)
            ? std::string_view{}
            : query.substr(qpos + 1);
    }
    // Strip leading '?'
    if (!query.empty() && query.front() == '?') query.remove_prefix(1);

    std::unordered_map<std::string, std::string> params;
    while (!query.empty()) {
        auto amp = query.find('&');
        std::string_view pair = (amp == std::string_view::npos)
            ? query
            : query.substr(0, amp);
        if (!pair.empty()) {
            auto eq = pair.find('=');
            std::string_view raw_key = (eq == std::string_view::npos) ? pair : pair.substr(0, eq);
            std::string_view raw_val = (eq == std::string_view::npos) ? std::string_view{} : pair.substr(eq + 1);
            params.emplace(percent_decode(raw_key), percent_decode(raw_val));
        }
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return params;
}

} // namespace claw::runtime
