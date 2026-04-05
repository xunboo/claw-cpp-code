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

// Return a truncated preview of a prompt string (<=48 visible chars, then "...").
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
    preview += "\xe2\x80\xa6";  // UTF-8 "..."
    return preview;
}

// Does the path `cwd` fall under `trusted_root`?
bool path_matches_allowlist(std::string_view cwd, std::string_view trusted_root) noexcept {
    return path_matches_trusted_root(cwd, trusted_root);
}

// Conservative heuristic: is the last non-empty screen line a shell prompt?
bool is_shell_prompt(std::string_view trimmed) noexcept {
    if (trimmed.empty()) return false;
    char back  = trimmed.back();
    char front = trimmed.front();
    return back  == '$' || back  == '%' || back  == '#'
        || front == '$' || front == '%' || front == '#';
}

bool is_shell_prompt_token(std::string_view token) noexcept {
    return token == "$" || token == "%" || token == "#"
        || token == ">" || token == "\xc2\xbb"         // UTF-8 > (U+203A)
        || token == "\xe2\x9d\xaf";                     // UTF-8 > (U+276F)
}

bool looks_like_cwd_label(std::string_view candidate) noexcept {
    return candidate.starts_with('/')
        || candidate.starts_with('~')
        || candidate.starts_with('.')
        || candidate.find('/') != std::string_view::npos;
}

std::filesystem::path normalize_path(std::string_view p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto canonical = fs::canonical(fs::path(p), ec);
    if (!ec) return canonical;
    return fs::path(p);
}

bool cwd_matches_observed_target(std::string_view expected_cwd, std::string_view observed_cwd) {
    namespace fs = std::filesystem;
    auto expected = normalize_path(expected_cwd);
    auto expected_base = expected.filename().string();
    if (expected_base.empty()) expected_base = expected.string();

    fs::path observed_path(observed_cwd);
    auto observed_base = observed_path.filename().string();
    if (observed_base.empty()) {
        // Trim colons from observed
        observed_base = std::string(observed_cwd);
        while (!observed_base.empty() && observed_base.back() == ':') observed_base.pop_back();
        while (!observed_base.empty() && observed_base.front() == ':') observed_base.erase(0, 1);
    }

    auto expected_str = expected.string();
    return expected_str.ends_with(observed_cwd)
        || std::string_view(observed_cwd).ends_with(expected_str)
        || expected_base == observed_base;
}

std::optional<std::string> detect_observed_shell_cwd(std::string_view screen_text) {
    std::string_view remaining = screen_text;
    while (!remaining.empty()) {
        auto nl = remaining.find('\n');
        std::string_view line = (nl == std::string_view::npos) ? remaining : remaining.substr(0, nl);

        // Split line into whitespace-separated tokens
        std::vector<std::string_view> tokens;
        std::string_view rest = line;
        while (!rest.empty()) {
            auto s = rest.find_first_not_of(" \t\r");
            if (s == std::string_view::npos) break;
            rest = rest.substr(s);
            auto e = rest.find_first_of(" \t\r");
            if (e == std::string_view::npos) {
                tokens.push_back(rest);
                rest = {};
            } else {
                tokens.push_back(rest.substr(0, e));
                rest = rest.substr(e);
            }
        }

        // Find a shell prompt token and check the token before it
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            if (is_shell_prompt_token(tokens[i]) && i > 0) {
                auto candidate = tokens[i - 1];
                if (looks_like_cwd_label(candidate)) {
                    return std::string(candidate);
                }
            }
        }

        if (nl == std::string_view::npos) break;
        remaining = remaining.substr(nl + 1);
    }
    return std::nullopt;
}

struct PromptDeliveryObservation {
    WorkerPromptTarget target;
    std::optional<std::string> observed_cwd;
};

std::string_view prompt_misdelivery_detail(const PromptDeliveryObservation& observation) {
    switch (observation.target) {
        case WorkerPromptTarget::Shell:       return "shell misdelivery detected";
        case WorkerPromptTarget::WrongTarget: return "prompt landed in wrong target";
        case WorkerPromptTarget::Unknown:     return "prompt delivery failure detected";
    }
    return "prompt delivery failure detected";
}

// Mirrors Rust detect_prompt_misdelivery with enhanced wrong-target detection.
std::optional<PromptDeliveryObservation> detect_prompt_misdelivery(
    std::string_view screen_text,
    std::string_view lowered,
    std::optional<std::string_view> prompt,
    std::string_view expected_cwd)
{
    if (!prompt.has_value()) return std::nullopt;

    // Find first non-empty line of prompt, lower-cased and trimmed.
    std::string prompt_snippet;
    {
        std::string_view rem = *prompt;
        while (!rem.empty()) {
            auto nl = rem.find('\n');
            std::string_view line = (nl == std::string_view::npos) ? rem : rem.substr(0, nl);
            auto s = line.find_first_not_of(" \t\r");
            if (s != std::string_view::npos) {
                auto e = line.find_last_not_of(" \t\r");
                std::string_view trimmed = line.substr(s, e - s + 1);
                for (unsigned char c : trimmed)
                    prompt_snippet.push_back(static_cast<char>(std::tolower(c)));
                break;
            }
            if (nl == std::string_view::npos) break;
            rem = rem.substr(nl + 1);
        }
    }
    if (prompt_snippet.empty()) return std::nullopt;
    bool prompt_visible = lowered.find(prompt_snippet) != std::string::npos;

    // Check for wrong-target delivery
    if (auto observed_cwd = detect_observed_shell_cwd(screen_text)) {
        if (prompt_visible && !cwd_matches_observed_target(expected_cwd, *observed_cwd)) {
            return PromptDeliveryObservation{
                WorkerPromptTarget::WrongTarget,
                std::move(observed_cwd),
            };
        }
    }

    // Check for shell error indicators
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

    if (shell_error && prompt_visible) {
        return PromptDeliveryObservation{
            WorkerPromptTarget::Shell,
            std::nullopt,
        };
    }

    return std::nullopt;
}

bool detect_running_cue(std::string_view lowered) {
    static constexpr std::string_view RUNNING_CUES[] = {
        "thinking", "working", "running tests", "inspecting", "analyzing",
    };
    for (auto cue : RUNNING_CUES) {
        if (lowered.find(cue) != std::string::npos) return true;
    }
    return false;
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
        case WorkerStatus::Running:        return "running";
        case WorkerStatus::Finished:       return "finished";
        case WorkerStatus::Failed:         return "failed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Screen-text detectors (free functions declared in worker_boot.hpp)
// ---------------------------------------------------------------------------

bool detect_ready_for_prompt(std::string_view screen_text, std::string_view lowered) {
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

    if (last_non_empty == ">"
     || last_non_empty == "\xc2\xbb"
     || last_non_empty == "\xe2\x9d\xaf")
    {
        return true;
    }
    if (last_non_empty.starts_with("> ")
     || last_non_empty.starts_with("\xc2\xbb ")
     || last_non_empty.starts_with("\xe2\x9d\xaf "))
    {
        return true;
    }
    if (last_non_empty.find("\xe2\x94\x82 >")            != std::string_view::npos
     || last_non_empty.find("\xe2\x94\x82 \xc2\xbb")     != std::string_view::npos
     || last_non_empty.find("\xe2\x94\x82 \xe2\x9d\xaf") != std::string_view::npos)
    {
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// WorkerRegistry::push_event  (private)
// ---------------------------------------------------------------------------

void WorkerRegistry::push_event(
    Worker& w,
    WorkerEventKind kind,
    std::optional<std::string> detail,
    std::optional<WorkerEventPayload> payload)
{
    uint64_t ts = now_secs();
    w.events.push_back(WorkerEvent{
        .seq             = ++w.event_seq,
        .kind            = kind,
        .status          = w.status,
        .detail          = std::move(detail),
        .payload         = std::move(payload),
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
    w.trust_gate_cleared = false;
    w.auto_recover_prompt_misdelivery = auto_recover_;
    w.prompt_delivery_attempts = 0;
    w.prompt_in_flight = false;
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

    // -- 1. Trust-gate --
    if (!w.trust_gate_cleared && detect_trust_prompt(screen_text)) {
        w.status = WorkerStatus::TrustRequired;
        push_event(w, WorkerEventKind::TrustRequired, "trust prompt detected",
            WorkerEventPayload{WorkerEventPayload::TrustPrompt{w.cwd, std::nullopt}});

        if (w.auto_trusted) {
            w.trust_gate_cleared = true;
            w.status = WorkerStatus::Spawning;
            push_event(w, WorkerEventKind::TrustResolved,
                       "allowlisted repo auto-resolved trust prompt",
                       WorkerEventPayload{WorkerEventPayload::TrustPrompt{
                           w.cwd, WorkerTrustResolution::AutoAllowlisted}});
        } else {
            return w;
        }
    }

    // -- 2. Prompt-misdelivery --
    bool misdelivery_relevant = w.prompt_in_flight && w.prompt.has_value();

    if (misdelivery_relevant) {
        auto observation = detect_prompt_misdelivery(
            screen_text, lowered,
            w.prompt.has_value() ? std::optional<std::string_view>(*w.prompt) : std::nullopt,
            w.cwd);

        if (observation.has_value()) {
            auto preview = prompt_preview(w.prompt.value_or(""));
            std::string message;
            switch (observation->target) {
                case WorkerPromptTarget::Shell:
                    message = std::format("worker prompt landed in shell instead of coding agent: {}", preview);
                    break;
                case WorkerPromptTarget::WrongTarget:
                    message = std::format("worker prompt landed in the wrong target instead of {}: {}", w.cwd, preview);
                    break;
                case WorkerPromptTarget::Unknown:
                    message = std::format("worker prompt delivery failed before reaching coding agent: {}", preview);
                    break;
            }

            w.last_error = WorkerFailure{
                WorkerFailureKind::PromptDelivery,
                message,
                now_secs(),
            };
            w.prompt_in_flight = false;
            w.status = WorkerStatus::Failed;
            push_event(w, WorkerEventKind::PromptMisdelivery,
                std::string(prompt_misdelivery_detail(*observation)),
                WorkerEventPayload{WorkerEventPayload::PromptDelivery{
                    preview, observation->target, observation->observed_cwd, false}});

            if (w.auto_recover_prompt_misdelivery) {
                w.replay_prompt = w.prompt;
                w.prompt_delivery_attempts += 1;
                w.status = WorkerStatus::ReadyForPrompt;
                push_event(w, WorkerEventKind::PromptReplayArmed,
                    "prompt replay armed after prompt misdelivery",
                    WorkerEventPayload{WorkerEventPayload::PromptDelivery{
                        preview, observation->target,
                        std::move(observation->observed_cwd), true}});
            } else {
                w.status = WorkerStatus::Failed;
            }
            return w;
        }
    }

    // -- 3. Running-cue --
    if (detect_running_cue(lowered) && w.prompt_in_flight) {
        w.prompt_in_flight = false;
        w.status = WorkerStatus::Running;
        w.last_error = std::nullopt;
    }

    // -- 4. Ready-for-prompt --
    if (detect_ready_for_prompt(screen_text, lowered)
        && w.status != WorkerStatus::ReadyForPrompt)
    {
        w.status = WorkerStatus::ReadyForPrompt;
        w.prompt_in_flight = false;
        if (w.last_error.has_value()
            && w.last_error->kind == WorkerFailureKind::TrustGate)
        {
            w.last_error = std::nullopt;
        }
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
        w.trust_gate_cleared = true;
        w.status = WorkerStatus::Spawning;
        push_event(w, WorkerEventKind::TrustResolved,
                   "trust prompt resolved manually",
                   WorkerEventPayload{WorkerEventPayload::TrustPrompt{
                       w.cwd, WorkerTrustResolution::ManualApproval}});
    }
    return w;
}

// ---------------------------------------------------------------------------
// WorkerRegistry::send_prompt
// ---------------------------------------------------------------------------

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
    w.prompt_delivery_attempts += 1;
    w.prompt_in_flight = true;
    w.prompt        = resolved;
    w.replay_prompt = std::nullopt;
    w.last_error    = std::nullopt;
    w.status        = WorkerStatus::Running;
    push_event(w, WorkerEventKind::Running,
               std::format("prompt dispatched to worker: {}", preview));
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
    w.prompt_in_flight = false;
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
    w.prompt        = std::nullopt;
    w.replay_prompt = std::nullopt;
    w.last_error    = std::nullopt;
    w.prompt_delivery_attempts = 0;
    w.prompt_in_flight = false;
    w.status        = WorkerStatus::Spawning;
    push_event(w, WorkerEventKind::Restarted, "worker restarted");
    return w;
}

// ---------------------------------------------------------------------------
// WorkerRegistry::observe_completion
// ---------------------------------------------------------------------------

tl::expected<Worker, std::string>
WorkerRegistry::observe_completion(std::string_view worker_id,
                                    std::string_view finish_reason,
                                    uint64_t tokens_output) {
    std::lock_guard lock(mutex_);
    auto it = inner_.workers.find(std::string(worker_id));
    if (it == inner_.workers.end()) {
        return tl::unexpected(std::format("worker not found: {}", worker_id));
    }
    Worker& w = it->second;

    bool is_provider_failure =
        (finish_reason == "unknown" && tokens_output == 0) || finish_reason == "error";

    if (is_provider_failure) {
        std::string message;
        if (finish_reason == "unknown" && tokens_output == 0) {
            message = "session completed with finish='unknown' and zero output -- provider degraded or context exhausted";
        } else {
            message = std::format("session failed with finish='{}' -- provider error", finish_reason);
        }

        w.last_error = WorkerFailure{
            WorkerFailureKind::Provider,
            message,
            now_secs(),
        };
        w.status = WorkerStatus::Failed;
        w.prompt_in_flight = false;
        push_event(w, WorkerEventKind::Failed, "provider failure classified");
    } else {
        w.status = WorkerStatus::Finished;
        w.prompt_in_flight = false;
        w.last_error = std::nullopt;
        push_event(w, WorkerEventKind::Finished,
            std::format("session completed: finish='{}', tokens={}", finish_reason, tokens_output));
    }

    return w;
}

// ---------------------------------------------------------------------------
// WorkerRegistry::await_ready
// ---------------------------------------------------------------------------

tl::expected<WorkerReadySnapshot, std::string>
WorkerRegistry::await_ready(std::string_view worker_id) const {
    std::lock_guard lock(mutex_);
    auto it = inner_.workers.find(std::string(worker_id));
    if (it == inner_.workers.end()) {
        return tl::unexpected(std::format("worker not found: {}", worker_id));
    }
    const Worker& w = it->second;
    return WorkerReadySnapshot{
        .worker_id = w.worker_id,
        .status = w.status,
        .ready = w.status == WorkerStatus::ReadyForPrompt,
        .blocked = (w.status == WorkerStatus::TrustRequired || w.status == WorkerStatus::Failed),
        .replay_prompt_ready = w.replay_prompt.has_value(),
        .last_error = w.last_error,
    };
}

} // namespace claw::runtime
