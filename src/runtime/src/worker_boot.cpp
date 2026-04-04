// In-memory worker-boot state machine and control registry.
//
// Provides a foundational control plane for reliable worker startup:
// trust-gate detection, ready-for-prompt handshakes, and prompt-misdelivery
// detection/recovery all live above raw terminal transport.
//
// Translated from Rust: crates/runtime/src/worker_boot.rs

#include "worker_boot.hpp"
#include "trust_resolver.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <filesystem>
#include <string>
#include <string_view>

namespace claw::runtime {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

uint64_t now_secs() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

// Return a truncated preview of a prompt string (≤48 visible chars, then "…").
// Mirrors Rust prompt_preview().
std::string prompt_preview(std::string_view prompt) {
    // Trim leading/trailing whitespace
    auto start = prompt.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    auto end = prompt.find_last_not_of(" \t\r\n");
    std::string_view trimmed = prompt.substr(start, end - start + 1);

    // Count Unicode code-points (UTF-8 aware: skip continuation bytes).
    std::size_t char_count = 0;
    for (unsigned char c : trimmed) {
        if ((c & 0xC0) != 0x80) ++char_count;   // not a UTF-8 continuation byte
    }
    if (char_count <= 48) return std::string(trimmed);

    // Take the first 48 code-points.
    std::string preview;
    preview.reserve(64);
    std::size_t counted = 0;
    for (std::size_t i = 0; i < trimmed.size(); ) {
        unsigned char c = static_cast<unsigned char>(trimmed[i]);
        std::size_t char_bytes = 1;
        if      ((c & 0x80) == 0x00) char_bytes = 1;
        else if ((c & 0xE0) == 0xC0) char_bytes = 2;
        else if ((c & 0xF0) == 0xE0) char_bytes = 3;
        else if ((c & 0xF8) == 0xF0) char_bytes = 4;
        if (counted == 48) break;
        preview.append(trimmed.data() + i, char_bytes);
        ++counted;
        i += char_bytes;
    }
    // Trim trailing whitespace from preview then append ellipsis.
    auto trim_end = preview.find_last_not_of(" \t");
    if (trim_end != std::string::npos) preview.erase(trim_end + 1);
    preview += "\xe2\x80\xa6";  // UTF-8 "…"
    return preview;
}

// Does the path `cwd` fall under `trusted_root`?
// Mirrors Rust path_matches_allowlist(): canonicalize if possible, then
// check equality or prefix.
bool path_matches_allowlist(std::string_view cwd, std::string_view trusted_root) noexcept {
    // Delegate to the shared helper declared in trust_resolver.hpp.
    return path_matches_trusted_root(cwd, trusted_root);
}

// Conservative heuristic: is the last non-empty screen line a shell prompt?
// Shell prompts end or start with $, %, or #.
bool is_shell_prompt(std::string_view trimmed) noexcept {
    if (trimmed.empty()) return false;
    char back  = trimmed.back();
    char front = trimmed.front();
    return back  == '$' || back  == '%' || back  == '#'
        || front == '$' || front == '%' || front == '#';
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// worker_status_name
// ---------------------------------------------------------------------------

std::string_view worker_status_name(WorkerStatus s) noexcept {
    switch (s) {
        case WorkerStatus::Spawning:       return "spawning";
        case WorkerStatus::TrustRequired:  return "trust_required";
        case WorkerStatus::ReadyForPrompt: return "ready_for_prompt";
        case WorkerStatus::PromptAccepted: return "prompt_accepted";
        case WorkerStatus::Running:        return "running";
        case WorkerStatus::Blocked:        return "blocked";
        case WorkerStatus::Finished:       return "finished";
        case WorkerStatus::Failed:         return "failed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Screen-text detectors (free functions declared in worker_boot.hpp)
// ---------------------------------------------------------------------------

// Mirrors Rust detect_ready_for_prompt().
// Returns true when the screen clearly shows a "ready for input" cue or
// one of the Claude prompt-line symbols (>, ›, ❯), but NOT a plain shell
// prompt (lines ending/starting with $, %, #).
bool detect_ready_for_prompt(std::string_view screen_text) {
    // Lower-case copy for keyword search.
    std::string lowered;
    lowered.reserve(screen_text.size());
    for (unsigned char c : screen_text) lowered.push_back(static_cast<char>(std::tolower(c)));

    static constexpr std::string_view READY_KEYWORDS[] = {
        "ready for input",
        "ready for your input",
        "ready for prompt",
        "send a message",
    };
    for (auto kw : READY_KEYWORDS) {
        if (lowered.find(kw) != std::string::npos) return true;
    }

    // Find the last non-empty line from the original (case-preserving) text.
    std::string_view remaining = screen_text;
    std::string_view last_non_empty;
    while (!remaining.empty()) {
        auto nl = remaining.find('\n');
        std::string_view line = (nl == std::string_view::npos)
            ? remaining
            : remaining.substr(0, nl);
        // Trim the line
        auto s = line.find_first_not_of(" \t\r");
        if (s != std::string_view::npos) {
            auto e = line.find_last_not_of(" \t\r");
            last_non_empty = line.substr(s, e - s + 1);
        }
        if (nl == std::string_view::npos) break;
        remaining.remove_prefix(nl + 1);
    }

    if (last_non_empty.empty()) return false;
    if (is_shell_prompt(last_non_empty)) return false;

    // Exact single-character prompt symbols.
    if (last_non_empty == ">"
     || last_non_empty == "\xc2\xbb"           // UTF-8 › (U+203A single right angle)
     || last_non_empty == "\xe2\x9d\xaf")      // UTF-8 ❯ (U+276F)
    {
        return true;
    }
    // Prefix forms: "> text", "› text", "❯ text"
    if (last_non_empty.starts_with("> ")
     || last_non_empty.starts_with("\xc2\xbb ")
     || last_non_empty.starts_with("\xe2\x9d\xaf "))
    {
        return true;
    }
    // Box-drawing forms: "│ >", "│ ›", "│ ❯"
    if (last_non_empty.find("\xe2\x94\x82 >")            != std::string_view::npos
     || last_non_empty.find("\xe2\x94\x82 \xc2\xbb")     != std::string_view::npos
     || last_non_empty.find("\xe2\x94\x82 \xe2\x9d\xaf") != std::string_view::npos)
    {
        return true;
    }

    return false;
}

// Mirrors Rust detect_prompt_misdelivery().
// Returns true when the screen shows a shell error AND the first non-empty
// line of the prompt appears in the screen text (i.e. the prompt was fed to
// the shell as a command).
bool detect_prompt_misdelivery(std::string_view screen_text, std::string_view prompt) {
    if (prompt.empty()) return false;

    // Lower-case screen text for keyword search.
    std::string lowered;
    lowered.reserve(screen_text.size());
    for (unsigned char c : screen_text) lowered.push_back(static_cast<char>(std::tolower(c)));

    static constexpr std::string_view SHELL_ERRORS[] = {
        "command not found",
        "syntax error near unexpected token",
        "parse error near",
        "no such file or directory",
        "unknown command",
    };
    bool shell_error = false;
    for (auto err : SHELL_ERRORS) {
        if (lowered.find(err) != std::string::npos) { shell_error = true; break; }
    }
    if (!shell_error) return false;

    // Find first non-empty line of prompt, lower-cased.
    std::string_view rem = prompt;
    std::string first_prompt_line_lower;
    while (!rem.empty()) {
        auto nl = rem.find('\n');
        std::string_view line = (nl == std::string_view::npos) ? rem : rem.substr(0, nl);
        // Trim
        auto s = line.find_first_not_of(" \t\r");
        if (s != std::string_view::npos) {
            auto e = line.find_last_not_of(" \t\r");
            std::string_view trimmed = line.substr(s, e - s + 1);
            for (unsigned char c : trimmed)
                first_prompt_line_lower.push_back(static_cast<char>(std::tolower(c)));
            break;
        }
        if (nl == std::string_view::npos) break;
        rem.remove_prefix(nl + 1);
    }

    // Mirrors Rust: "first_prompt_line.is_empty() || lowered.contains(&first_prompt_line)"
    return first_prompt_line_lower.empty()
        || lowered.find(first_prompt_line_lower) != std::string::npos;
}

// ---------------------------------------------------------------------------
// WorkerRegistry::push_event  (private)
// ---------------------------------------------------------------------------

void WorkerRegistry::push_event(
    Worker& w,
    WorkerEventKind kind,
    std::optional<std::string> detail)
{
    uint64_t ts = now_secs();
    w.events.push_back(WorkerEvent{
        .seq             = ++w.event_seq,
        .kind            = kind,
        .status          = w.status,
        .detail          = std::move(detail),
        .timestamp_epoch = ts,
    });
}

// ---------------------------------------------------------------------------
// WorkerRegistry::create
// ---------------------------------------------------------------------------

Worker WorkerRegistry::create(std::string cwd, std::optional<std::string> prompt) {
    std::lock_guard lock(mutex_);
    ++inner_.counter;
    uint64_t ts = now_secs();
    std::string worker_id = std::format("worker_{:08x}_{}", ts, inner_.counter);

    // Determine whether this cwd is in the trusted-root allowlist.
    bool auto_trusted = false;
    for (const auto& root : trusted_roots_) {
        if (path_matches_allowlist(cwd, root)) {
            auto_trusted = true;
            break;
        }
    }

    Worker w;
    w.worker_id   = worker_id;
    w.cwd         = cwd;
    w.prompt      = std::move(prompt);
    w.auto_trusted = auto_trusted;
    w.status      = WorkerStatus::Spawning;
    w.event_seq   = 0;

    push_event(w, WorkerEventKind::Spawned, "worker created");
    inner_.workers.emplace(worker_id, w);
    return w;
}

// ---------------------------------------------------------------------------
// WorkerRegistry::get / list / len
// ---------------------------------------------------------------------------

std::optional<Worker> WorkerRegistry::get(std::string_view worker_id) const {
    std::lock_guard lock(mutex_);
    auto it = inner_.workers.find(std::string(worker_id));
    if (it == inner_.workers.end()) return std::nullopt;
    return it->second;
}

std::vector<Worker> WorkerRegistry::list() const {
    std::lock_guard lock(mutex_);
    std::vector<Worker> result;
    result.reserve(inner_.workers.size());
    for (const auto& [id, w] : inner_.workers) result.push_back(w);
    return result;
}

std::size_t WorkerRegistry::len() const {
    std::lock_guard lock(mutex_);
    return inner_.workers.size();
}

// ---------------------------------------------------------------------------
// WorkerRegistry::observe
// ---------------------------------------------------------------------------
// Mirrors Rust WorkerRegistry::observe() in full:
//   1. Trust-gate detection (with optional auto-resolve for allowlisted cwds)
//   2. Prompt-misdelivery detection (with optional auto-replay)
//   3. Running-cue detection (PromptAccepted → Running)
//   4. Ready-for-prompt detection

tl::expected<Worker, std::string>
WorkerRegistry::observe(std::string_view worker_id, std::string_view screen_text) {
    std::lock_guard lock(mutex_);
    auto it = inner_.workers.find(std::string(worker_id));
    if (it == inner_.workers.end()) {
        return tl::unexpected(std::format("worker not found: {}", worker_id));
    }
    Worker& w = it->second;

    // Lower-cased copy used for all keyword searches.
    std::string lowered;
    lowered.reserve(screen_text.size());
    for (unsigned char c : screen_text) lowered.push_back(static_cast<char>(std::tolower(c)));

    // ── 1. Trust-gate ────────────────────────────────────────────────────────
    // Only check when the trust gate has not yet been cleared.
    // (In the C++ model we track this via status == TrustRequired to avoid
    //  re-entering the trust block; conceptually equivalent to Rust's
    //  !worker.trust_gate_cleared guard.)
    if (w.status != WorkerStatus::TrustRequired
     && detect_trust_prompt(screen_text))
    {
        w.status = WorkerStatus::TrustRequired;
        push_event(w, WorkerEventKind::TrustRequired, "trust prompt detected");

        if (w.auto_trusted) {
            // Allowlisted repo: auto-resolve and keep going.
            w.status = WorkerStatus::Spawning;
            push_event(w, WorkerEventKind::TrustResolved,
                       "allowlisted repo auto-resolved trust prompt");
            // Fall through to the remaining detectors.
        } else {
            return w;
        }
    }

    // ── 2. Prompt-misdelivery ─────────────────────────────────────────────────
    // Relevant only when we have already sent a prompt (PromptAccepted or Running).
    bool misdelivery_relevant =
        (w.status == WorkerStatus::PromptAccepted || w.status == WorkerStatus::Running)
        && w.prompt.has_value();

    if (misdelivery_relevant
     && detect_prompt_misdelivery(lowered, w.prompt.has_value() ? *w.prompt : ""))
    {
        std::string detail = "shell misdelivery detected";
        push_event(w, WorkerEventKind::Blocked, detail);

        // Always arm replay prompt (mirrors Rust auto_recover_prompt_misdelivery;
        // in C++ the registry always stores replay_prompt and the caller decides
        // whether to use it).
        w.replay_prompt = w.prompt;
        w.status = WorkerStatus::ReadyForPrompt;
        push_event(w, WorkerEventKind::ReadyForPrompt,
                   "prompt replay armed after shell misdelivery");
        return w;
    }

    // ── 3. Running-cue ───────────────────────────────────────────────────────
    // Mirrors Rust detect_running_cue: "thinking", "working", "running tests",
    // "inspecting", "analyzing".
    static constexpr std::string_view RUNNING_CUES[] = {
        "thinking", "working", "running tests", "inspecting", "analyzing",
    };
    bool running_cue = false;
    for (auto cue : RUNNING_CUES) {
        if (lowered.find(cue) != std::string::npos) { running_cue = true; break; }
    }
    if (running_cue
     && (w.status == WorkerStatus::PromptAccepted
      || w.status == WorkerStatus::ReadyForPrompt))
    {
        w.status = WorkerStatus::Running;
        push_event(w, WorkerEventKind::Running,
                   "worker accepted prompt and started running");
    }

    // ── 4. Ready-for-prompt ──────────────────────────────────────────────────
    // Mirrors Rust: only transition when NOT already ReadyForPrompt or Running.
    if (detect_ready_for_prompt(screen_text)
     && w.status != WorkerStatus::ReadyForPrompt
     && w.status != WorkerStatus::Running)
    {
        w.status = WorkerStatus::ReadyForPrompt;
        push_event(w, WorkerEventKind::ReadyForPrompt,
                   "worker is ready for prompt delivery");
    }

    return w;
}

// ---------------------------------------------------------------------------
// WorkerRegistry::resolve_trust
// ---------------------------------------------------------------------------

tl::expected<Worker, std::string>
WorkerRegistry::resolve_trust(std::string_view worker_id, bool approved) {
    std::lock_guard lock(mutex_);
    auto it = inner_.workers.find(std::string(worker_id));
    if (it == inner_.workers.end()) {
        return tl::unexpected(std::format("worker not found: {}", worker_id));
    }
    Worker& w = it->second;

    if (w.status != WorkerStatus::TrustRequired) {
        return tl::unexpected(std::format(
            "worker {} is not waiting on trust; current status: {}",
            worker_id, worker_status_name(w.status)));
    }

    if (!approved) {
        w.status = WorkerStatus::Failed;
        push_event(w, WorkerEventKind::TrustDenied, "user denied trust");
    } else {
        w.status = WorkerStatus::Spawning;
        push_event(w, WorkerEventKind::TrustResolved, "trust prompt resolved manually");
    }
    return w;
}

// ---------------------------------------------------------------------------
// WorkerRegistry::send_prompt
// ---------------------------------------------------------------------------
// Mirrors Rust send_prompt():
//   - Worker must be ReadyForPrompt.
//   - Use caller-provided prompt, or fall back to replay_prompt.
//   - Increment prompt_delivery_attempts (tracked via event count proxy here;
//     the C++ Worker struct doesn't carry the counter so we just ensure state
//     transitions match).

tl::expected<Worker, std::string>
WorkerRegistry::send_prompt(std::string_view worker_id,
                             std::optional<std::string> new_prompt) {
    std::lock_guard lock(mutex_);
    auto it = inner_.workers.find(std::string(worker_id));
    if (it == inner_.workers.end()) {
        return tl::unexpected(std::format("worker not found: {}", worker_id));
    }
    Worker& w = it->second;

    if (w.status != WorkerStatus::ReadyForPrompt) {
        return tl::unexpected(std::format(
            "worker {} is not ready for prompt delivery; current status: {}",
            worker_id, worker_status_name(w.status)));
    }

    // Resolve the prompt to send: caller-provided (trimmed, non-empty) or replay.
    std::optional<std::string> resolved;
    if (new_prompt.has_value()) {
        std::string trimmed = *new_prompt;
        // Strip leading/trailing whitespace.
        auto s = trimmed.find_first_not_of(" \t\r\n");
        auto e = trimmed.find_last_not_of(" \t\r\n");
        if (s != std::string::npos) trimmed = trimmed.substr(s, e - s + 1);
        else trimmed.clear();
        if (!trimmed.empty()) resolved = std::move(trimmed);
    }
    if (!resolved.has_value() && w.replay_prompt.has_value()) {
        resolved = w.replay_prompt;
    }
    if (!resolved.has_value()) {
        return tl::unexpected(std::format(
            "worker {} has no prompt to send or replay", worker_id));
    }

    std::string preview = prompt_preview(*resolved);
    w.prompt        = resolved;
    w.replay_prompt = std::nullopt;
    w.status        = WorkerStatus::PromptAccepted;
    push_event(w, WorkerEventKind::PromptSent,
               std::format("prompt accepted for delivery: {}", preview));
    return w;
}

// ---------------------------------------------------------------------------
// WorkerRegistry::terminate
// ---------------------------------------------------------------------------

tl::expected<Worker, std::string> WorkerRegistry::terminate(std::string_view worker_id) {
    std::lock_guard lock(mutex_);
    auto it = inner_.workers.find(std::string(worker_id));
    if (it == inner_.workers.end()) {
        return tl::unexpected(std::format("worker not found: {}", worker_id));
    }
    Worker& w = it->second;
    w.status = WorkerStatus::Finished;
    push_event(w, WorkerEventKind::Finished, "worker terminated by control plane");
    return w;
}

// ---------------------------------------------------------------------------
// WorkerRegistry::restart
// ---------------------------------------------------------------------------

tl::expected<Worker, std::string> WorkerRegistry::restart(std::string_view worker_id) {
    std::lock_guard lock(mutex_);
    auto it = inner_.workers.find(std::string(worker_id));
    if (it == inner_.workers.end()) {
        return tl::unexpected(std::format("worker not found: {}", worker_id));
    }
    Worker& w = it->second;
    // Reset to initial spawning state; preserve replay_prompt from current prompt.
    w.replay_prompt = w.prompt;
    w.prompt        = std::nullopt;
    w.status        = WorkerStatus::Spawning;
    push_event(w, WorkerEventKind::Spawned, "worker restarted");
    return w;
}

} // namespace claw::runtime
