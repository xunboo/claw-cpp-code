#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <vector>
#include <stdexcept>
#include <string>
#include <string_view>

namespace claw::api {

/// Mirrors the Rust ApiError enum.  All variants are represented as a single
/// class with a discriminant tag so callers can switch on Kind.
class ApiError : public std::exception {
public:
    enum class Kind {
        MissingCredentials,
        ContextWindowExceeded,
        ExpiredOAuthToken,
        Auth,
        InvalidApiKeyEnv,
        Http,
        Io,
        Json,
        Api,
        RetriesExhausted,
        InvalidSseFrame,
        BackoffOverflow,
    };

    // Factories
    [[nodiscard]] static ApiError missing_credentials(
        std::string_view provider,
        std::vector<std::string_view> env_vars);
    [[nodiscard]] static ApiError context_window_exceeded(
        std::string model,
        uint32_t estimated_input_tokens,
        uint32_t requested_output_tokens,
        uint32_t estimated_total_tokens,
        uint32_t context_window_tokens);
    [[nodiscard]] static ApiError expired_oauth_token();
    [[nodiscard]] static ApiError auth(std::string message);
    [[nodiscard]] static ApiError invalid_api_key_env(std::string detail);
    [[nodiscard]] static ApiError http(std::string detail,
                                       bool is_connect = false,
                                       bool is_timeout = false);
    [[nodiscard]] static ApiError io(std::string detail);
    [[nodiscard]] static ApiError json(std::string detail);
    [[nodiscard]] static ApiError api(int status,
                                      std::string error_type,
                                      std::string message,
                                      std::string body,
                                      bool retryable,
                                      std::optional<std::string> request_id = std::nullopt);
    [[nodiscard]] static ApiError retries_exhausted(
        uint32_t attempts,
        std::unique_ptr<ApiError> last_error);
    [[nodiscard]] static ApiError invalid_sse_frame(std::string_view reason);
    [[nodiscard]] static ApiError backoff_overflow(
        uint32_t attempt,
        std::chrono::milliseconds base_delay);

    // Queries
    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_retryable() const noexcept;
    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] std::optional<std::string> request_id() const noexcept;
    [[nodiscard]] const char* safe_failure_class() const noexcept;
    [[nodiscard]] bool is_generic_fatal_wrapper() const noexcept;

    // Accessors for specific variant fields
    [[nodiscard]] std::string_view provider() const noexcept { return provider_; }
    [[nodiscard]] const std::vector<std::string>& env_vars() const noexcept { return env_vars_; }
    [[nodiscard]] int http_status() const noexcept { return http_status_; }
    [[nodiscard]] const std::string& error_type() const noexcept { return error_type_; }
    [[nodiscard]] const std::string& body() const noexcept { return body_; }
    [[nodiscard]] uint32_t attempts() const noexcept { return attempts_; }
    [[nodiscard]] const ApiError* last_error() const noexcept { return last_error_.get(); }

    // Copy / move
    ApiError(const ApiError&);
    ApiError& operator=(const ApiError&);
    ApiError(ApiError&&) noexcept = default;
    ApiError& operator=(ApiError&&) noexcept = default;
    ~ApiError() override = default;

private:
    explicit ApiError(Kind kind) : kind_(kind) {}

    Kind kind_;
    std::string message_;
    std::string provider_;
    std::vector<std::string> env_vars_;
    int http_status_{0};
    std::string error_type_;
    std::string body_;
    std::optional<std::string> request_id_;
    std::string model_;
    uint32_t estimated_input_tokens_{0};
    uint32_t requested_output_tokens_{0};
    uint32_t estimated_total_tokens_{0};
    uint32_t context_window_tokens_{0};
    bool retryable_{false};
    bool http_is_connect_{false};
    bool http_is_timeout_{false};
    uint32_t attempts_{0};
    std::unique_ptr<ApiError> last_error_;
    uint32_t backoff_attempt_{0};
    std::chrono::milliseconds base_delay_{0};

    mutable std::string what_cache_;
    void rebuild_what() const;
};

} // namespace claw::api