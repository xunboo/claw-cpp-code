// ---------------------------------------------------------------------------
// error.cpp
// ---------------------------------------------------------------------------

#include "error.hpp"

#include <format>
#include <sstream>

namespace claw::api {

// ── Copy ─────────────────────────────────────────────────────────────────────

ApiError::ApiError(const ApiError& other)
    : kind_(other.kind_),
      message_(other.message_),
      provider_(other.provider_),
      env_vars_(other.env_vars_),
      http_status_(other.http_status_),
      error_type_(other.error_type_),
      body_(other.body_),
      retryable_(other.retryable_),
      http_is_connect_(other.http_is_connect_),
      http_is_timeout_(other.http_is_timeout_),
      attempts_(other.attempts_),
      last_error_(other.last_error_
                      ? std::make_unique<ApiError>(*other.last_error_)
                      : nullptr),
      backoff_attempt_(other.backoff_attempt_),
      base_delay_(other.base_delay_) {}

ApiError& ApiError::operator=(const ApiError& other) {
    if (this != &other) {
        ApiError tmp(other);
        *this = std::move(tmp);
    }
    return *this;
}

// ── Factories ─────────────────────────────────────────────────────────────────

ApiError ApiError::missing_credentials(std::string_view provider,
                                       std::vector<std::string_view> env_vars) {
    ApiError e(Kind::MissingCredentials);
    e.provider_ = std::string(provider);
    for (auto& v : env_vars) e.env_vars_.emplace_back(v);
    return e;
}

ApiError ApiError::expired_oauth_token() {
    return ApiError(Kind::ExpiredOAuthToken);
}

ApiError ApiError::auth(std::string message) {
    ApiError e(Kind::Auth);
    e.message_ = std::move(message);
    return e;
}

ApiError ApiError::invalid_api_key_env(std::string detail) {
    ApiError e(Kind::InvalidApiKeyEnv);
    e.message_ = std::move(detail);
    return e;
}

ApiError ApiError::http(std::string detail, bool is_connect, bool is_timeout) {
    ApiError e(Kind::Http);
    e.message_         = std::move(detail);
    e.http_is_connect_ = is_connect;
    e.http_is_timeout_ = is_timeout;
    e.retryable_       = is_connect || is_timeout;
    return e;
}

ApiError ApiError::io(std::string detail) {
    ApiError e(Kind::Io);
    e.message_ = std::move(detail);
    return e;
}

ApiError ApiError::json(std::string detail) {
    ApiError e(Kind::Json);
    e.message_ = std::move(detail);
    return e;
}

ApiError ApiError::api(int status, std::string error_type, std::string message,
                        std::string body, bool retryable) {
    ApiError e(Kind::Api);
    e.http_status_ = status;
    e.error_type_  = std::move(error_type);
    e.message_     = std::move(message);
    e.body_        = std::move(body);
    e.retryable_   = retryable;
    return e;
}

ApiError ApiError::retries_exhausted(uint32_t attempts,
                                      std::unique_ptr<ApiError> last_error) {
    ApiError e(Kind::RetriesExhausted);
    e.attempts_   = attempts;
    e.last_error_ = std::move(last_error);
    return e;
}

ApiError ApiError::invalid_sse_frame(std::string_view reason) {
    ApiError e(Kind::InvalidSseFrame);
    e.message_ = std::string(reason);
    return e;
}

ApiError ApiError::backoff_overflow(uint32_t attempt,
                                     std::chrono::milliseconds base_delay) {
    ApiError e(Kind::BackoffOverflow);
    e.backoff_attempt_ = attempt;
    e.base_delay_      = base_delay;
    return e;
}

// ── is_retryable ─────────────────────────────────────────────────────────────

bool ApiError::is_retryable() const noexcept {
    switch (kind_) {
        case Kind::Http:
            return http_is_connect_ || http_is_timeout_;
        case Kind::Api:
            return retryable_;
        case Kind::RetriesExhausted:
            return last_error_ && last_error_->is_retryable();
        default:
            return false;
    }
}

// ── what() ───────────────────────────────────────────────────────────────────

void ApiError::rebuild_what() const {
    std::ostringstream oss;
    switch (kind_) {
        case Kind::MissingCredentials: {
            std::string joined;
            for (size_t i = 0; i < env_vars_.size(); ++i) {
                if (i > 0) joined += " or ";
                joined += env_vars_[i];
            }
            oss << "missing " << provider_ << " credentials; export " << joined
                << " before calling the " << provider_ << " API";
            break;
        }
        case Kind::ExpiredOAuthToken:
            oss << "saved OAuth token is expired and no refresh token is available";
            break;
        case Kind::Auth:
            oss << "auth error: " << message_;
            break;
        case Kind::InvalidApiKeyEnv:
            oss << "failed to read credential environment variable: " << message_;
            break;
        case Kind::Http:
            oss << "http error: " << message_;
            break;
        case Kind::Io:
            oss << "io error: " << message_;
            break;
        case Kind::Json:
            oss << "json error: " << message_;
            break;
        case Kind::Api:
            if (!error_type_.empty() && !message_.empty())
                oss << "api returned " << http_status_ << " (" << error_type_ << "): " << message_;
            else
                oss << "api returned " << http_status_ << ": " << body_;
            break;
        case Kind::RetriesExhausted:
            oss << "api failed after " << attempts_ << " attempts: "
                << (last_error_ ? last_error_->what() : "(unknown)");
            break;
        case Kind::InvalidSseFrame:
            oss << "invalid sse frame: " << message_;
            break;
        case Kind::BackoffOverflow:
            oss << "retry backoff overflowed on attempt " << backoff_attempt_
                << " with base delay " << base_delay_.count() << "ms";
            break;
    }
    what_cache_ = oss.str();
}

const char* ApiError::what() const noexcept {
    rebuild_what();
    return what_cache_.c_str();
}

} // namespace claw::api