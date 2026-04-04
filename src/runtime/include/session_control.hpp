#pragma once
#include "session.hpp"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <tl/expected.hpp>
#include <variant>
#include <cstdint>

namespace claw::runtime {

inline constexpr std::string_view PRIMARY_SESSION_EXTENSION = "jsonl";
inline constexpr std::string_view LEGACY_SESSION_EXTENSION  = "json";
inline constexpr std::string_view SESSION_REFERENCE_ALIASES[] = {"latest", "last", "recent"};

struct SessionHandle {
    std::string id;
    std::filesystem::path path;
};

struct ManagedSessionSummary {
    std::string id;
    std::filesystem::path path;
    uint64_t modified_epoch_millis{0};
    std::size_t message_count{0};
    std::optional<std::string> parent_session_id;
    std::optional<std::string> branch_name;
};

struct LoadedManagedSession {
    ManagedSessionSummary summary;
    Session session;
};

struct ForkedManagedSession {
    SessionHandle handle;
    Session forked;
};

struct SessionControlErrorIo      { std::string message; };
struct SessionControlErrorSession { std::string message; };
struct SessionControlErrorFormat  { std::string message; };

using SessionControlError = std::variant<SessionControlErrorIo, SessionControlErrorSession, SessionControlErrorFormat>;

[[nodiscard]] std::string session_control_error_message(const SessionControlError& e);

// Base directory for managed sessions: .claw/sessions/
[[nodiscard]] std::filesystem::path sessions_dir(const std::filesystem::path& project_root);

// Create session directory for a given root
[[nodiscard]] tl::expected<std::filesystem::path, SessionControlError>
    managed_sessions_dir_for(const std::filesystem::path& project_root);

// Resolve "latest"/"last"/"recent" alias or session ID or absolute path
[[nodiscard]] tl::expected<SessionHandle, SessionControlError>
    resolve_session_reference_for(const std::filesystem::path& project_root, std::string_view reference);

// List all managed sessions, sorted by modified desc then id desc
[[nodiscard]] tl::expected<std::vector<ManagedSessionSummary>, SessionControlError>
    list_managed_sessions_for(const std::filesystem::path& project_root);

// Fork a session from the given reference
[[nodiscard]] tl::expected<ForkedManagedSession, SessionControlError>
    fork_managed_session_for(const std::filesystem::path& project_root,
                             std::string_view reference,
                             std::string new_id);

} // namespace claw::runtime
