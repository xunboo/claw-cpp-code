#pragma once
#include "trust_resolver.hpp"
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <chrono>
#include <tl/expected.hpp>

namespace claw::runtime {

enum class WorkerStatus {
    Spawning,
    TrustRequired,
    ReadyForPrompt,
    Running,
    Finished,
    Failed,
};

[[nodiscard]] std::string_view worker_status_name(WorkerStatus s) noexcept;

enum class WorkerFailureKind {
    TrustGate,
    PromptDelivery,
    Protocol,
    Provider,
};

enum class WorkerEventKind {
    Spawned,
    TrustRequired,
    TrustResolved,
    TrustDenied,
    ReadyForPrompt,
    PromptMisdelivery,
    PromptReplayArmed,
    Running,
    Restarted,
    Finished,
    Failed,
};

/// How a trust prompt was resolved.
enum class WorkerTrustResolution {
    AutoAllowlisted,
    ManualApproval,
};

/// Where the prompt was observed to land.
enum class WorkerPromptTarget {
    Shell,
    WrongTarget,
    Unknown,
};

/// Structured payload attached to specific worker events.
struct WorkerEventPayload {
    struct TrustPrompt {
        std::string cwd;
        std::optional<WorkerTrustResolution> resolution;
    };
    struct PromptDelivery {
        std::string prompt_preview;
        WorkerPromptTarget observed_target{WorkerPromptTarget::Unknown};
        std::optional<std::string> observed_cwd;
        bool recovery_armed{false};
    };

    std::variant<TrustPrompt, PromptDelivery> data;
};

/// Failure details attached to a worker.
struct WorkerFailure {
    WorkerFailureKind kind;
    std::string message;
    uint64_t created_at{0};
};

struct WorkerEvent {
    uint64_t seq;
    WorkerEventKind kind;
    WorkerStatus status;
    std::optional<std::string> detail;
    std::optional<WorkerEventPayload> payload;
    uint64_t timestamp_epoch{0};
};

struct Worker {
    std::string worker_id;
    std::string cwd;
    std::optional<std::string> prompt;
    std::optional<std::string> replay_prompt;
    bool auto_trusted{false};
    bool trust_gate_cleared{false};
    bool auto_recover_prompt_misdelivery{true};
    uint32_t prompt_delivery_attempts{0};
    bool prompt_in_flight{false};
    std::optional<WorkerFailure> last_error;
    WorkerStatus status{WorkerStatus::Spawning};
    std::vector<WorkerEvent> events;
    uint64_t event_seq{0};
};

// Screen text detectors
[[nodiscard]] bool detect_ready_for_prompt(std::string_view screen_text, std::string_view lowered);

/// Snapshot of a worker's readiness state (mirrors Rust WorkerReadySnapshot).
struct WorkerReadySnapshot {
    std::string worker_id;
    WorkerStatus status;
    bool ready{false};
    bool blocked{false};
    bool replay_prompt_ready{false};
    std::optional<WorkerFailure> last_error;
};

class WorkerRegistry {
public:
    explicit WorkerRegistry(std::vector<std::string> trusted_roots = {},
                            bool auto_recover_prompt_misdelivery = true)
        : trusted_roots_(std::move(trusted_roots)),
          auto_recover_(auto_recover_prompt_misdelivery) {}

    [[nodiscard]] Worker create(std::string cwd, std::optional<std::string> prompt = std::nullopt);
    [[nodiscard]] std::optional<Worker> get(std::string_view worker_id) const;
    [[nodiscard]] std::vector<Worker> list() const;

    // Observe a screen text snapshot: detects trust prompts, misdelivery, ready state
    [[nodiscard]] tl::expected<Worker, std::string>
        observe(std::string_view worker_id, std::string_view screen_text);

    // Resolve trust prompt (auto-trust or approval)
    [[nodiscard]] tl::expected<Worker, std::string>
        resolve_trust(std::string_view worker_id, bool approved);

    // Send the prompt to the worker
    [[nodiscard]] tl::expected<Worker, std::string>
        send_prompt(std::string_view worker_id, std::optional<std::string> new_prompt = std::nullopt);

    // Mark as terminated
    [[nodiscard]] tl::expected<Worker, std::string> terminate(std::string_view worker_id);
    [[nodiscard]] tl::expected<Worker, std::string> restart(std::string_view worker_id);

    /// Classify session completion and transition worker to appropriate terminal state.
    [[nodiscard]] tl::expected<Worker, std::string>
        observe_completion(std::string_view worker_id, std::string_view finish_reason, uint64_t tokens_output);

    /// Get a readiness snapshot for a worker.
    [[nodiscard]] tl::expected<WorkerReadySnapshot, std::string>
        await_ready(std::string_view worker_id) const;

    [[nodiscard]] std::size_t len() const;

private:
    struct Inner {
        std::unordered_map<std::string, Worker> workers;
        uint64_t counter{0};
    };
    mutable std::mutex mutex_;
    Inner inner_;
    std::vector<std::string> trusted_roots_;
    bool auto_recover_{true};
    TrustResolver trust_resolver_{TrustConfig{}};

    Worker& worker_ref(std::string_view id);
    void push_event(Worker& w, WorkerEventKind kind, std::optional<std::string> detail = std::nullopt,
                    std::optional<WorkerEventPayload> payload = std::nullopt);
};

} // namespace claw::runtime
