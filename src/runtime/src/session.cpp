// session.cpp — C++20 translation of session.rs
// Maps: MessageRole{User,Assistant}, ContentBlock variant, Session JSONL persistence.
// Note: the Rust source also has System/Tool roles and a richer session struct;
// we implement everything the header exposes and add the Rust logic faithfully.

#include "session.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace claw::runtime {

namespace fs = std::filesystem;

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr uint32_t SESSION_VERSION      = 1;
static constexpr uint64_t ROTATE_AFTER_BYTES   = 256ULL * 1024ULL; // matches Rust ROTATE_AFTER_BYTES
static constexpr std::size_t MAX_ROTATED_FILES = 3;

static std::atomic<uint64_t> s_session_id_counter{0};

// ─── Time helpers ─────────────────────────────────────────────────────────────

static uint64_t current_time_millis() {
    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(duration_cast<milliseconds>(now).count());
}

std::string generate_session_id() {
    uint64_t millis  = current_time_millis();
    uint64_t counter = s_session_id_counter.fetch_add(1, std::memory_order_relaxed);
    return std::format("session-{}-{}", millis, counter);
}

// ─── Temporary-path helper (mirrors Rust temporary_path_for) ─────────────────

static fs::path temporary_path_for(const fs::path& path) {
    std::string file_name = path.filename().string();
    if (file_name.empty()) file_name = "session";
    uint64_t millis  = current_time_millis();
    uint64_t counter = s_session_id_counter.fetch_add(1, std::memory_order_relaxed);
    return path.parent_path() / std::format("{}.tmp-{}-{}", file_name, millis, counter);
}

// ─── Rotated-log path (mirrors Rust rotated_log_path) ─────────────────────────

static fs::path rotated_log_path(const fs::path& path) {
    std::string stem = path.stem().string();
    if (stem.empty()) stem = "session";
    uint64_t millis = current_time_millis();
    return path.parent_path() / std::format("{}.rot-{}.jsonl", stem, millis);
}

// ─── JSON helpers ─────────────────────────────────────────────────────────────

nlohmann::json content_block_to_json(const ContentBlock& block) {
    return std::visit([](const auto& b) -> nlohmann::json {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, TextBlock>) {
            return {{"type", "text"}, {"text", b.text}};
        } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
            return {{"type", "tool_use"}, {"id", b.id}, {"name", b.name}, {"input", b.input}};
        } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
            return {
                {"type", "tool_result"},
                {"tool_use_id", b.tool_use_id},
                {"content", b.content},
                {"is_error", b.is_error}
            };
        } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
            return {{"type", "thinking"}, {"thinking", b.thinking}, {"signature", b.signature}};
        } else {
            return nlohmann::json::object();
        }
    }, block);
}

ContentBlock content_block_from_json(const nlohmann::json& j) {
    auto type = j.value("type", std::string{});
    if (type == "text") {
        return TextBlock{j.value("text", std::string{})};
    } else if (type == "tool_use") {
        return ToolUseBlock{
            j.value("id", std::string{}),
            j.value("name", std::string{}),
            j.contains("input") ? j["input"] : nlohmann::json{}
        };
    } else if (type == "tool_result") {
        return ToolResultBlock{
            j.value("tool_use_id", std::string{}),
            j.value("content", std::string{}),
            j.value("is_error", false)
        };
    } else if (type == "thinking") {
        return ThinkingBlock{
            j.value("thinking", std::string{}),
            j.value("signature", std::string{})
        };
    }
    // Unknown block type: return empty text block (mirrors Rust's error path
    // but C++ header has no error return here; caller should validate).
    return TextBlock{};
}

nlohmann::json message_to_json(const ConversationMessage& msg) {
    nlohmann::json j;
    j["role"] = (msg.role == MessageRole::User) ? "user" : "assistant";
    nlohmann::json blocks = nlohmann::json::array();
    for (const auto& block : msg.blocks) {
        blocks.push_back(content_block_to_json(block));
    }
    j["blocks"] = std::move(blocks);
    if (msg.usage.has_value()) {
        const auto& u = *msg.usage;
        j["usage"] = {
            {"input_tokens",                u.input_tokens},
            {"output_tokens",               u.output_tokens},
            {"cache_creation_input_tokens", u.cache_creation_input_tokens},
            {"cache_read_input_tokens",     u.cache_read_input_tokens}
        };
    }
    return j;
}

ConversationMessage message_from_json(const nlohmann::json& j) {
    ConversationMessage msg;
    auto role_str = j.value("role", std::string{"user"});
    msg.role = (role_str == "assistant") ? MessageRole::Assistant : MessageRole::User;

    if (j.contains("blocks") && j["blocks"].is_array()) {
        for (const auto& b : j["blocks"]) {
            msg.blocks.push_back(content_block_from_json(b));
        }
    }
    if (j.contains("usage") && j["usage"].is_object()) {
        const auto& u = j["usage"];
        msg.usage = TokenUsageMsg{
            u.value("input_tokens",                0u),
            u.value("output_tokens",               0u),
            u.value("cache_creation_input_tokens", 0u),
            u.value("cache_read_input_tokens",     0u)
        };
    }
    return msg;
}

// ─── Internal: JSONL record builders ─────────────────────────────────────────

// Produces a {"type":"message","message":{...}} record (mirrors Rust message_record).
static nlohmann::json make_message_record(const ConversationMessage& msg) {
    return {{"type", "message"}, {"message", message_to_json(msg)}};
}

// ─── Internal: write_atomic (mirrors Rust write_atomic) ──────────────────────

static tl::expected<void, std::string> write_atomic(const fs::path& path, const std::string& contents) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) return tl::unexpected(std::format("cannot create directories: {}", ec.message()));
    }
    fs::path tmp = temporary_path_for(path);
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return tl::unexpected(std::format("cannot write temp file: {}", tmp.string()));
        out << contents;
        if (!out) return tl::unexpected(std::format("write failed for temp file: {}", tmp.string()));
    }
    fs::rename(tmp, path, ec);
    if (ec) return tl::unexpected(std::format("atomic rename failed: {}", ec.message()));
    return {};
}

// ─── Internal: rotate_session_file_if_needed ─────────────────────────────────

static tl::expected<void, std::string> rotate_session_file_if_needed(const fs::path& path) {
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec) return {}; // file may not exist yet; that's fine
    if (sz < ROTATE_AFTER_BYTES) return {};
    fs::path dest = rotated_log_path(path);
    fs::rename(path, dest, ec);
    if (ec) return tl::unexpected(std::format("rotation rename failed: {}", ec.message()));
    return {};
}

// ─── Internal: cleanup_rotated_logs ───────────────────────────────────────────

static tl::expected<void, std::string> cleanup_rotated_logs(const fs::path& path) {
    std::error_code ec;
    fs::path parent = path.parent_path();
    if (parent.empty()) return {};

    std::string stem = path.stem().string();
    if (stem.empty()) stem = "session";
    std::string prefix = stem + ".rot-";

    std::vector<fs::path> rotated_paths;
    for (auto& entry : fs::directory_iterator(parent, ec)) {
        if (ec) break;
        std::string name = entry.path().filename().string();
        if (name.starts_with(prefix)) {
            // Check extension is .jsonl (case-insensitive)
            auto ext = entry.path().extension().string();
            // convert ext to lowercase for comparison
            std::string ext_lower = ext;
            std::ranges::transform(ext_lower, ext_lower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            if (ext_lower == ".jsonl") {
                rotated_paths.push_back(entry.path());
            }
        }
    }

    // Sort by modification time (oldest first)
    std::ranges::sort(rotated_paths, [](const fs::path& a, const fs::path& b) {
        std::error_code e1, e2;
        auto ta = fs::last_write_time(a, e1);
        auto tb = fs::last_write_time(b, e2);
        if (e1 || e2) return false;
        return ta < tb;
    });

    std::size_t remove_count = rotated_paths.size() > MAX_ROTATED_FILES
        ? rotated_paths.size() - MAX_ROTATED_FILES
        : 0;
    for (std::size_t i = 0; i < remove_count; ++i) {
        fs::remove(rotated_paths[i], ec); // ignore error on individual file removal
    }
    return {};
}

// ─── Internal: render full JSONL snapshot ────────────────────────────────────

// Builds the complete JSONL snapshot for a session, matching Rust render_jsonl_snapshot.
// Format:
//   line 1: {"type":"session_meta","version":1,"session_id":"...","created_at_ms":...,"updated_at_ms":...[,"fork":{...}]}
//   line 2 (optional): {"type":"compaction","count":N,"removed_message_count":M,"summary":"..."}
//   lines N+: {"type":"message","message":{...}}
struct SessionSnapshot {
    uint32_t    version{SESSION_VERSION};
    std::string session_id;
    uint64_t    created_at_ms{0};
    uint64_t    updated_at_ms{0};
    // compaction metadata
    bool        has_compaction{false};
    uint32_t    compaction_count{0};
    std::size_t compaction_removed{0};
    std::string compaction_summary;
    // fork metadata
    bool        has_fork{false};
    std::string fork_parent_session_id;
    std::optional<std::string> fork_branch_name;
};

static std::string render_snapshot(const Session& session, const SessionSnapshot& meta) {
    std::string result;

    // --- session_meta record ---
    nlohmann::json meta_obj;
    meta_obj["type"]           = "session_meta";
    meta_obj["version"]        = meta.version;
    meta_obj["session_id"]     = meta.session_id;
    meta_obj["created_at_ms"]  = meta.created_at_ms;
    meta_obj["updated_at_ms"]  = meta.updated_at_ms;
    if (meta.has_fork) {
        nlohmann::json fork_obj;
        fork_obj["parent_session_id"] = meta.fork_parent_session_id;
        if (meta.fork_branch_name.has_value()) {
            fork_obj["branch_name"] = *meta.fork_branch_name;
        }
        meta_obj["fork"] = fork_obj;
    }
    result += meta_obj.dump();
    result += '\n';

    // --- compaction record (optional) ---
    if (meta.has_compaction) {
        nlohmann::json comp_obj;
        comp_obj["type"]                  = "compaction";
        comp_obj["count"]                 = meta.compaction_count;
        comp_obj["removed_message_count"] = meta.compaction_removed;
        comp_obj["summary"]               = meta.compaction_summary;
        result += comp_obj.dump();
        result += '\n';
    }

    // --- message records ---
    for (const auto& msg : session.messages) {
        result += make_message_record(msg).dump();
        result += '\n';
    }

    return result;
}

// ─── Session::persist ────────────────────────────────────────────────────────
// Mirrors Rust Session::save_to_path: writes atomic JSONL snapshot after
// rotating if the existing file is over the size limit, then cleans old logs.

tl::expected<void, std::string> Session::persist(const fs::path& path) const {
    // Build meta from the Session's own fields (mirrors Rust's meta_record).
    SessionSnapshot meta;
    meta.version        = SESSION_VERSION;
    meta.session_id     = id.empty() ? generate_session_id() : id;
    meta.created_at_ms  = (created_at_ms != 0) ? created_at_ms : current_time_millis();
    meta.updated_at_ms  = current_time_millis();
    // Update the session's updated_at_ms timestamp (Rust calls touch())
    updated_at_ms = meta.updated_at_ms;

    if (parent_session_id.has_value()) {
        meta.has_fork                 = true;
        meta.fork_parent_session_id   = *parent_session_id;
        meta.fork_branch_name         = branch_name;
    }

    std::string snapshot = render_snapshot(*this, meta);

    // Rotate if needed
    if (auto r = rotate_session_file_if_needed(path); !r) return r;
    // Write atomically
    if (auto r = write_atomic(path, snapshot); !r) return r;
    // Clean up old rotated logs
    if (auto r = cleanup_rotated_logs(path); !r) return r;

    // After a full snapshot, all messages are now on disk.
    persisted_count_ = messages.size();

    return {};
}

// ─── Session::append_new_messages ────────────────────────────────────────────
// Mirrors Rust's append_persisted_message: only appends JSONL lines for
// messages added since the last persist/append.  Falls back to full persist()
// when the file doesn't exist or is empty (bootstrap).

tl::expected<void, std::string> Session::append_new_messages(const fs::path& path) {
    // Nothing new to write.
    if (persisted_count_ >= messages.size()) return {};

    // Bootstrap: file missing or empty → write full snapshot instead.
    {
        std::error_code ec;
        auto sz = fs::file_size(path, ec);
        if (ec || sz == 0) {
            return persist(path);  // persist() sets persisted_count_
        }
    }

    // Rotate if the file has grown past the threshold.
    // After rotation the old file is gone, so fall back to full snapshot.
    {
        std::error_code ec;
        auto sz = fs::file_size(path, ec);
        if (!ec && sz >= ROTATE_AFTER_BYTES) {
            if (auto r = rotate_session_file_if_needed(path); !r) return r;
            if (auto r = cleanup_rotated_logs(path); !r) return r;
            return persist(path);
        }
    }

    // Append only the new messages.
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return tl::unexpected(std::format("cannot open session file for append: {}", path.string()));

    for (std::size_t i = persisted_count_; i < messages.size(); ++i) {
        out << make_message_record(messages[i]).dump() << '\n';
    }
    if (!out) return tl::unexpected(std::format("write failed appending to: {}", path.string()));

    persisted_count_ = messages.size();
    return {};
}

// ─── Session::load ────────────────────────────────────────────────────────────
// Mirrors Rust Session::load_from_path. Handles both legacy JSON objects and
// JSONL-format files. JSONL record types: session_meta, message, compaction.

tl::expected<Session, std::string> Session::load(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return tl::unexpected(std::format("cannot open session file: {}", path.string()));

    std::string contents((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    in.close();

    // --- Try to detect legacy single-JSON-object format ---
    // A legacy session starts with '{' and contains a "messages" key.
    {
        std::string_view sv = contents;
        // Skip leading whitespace
        std::size_t first = sv.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos && sv[first] == '{') {
            try {
                auto j = nlohmann::json::parse(contents);
                if (j.is_object() && j.contains("messages")) {
                    // Legacy object format: {"version":1,"messages":[...], ...}
                    Session session;
                    session.id = j.value("session_id", generate_session_id());
                    if (j.contains("parent_session_id") && j["parent_session_id"].is_string()) {
                        session.parent_session_id = j["parent_session_id"].get<std::string>();
                    }
                    if (j.contains("branch_name") && j["branch_name"].is_string()) {
                        session.branch_name = j["branch_name"].get<std::string>();
                    }
                    if (j.contains("model") && j["model"].is_string()) {
                        session.model = j["model"].get<std::string>();
                    }
                    if (j.contains("system_prompt") && j["system_prompt"].is_string()) {
                        session.system_prompt = j["system_prompt"].get<std::string>();
                    }
                    for (const auto& m : j["messages"]) {
                        session.messages.push_back(message_from_json(m));
                    }
                    session.mark_all_persisted();
                    return session;
                }
                // Falls through to JSONL parsing if not a legacy object.
            } catch (...) {
                // Not a valid JSON object; try JSONL.
            }
        }
    }

    // --- JSONL format ---
    Session session;
    bool    got_meta = false;
    int     line_number = 0;

    std::istringstream iss(contents);
    std::string raw_line;
    while (std::getline(iss, raw_line)) {
        ++line_number;
        // Trim
        std::string_view line = raw_line;
        std::size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) continue;
        line = line.substr(first);
        std::size_t last = line.find_last_not_of(" \t\r\n");
        if (last != std::string_view::npos) line = line.substr(0, last + 1);
        if (line.empty()) continue;

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(line);
        } catch (const nlohmann::json::exception& e) {
            return tl::unexpected(std::format(
                "invalid JSONL record at line {}: {}", line_number, e.what()));
        }

        if (!j.is_object()) {
            return tl::unexpected(std::format(
                "JSONL record at line {} must be an object", line_number));
        }
        if (!j.contains("type") || !j["type"].is_string()) {
            return tl::unexpected(std::format(
                "JSONL record at line {} missing type", line_number));
        }

        std::string type = j["type"].get<std::string>();
        if (type == "session_meta") {
            session.id = j.value("session_id", generate_session_id());
            session.created_at_ms = j.value("created_at_ms", uint64_t{0});
            session.updated_at_ms = j.value("updated_at_ms", uint64_t{0});
            if (j.contains("model") && j["model"].is_string()) {
                session.model = j["model"].get<std::string>();
            }
            if (j.contains("system_prompt") && j["system_prompt"].is_string()) {
                session.system_prompt = j["system_prompt"].get<std::string>();
            }
            if (j.contains("fork") && j["fork"].is_object()) {
                const auto& fk = j["fork"];
                if (fk.contains("parent_session_id") && fk["parent_session_id"].is_string()) {
                    session.parent_session_id = fk["parent_session_id"].get<std::string>();
                }
                if (fk.contains("branch_name") && fk["branch_name"].is_string()) {
                    session.branch_name = fk["branch_name"].get<std::string>();
                }
            }
            got_meta = true;
        } else if (type == "message") {
            if (!j.contains("message")) {
                return tl::unexpected(std::format(
                    "JSONL record at line {} missing message", line_number));
            }
            session.messages.push_back(message_from_json(j["message"]));
        } else if (type == "compaction") {
            // Compaction metadata is acknowledged but not stored on C++ Session
            // (C++ header has no compaction field). We simply skip it; the
            // messages that follow are the live messages.
        } else {
            return tl::unexpected(std::format(
                "unsupported JSONL record type at line {}: {}", line_number, type));
        }
    }

    if (session.id.empty()) {
        session.id = generate_session_id();
    }

    session.mark_all_persisted();
    return session;
}

// ─── Session::fork ────────────────────────────────────────────────────────────
// Creates a child session sharing all messages. Mirrors Rust Session::fork.

Session Session::fork(std::string fork_branch_name) const {
    auto now = current_time_millis();
    Session child;
    child.id                = generate_session_id();
    child.model             = model;
    child.system_prompt     = system_prompt;
    child.messages          = messages;
    child.parent_session_id = id;               // lineage: parent is this session
    child.created_at_ms     = now;
    child.updated_at_ms     = now;
    // branch_name belongs to the fork's own metadata (Rust takes Option<String>).
    child.branch_name       = fork_branch_name.empty()
                                  ? std::nullopt
                                  : std::optional<std::string>{std::move(fork_branch_name)};
    return child;
}

} // namespace claw::runtime
