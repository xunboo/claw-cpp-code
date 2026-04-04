#include "session_control.hpp"
#include <algorithm>
#include <format>
#include <fstream>
#include <chrono>

namespace claw::runtime {

namespace fs = std::filesystem;

std::string session_control_error_message(const SessionControlError& e) {
    return std::visit([](const auto& v) { return v.message; }, e);
}

fs::path sessions_dir(const fs::path& project_root) {
    return project_root / ".claw" / "sessions";
}

tl::expected<fs::path, SessionControlError>
managed_sessions_dir_for(const fs::path& project_root) {
    auto dir = sessions_dir(project_root);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return tl::unexpected(SessionControlErrorIo{std::format("cannot create sessions dir: {}", ec.message())});
    return dir;
}

tl::expected<std::vector<ManagedSessionSummary>, SessionControlError>
list_managed_sessions_for(const fs::path& project_root) {
    auto dir_r = managed_sessions_dir_for(project_root);
    if (!dir_r) return tl::unexpected(dir_r.error());
    auto dir = *dir_r;

    std::vector<ManagedSessionSummary> summaries;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        auto ext = entry.path().extension().string();
        if (ext != std::string(".") + std::string(PRIMARY_SESSION_EXTENSION) &&
            ext != std::string(".") + std::string(LEGACY_SESSION_EXTENSION)) continue;

        ManagedSessionSummary summary;
        summary.id = entry.path().stem().string();
        summary.path = entry.path();

        auto lwt = entry.last_write_time(ec);
        if (!ec) {
#if defined(_MSC_VER)
            // MSVC C++20: file_clock::to_sys not available; use raw ticks
            // file_time_type epoch is Jan 1 1601 on Windows
            auto ft_dur = lwt.time_since_epoch();
            // Convert to system_clock epoch (Jan 1 1970 = 11644473600 seconds after 1601)
            constexpr auto epoch_diff = std::chrono::seconds(11644473600LL);
            auto sys_dur = std::chrono::duration_cast<std::chrono::milliseconds>(ft_dur) -
                           std::chrono::duration_cast<std::chrono::milliseconds>(epoch_diff);
            summary.modified_epoch_millis = static_cast<uint64_t>(sys_dur.count());
#else
            auto sctp = std::chrono::file_clock::to_sys(lwt);
            summary.modified_epoch_millis = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    sctp.time_since_epoch()).count());
#endif
        }

        // Load session briefly for metadata
        auto session_r = Session::load(entry.path());
        if (session_r) {
            summary.message_count = session_r->messages.size();
            summary.parent_session_id = session_r->parent_session_id;
            summary.branch_name = session_r->branch_name;
        }
        summaries.push_back(std::move(summary));
    }

    // Sort: modified desc, then id desc
    std::sort(summaries.begin(), summaries.end(), [](const ManagedSessionSummary& a, const ManagedSessionSummary& b) {
        if (a.modified_epoch_millis != b.modified_epoch_millis) {
            return a.modified_epoch_millis > b.modified_epoch_millis;
        }
        return a.id > b.id;
    });

    return summaries;
}

tl::expected<SessionHandle, SessionControlError>
resolve_session_reference_for(const fs::path& project_root, std::string_view reference) {
    // Check aliases
    bool is_alias = false;
    for (auto alias : SESSION_REFERENCE_ALIASES) {
        if (reference == alias) { is_alias = true; break; }
    }

    if (is_alias) {
        auto list_r = list_managed_sessions_for(project_root);
        if (!list_r) return tl::unexpected(list_r.error());
        if (list_r->empty()) return tl::unexpected(SessionControlErrorSession{"no sessions found"});
        auto& first = list_r->front();
        return SessionHandle{first.id, first.path};
    }

    // Absolute path?
    fs::path ref_path(reference);
    if (ref_path.is_absolute() && fs::exists(ref_path)) {
        return SessionHandle{ref_path.stem().string(), ref_path};
    }

    // Look up by ID in sessions dir
    auto dir_r = managed_sessions_dir_for(project_root);
    if (!dir_r) return tl::unexpected(dir_r.error());

    for (auto ext : {PRIMARY_SESSION_EXTENSION, LEGACY_SESSION_EXTENSION}) {
        fs::path candidate = *dir_r / (std::string(reference) + "." + std::string(ext));
        if (fs::exists(candidate)) {
            return SessionHandle{std::string(reference), candidate};
        }
    }

    return tl::unexpected(SessionControlErrorSession{std::format("session not found: {}", reference)});
}

tl::expected<ForkedManagedSession, SessionControlError>
fork_managed_session_for(const fs::path& project_root, std::string_view reference, std::string new_id) {
    auto handle_r = resolve_session_reference_for(project_root, reference);
    if (!handle_r) return tl::unexpected(handle_r.error());

    auto session_r = Session::load(handle_r->path);
    if (!session_r) return tl::unexpected(SessionControlErrorSession{session_r.error()});

    Session forked = session_r->fork(new_id);

    auto dir_r = managed_sessions_dir_for(project_root);
    if (!dir_r) return tl::unexpected(dir_r.error());

    fs::path new_path = *dir_r / (new_id + "." + std::string(PRIMARY_SESSION_EXTENSION));
    SessionHandle new_handle{new_id, new_path};

    auto persist_r = forked.persist(new_path);
    if (!persist_r) return tl::unexpected(SessionControlErrorIo{persist_r.error()});

    return ForkedManagedSession{std::move(new_handle), std::move(forked)};
}

} // namespace claw::runtime
