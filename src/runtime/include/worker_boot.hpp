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
    PromptAccepted,
    Running,
    Blocked,
    Finished,
    Failed,
};

[[nodiscard]] std::string_view worker_status_name(WorkerStatus s) noexcept;

enum class WorkerFailureKind {
    TrustGate,
    PromptDelivery,
    Protocol,
};

enum class WorkerEventKind {
    Spawned,
    TrustRequired,
    TrustResolved,
    TrustDenied,
    ReadyForPrompt,
    PromptSent,
    PromptAccepted,
    Running,
    Blocked,
    Finished,
    Failed,
};

struct WorkerEvent {
    uint64_t seq;
    WorkerEventKind kind;
    WorkerStatus status;
    std::optional<std::string> detail;
    uint64_t timestamp_epoch{0};
};

struct Worker {
    std::string worker_id;
    std::string cwd;
    std::optional<std::string> prompt;
    std::optional<std::string> replay_prompt;
    bool auto_trusted{false};
    WorkerStatus status{WorkerStatus::Spawning};
    std::vector<WorkerEvent> events;
    uint64_t event_seq{0};
};

// Screen text detectors
[[nodiscard]] bool detect_ready_for_prompt(std::string_view screen_text);
[[nodiscard]] bool detect_prompt_misdelivery(std::string_view screen_text, std::string_view prompt);

class WorkerRegistry {
public:
    explicit WorkerRegistry(std::vector<std::string> trusted_roots = {})
        : trusted_roots_(std::move(trusted_roots)) {}

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

    [[nodiscard]] std::size_t len() const;

private:
    struct Inner {
        std::unordered_map<std::string, Worker> workers;
        uint64_t counter{0};
    };
    mutable std::mutex mutex_;
    Inner inner_;
    std::vector<std::string> trusted_roots_;
    TrustResolver trust_resolver_{TrustConfig{}};

    Worker& worker_ref(std::string_view id);
    void push_event(Worker& w, WorkerEventKind kind, std::optional<std::string> detail = std::nullopt);
};

} // namespace claw::runtime
