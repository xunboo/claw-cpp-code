#pragma once
#include <string>
#include <optional>
#include <map>
#include <vector>

namespace claw::runtime {

struct RemoteSessionContext {
    bool is_remote{false};
    std::optional<std::string> session_id;
    std::optional<std::string> base_url;

    [[nodiscard]] static RemoteSessionContext from_env_map(const std::map<std::string, std::string>& env);
};

struct UpstreamProxyBootstrap {
    std::optional<std::string> session_token_path;  // CCR_SESSION_TOKEN_PATH
    std::optional<std::string> system_ca_bundle;    // CCR_SYSTEM_CA_BUNDLE
    std::optional<std::string> ca_bundle_path;      // CCR_CA_BUNDLE_PATH
    bool enabled{false};                             // CCR_UPSTREAM_PROXY_ENABLED

    [[nodiscard]] static UpstreamProxyBootstrap from_env();
};

struct UpstreamProxyState {
    std::string proxy_url;            // HTTPS_PROXY
    std::optional<std::string> ssl_cert_file;
    std::vector<std::string> no_proxy_hosts;

    // Returns env vars to set on child processes for proxy routing
    [[nodiscard]] std::map<std::string, std::string> subprocess_env() const;
};

// Convert https://... to wss://..., http://... to ws://...
[[nodiscard]] std::string upstream_proxy_ws_url(std::string_view url);

// Default no-proxy host list (internal registries, etc.)
[[nodiscard]] std::vector<std::string> no_proxy_list();

// Read inherited proxy env from parent process (requires HTTPS_PROXY + SSL_CERT_FILE)
[[nodiscard]] std::optional<UpstreamProxyState> inherited_upstream_proxy_env();

} // namespace claw::runtime
