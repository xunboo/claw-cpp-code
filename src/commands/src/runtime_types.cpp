// runtime_types.cpp — implementations for the runtime types declared in
// commands/include/runtime_types.hpp. These are simplified stubs that allow
// the commands module to compile and link standalone.

#include "runtime_types.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace runtime {

// ── Session ─────────────────────────────────────────────────────────────────

Session Session::new_session() {
    Session s;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    s.created_at_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    s.updated_at_ms = s.created_at_ms;
    s.session_id = "session_" + std::to_string(s.created_at_ms);
    return s;
}

// ── compact_session ─────────────────────────────────────────────────────────

CompactionResult compact_session(const Session& session, CompactionConfig config) {
    CompactionResult result;
    if (session.messages.size() <= config.preserve_recent_messages) {
        result.compacted_session = session;
        result.removed_message_count = 0;
        result.summary = "No compaction needed.";
        result.formatted_summary = result.summary;
        return result;
    }

    // Keep only the last N messages
    Session compacted = session;
    std::size_t to_remove = compacted.messages.size() - config.preserve_recent_messages;
    compacted.messages.erase(compacted.messages.begin(),
                              compacted.messages.begin() + static_cast<std::ptrdiff_t>(to_remove));

    result.compacted_session = std::move(compacted);
    result.removed_message_count = to_remove;
    result.summary = "Compacted " + std::to_string(to_remove) + " messages.";
    result.formatted_summary = result.summary;
    return result;
}

// ── ConfigLoader ────────────────────────────────────────────────────────────

ConfigLoader ConfigLoader::default_for(std::filesystem::path cwd) {
    // Resolve config home from env or default
    std::filesystem::path config_home;
    if (auto* h = std::getenv("CLAW_CONFIG_HOME")) {
        config_home = h;
    } else if (auto* h2 = std::getenv("CODEX_HOME")) {
        config_home = h2;
    } else {
#ifdef _WIN32
        if (auto* appdata = std::getenv("APPDATA"))
            config_home = std::filesystem::path(appdata) / "claw";
        else
            config_home = cwd / ".claw";
#else
        if (auto* home = std::getenv("HOME"))
            config_home = std::filesystem::path(home) / ".config" / "claw";
        else
            config_home = cwd / ".claw";
#endif
    }
    return ConfigLoader{std::move(cwd), std::move(config_home)};
}

RuntimeConfig ConfigLoader::load() const {
    RuntimeConfig config;
    // Try to load MCP config from .claw.json or .claude.json
    for (auto name : {".claw.json", ".claude.json"}) {
        auto path = cwd_ / name;
        if (!std::filesystem::exists(path)) continue;
        try {
            // We don't have nlohmann::json here, so just return empty config
            // The real MCP loading is in the runtime module
            break;
        } catch (...) {}
    }
    return config;
}

} // namespace runtime
