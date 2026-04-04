#pragma once
#include <string>
#include <optional>
#include <filesystem>
#include <tl/expected.hpp>
#include <unordered_map>

namespace claw::runtime {

struct OAuthTokenSet {
    std::string access_token;
    std::optional<std::string> refresh_token;
    std::optional<std::string> token_type;
    std::optional<uint64_t> expires_at_epoch; // Unix timestamp
    std::optional<std::string> scope;
};

enum class PkceChallengeMethod { S256 };

struct PkceCodePair {
    std::string code_verifier;   // raw 32-byte base64url
    std::string code_challenge;  // SHA256 of verifier, base64url
    PkceChallengeMethod method{PkceChallengeMethod::S256};
};

struct OAuthAuthorizationRequest {
    std::string authorization_endpoint;
    std::string client_id;
    std::string redirect_uri;
    std::string scope;
    std::string state;
    PkceCodePair pkce;

    [[nodiscard]] std::string build_url() const;
};

struct OAuthTokenExchangeRequest {
    std::string token_endpoint;
    std::string client_id;
    std::string code;
    std::string redirect_uri;
    std::string code_verifier;

    [[nodiscard]] std::string form_params() const;
};

struct OAuthRefreshRequest {
    std::string token_endpoint;
    std::string client_id;
    std::string refresh_token;

    [[nodiscard]] std::string form_params() const;
};

// PKCE generation (reads from /dev/urandom on POSIX)
[[nodiscard]] tl::expected<PkceCodePair, std::string> generate_pkce_pair();

// Generate a random state string
[[nodiscard]] std::string generate_state();

// SHA256 then base64url-encode (for PKCE S256)
[[nodiscard]] std::string code_challenge_s256(std::string_view verifier);

// base64url without padding
[[nodiscard]] std::string base64url_encode(const uint8_t* data, std::size_t len);

// Loopback redirect URI (e.g. http://127.0.0.1:PORT/callback)
[[nodiscard]] std::string loopback_redirect_uri(uint16_t port);

// Credentials file path: $CLAW_CONFIG_HOME/credentials.json or ~/.claw/credentials.json
[[nodiscard]] std::filesystem::path credentials_path();

// Load/save/clear the "oauth" key in credentials.json
[[nodiscard]] tl::expected<OAuthTokenSet, std::string> load_oauth_credentials();
[[nodiscard]] tl::expected<void, std::string> save_oauth_credentials(const OAuthTokenSet& tokens);
[[nodiscard]] tl::expected<void, std::string> clear_oauth_credentials();

// Parse OAuth callback query string (expects path=/callback)
[[nodiscard]] tl::expected<std::unordered_map<std::string,std::string>, std::string>
    parse_oauth_callback_query(std::string_view query);

} // namespace claw::runtime

// Include span support
#include <span>
