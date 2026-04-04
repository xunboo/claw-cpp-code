#include "remote.hpp"
#include <format>
#include <algorithm>

namespace claw::runtime {

RemoteSessionContext RemoteSessionContext::from_env_map(const std::map<std::string, std::string>& env) {
    RemoteSessionContext ctx;
    auto it = env.find("CLAUDE_CODE_REMOTE");
    if (it != env.end() && (it->second == "1" || it->second == "true")) {
        ctx.is_remote = true;
    }
    auto sid = env.find("SESSION_ID");
    if (sid != env.end() && !sid->second.empty()) ctx.session_id = sid->second;
    auto url = env.find("ANTHROPIC_BASE_URL");
    if (url != env.end() && !url->second.empty()) ctx.base_url = url->second;
    return ctx;
}

UpstreamProxyBootstrap UpstreamProxyBootstrap::from_env() {
    UpstreamProxyBootstrap b;
    if (const char* v = std::getenv("CCR_SESSION_TOKEN_PATH"); v && *v) b.session_token_path = v;
    if (const char* v = std::getenv("CCR_SYSTEM_CA_BUNDLE"); v && *v) b.system_ca_bundle = v;
    if (const char* v = std::getenv("CCR_CA_BUNDLE_PATH"); v && *v) b.ca_bundle_path = v;
    if (const char* v = std::getenv("CCR_UPSTREAM_PROXY_ENABLED"); v) {
        b.enabled = (std::string_view(v) == "1" || std::string_view(v) == "true");
    }
    return b;
}

std::map<std::string, std::string> UpstreamProxyState::subprocess_env() const {
    std::map<std::string, std::string> env;
    env["HTTPS_PROXY"] = proxy_url;
    env["https_proxy"] = proxy_url;

    // Build NO_PROXY list
    std::string no_proxy_str;
    for (const auto& host : no_proxy_hosts) {
        if (!no_proxy_str.empty()) no_proxy_str += ',';
        no_proxy_str += host;
    }
    env["NO_PROXY"] = no_proxy_str;
    env["no_proxy"] = no_proxy_str;

    if (ssl_cert_file.has_value()) {
        env["SSL_CERT_FILE"] = *ssl_cert_file;
        env["NODE_EXTRA_CA_CERTS"] = *ssl_cert_file;
        env["REQUESTS_CA_BUNDLE"] = *ssl_cert_file;
        env["CURL_CA_BUNDLE"] = *ssl_cert_file;
    }
    return env;
}

std::string upstream_proxy_ws_url(std::string_view url) {
    if (url.starts_with("https://")) {
        return std::string("wss://") + std::string(url.substr(8));
    }
    if (url.starts_with("http://")) {
        return std::string("ws://") + std::string(url.substr(7));
    }
    return std::string(url);
}

std::vector<std::string> no_proxy_list() {
    return {
        "localhost",
        "127.0.0.1",
        "::1",
        "169.254.0.0/16",
        "*.local",
        "*.internal",
        "*.cluster.local",
        "*.svc.cluster.local",
        "10.0.0.0/8",
        "172.16.0.0/12",
        "192.168.0.0/16",
        "pypi.org",
        "files.pythonhosted.org",
        "pypi.python.org",
        "golang.org",
        "proxy.golang.org",
        "sum.golang.org",
    };
}

std::optional<UpstreamProxyState> inherited_upstream_proxy_env() {
    const char* proxy = std::getenv("HTTPS_PROXY");
    const char* cert  = std::getenv("SSL_CERT_FILE");
    if (!proxy || !*proxy || !cert || !*cert) return std::nullopt;

    UpstreamProxyState state;
    state.proxy_url = proxy;
    state.ssl_cert_file = cert;
    state.no_proxy_hosts = no_proxy_list();
    return state;
}

} // namespace claw::runtime
