// ---------------------------------------------------------------------------
// error.cpp
// ---------------------------------------------------------------------------

#include "error.hpp"

#include <format>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>

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
      request_id_(other.request_id_),
      model_(other.model_),
      estimated_input_tokens_(other.estimated_input_tokens_),
      requested_output_tokens_(other.requested_output_tokens_),
      estimated_total_tokens_(other.estimated_total_tokens_),
      context_window_tokens_(other.context_window_tokens_),
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

ApiError ApiError::context_window_exceeded(
        std::string model,
        uint32_t estimated_input_tokens,
        uint32_t requested_output_tokens,
        uint32_t estimated_total_tokens,
        uint32_t context_window_tokens) {
    ApiError e(Kind::ContextWindowExceeded);
    e.model_ = std::move(model);
    e.estimated_input_tokens_ = estimated_input_tokens;
    e.requested_output_tokens_ = requested_output_tokens;
    e.estimated_total_tokens_ = estimated_total_tokens;
    e.context_window_tokens_ = context_window_tokens;
    return e;
}

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
                        std::string body, bool retryable, std::optional<std::string> request_id) {
    ApiError e(Kind::Api);
    e.http_status_ = status;
    e.error_type_  = std::move(error_type);
    e.message_     = std::move(message);
    e.body_        = std::move(body);
    e.retryable_   = retryable;
    e.request_id_  = std::move(request_id);
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

// ── Additional Methods ────────────────────────────────────────────────────────

std::optional<std::string> ApiError::request_id() const noexcept {
    switch (kind_) {
        case Kind::Api:
            return request_id_;
        case Kind::RetriesExhausted:
            return last_error_ ? last_error_->request_id() : std::nullopt;
        default:
            return std::nullopt;
    }
}

const char* ApiError::safe_failure_class() const noexcept {
    switch(kind_) {
        case Kind::RetriesExhausted: return "provider_retry_exhausted";
        case Kind::MissingCredentials:
        case Kind::ExpiredOAuthToken:
        case Kind::Auth:
            return "provider_auth";
        case Kind::Api:
            if (http_status_ == 401 || http_status_ == 403) return "provider_auth";
            if (http_status_ == 429) return "provider_rate_limit";
            if (is_generic_fatal_wrapper()) return "provider_internal";
            return "provider_error";
        case Kind::ContextWindowExceeded: return "context_window";
        case Kind::Http:
        case Kind::InvalidSseFrame:
        case Kind::BackoffOverflow:
            return "provider_transport";
        case Kind::InvalidApiKeyEnv:
        case Kind::Io:
        case Kind::Json:
            return "runtime_io";
    }
    return "unknown"; // should be unreachable
}

namespace {
    bool looks_like_generic_fatal_wrapper(std::string_view text) {
        if (text.empty()) return false;
        std::string lowered(text);
        for (auto& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lowered.find("something went wrong while processing your request") != std::string::npos ||
            lowered.find("please try again, or use /new to start a fresh session") != std::string::npos) {
            return true;
        }
        return false;
    }
}

bool ApiError::is_generic_fatal_wrapper() const noexcept {
    switch (kind_) {
        case Kind::Api:
            if (!message_.empty() && looks_like_generic_fatal_wrapper(message_)) return true;
            if (!body_.empty() && looks_like_generic_fatal_wrapper(body_)) return true;
            return false;
        case Kind::RetriesExhausted:
            return last_error_ && last_error_->is_generic_fatal_wrapper();
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
        case Kind::ContextWindowExceeded:
            oss << "context_window_blocked for " << model_ << ": estimated input "
                << estimated_input_tokens_ << " + requested output " << requested_output_tokens_
                << " = " << estimated_total_tokens_ << " tokens exceeds the "
                << context_window_tokens_ << "-token context window; compact the session or reduce request size before retrying";
            break;
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
            if (!error_type_.empty() && !message_.empty()) {
                oss << "api returned " << http_status_ << " (" << error_type_ << ")";
                if (request_id_) oss << " [trace " << *request_id_ << "]";
                oss << ": " << message_;
            } else {
                oss << "api returned " << http_status_;
                if (request_id_) oss << " [trace " << *request_id_ << "]";
                oss << ": " << body_;
            }
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