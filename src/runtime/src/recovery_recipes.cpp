// recovery_recipes.cpp — translated from Rust recovery_recipes.rs
//
// Encodes known automatic recoveries for the six failure scenarios and
// enforces one automatic recovery attempt before escalation. Each attempt
// is emitted as a structured recovery event.
//
// Mapping notes:
//   Rust RecoveryStep variants       C++ RecoveryStep variants
//   ─────────────────────────────    ────────────────────────────────────────
//   AcceptTrustPrompt             →  StepResetToLastGoodState   (closest proxy)
//   RedirectPromptToAgent         →  StepRestartComponent{"prompt-router"}
//   RebaseBranch                  →  StepRestartComponent{"git-rebase"}
//   CleanBuild                    →  StepRetryWithDelay{0}
//   RetryMcpHandshake{timeout}    →  StepRetryMcpHandshake{timeout}
//   RestartPlugin{name}           →  StepRestartPlugin{name}
//   EscalateToHuman{reason}       →  StepEscalateToHuman{}
//
// RecoveryResult mapping:
//   Rust Recovered{steps_taken: u32}           → RecoveryRecovered{steps_taken: vector}
//   Rust PartialRecovery{recovered, remaining} → RecoveryPartial{recovered, remaining}
//   Rust EscalationRequired{reason}            → RecoveryEscalation{reason}
//
// RecoveryEvent is a lightweight {kind, detail} struct in C++ (the Rust
// enum variants map to kind strings so structured data is preserved).
//
// RecoveryContext::fail_at_step controls simulated step failure (tests).
// Attempt counts are keyed by scenario name string, matching the header.

#include "recovery_recipes.hpp"
#include <format>

namespace claw::runtime {

// ---------------------------------------------------------------------------
// failure_scenario_name
// ---------------------------------------------------------------------------

std::string_view failure_scenario_name(FailureScenario s) noexcept {
    switch (s) {
        case FailureScenario::TrustPromptUnresolved:  return "trust_prompt_unresolved";
        case FailureScenario::PromptMisdelivery:      return "prompt_misdelivery";
        case FailureScenario::StaleBranch:            return "stale_branch";
        case FailureScenario::CompileRedCrossCrate:   return "compile_red_cross_crate";
        case FailureScenario::McpHandshakeFailure:    return "mcp_handshake_failure";
        case FailureScenario::PartialPluginStartup:   return "partial_plugin_startup";
        case FailureScenario::ProviderFailure:        return "provider_failure";
    }
    return "unknown";
}

FailureScenario from_worker_failure_kind(WorkerFailureKind kind) noexcept {
    switch (kind) {
        case WorkerFailureKind::TrustGate:       return FailureScenario::TrustPromptUnresolved;
        case WorkerFailureKind::PromptDelivery:  return FailureScenario::PromptMisdelivery;
        case WorkerFailureKind::Protocol:        return FailureScenario::McpHandshakeFailure;
        case WorkerFailureKind::Provider:        return FailureScenario::ProviderFailure;
    }
    return FailureScenario::ProviderFailure;
}

// ---------------------------------------------------------------------------
// recipe_for — mirrors Rust fn recipe_for(scenario: &FailureScenario)
// ---------------------------------------------------------------------------

RecoveryRecipe recipe_for(FailureScenario scenario) {
    switch (scenario) {

        case FailureScenario::TrustPromptUnresolved:
            // Rust: steps=[AcceptTrustPrompt], max=1, escalation=AlertHuman
            return RecoveryRecipe{
                scenario,
                { StepResetToLastGoodState{} },
                1,
                EscalationPolicy::AlertHuman,
            };

        case FailureScenario::PromptMisdelivery:
            // Rust: steps=[RedirectPromptToAgent], max=1, escalation=AlertHuman
            return RecoveryRecipe{
                scenario,
                { StepRestartComponent{"prompt-router"} },
                1,
                EscalationPolicy::AlertHuman,
            };

        case FailureScenario::StaleBranch:
            // Rust: steps=[RebaseBranch, CleanBuild], max=1, escalation=AlertHuman
            return RecoveryRecipe{
                scenario,
                { StepRestartComponent{"git-rebase"}, StepRetryWithDelay{0} },
                1,
                EscalationPolicy::AlertHuman,
            };

        case FailureScenario::CompileRedCrossCrate:
            // Rust: steps=[CleanBuild], max=1, escalation=AlertHuman
            return RecoveryRecipe{
                scenario,
                { StepRetryWithDelay{0} },
                1,
                EscalationPolicy::AlertHuman,
            };

        case FailureScenario::McpHandshakeFailure:
            // Rust: steps=[RetryMcpHandshake{timeout:5000}], max=1, escalation=Abort
            return RecoveryRecipe{
                scenario,
                { StepRetryMcpHandshake{5000} },
                1,
                EscalationPolicy::Abort,
            };

        case FailureScenario::PartialPluginStartup:
            // Rust: steps=[RestartPlugin{"stalled"}, RetryMcpHandshake{3000}],
            //        max=1, escalation=LogAndContinue
            return RecoveryRecipe{
                scenario,
                { StepRestartPlugin{"stalled"}, StepRetryMcpHandshake{3000} },
                1,
                EscalationPolicy::LogAndContinue,
            };

        case FailureScenario::ProviderFailure:
            return RecoveryRecipe{
                scenario,
                { StepRestartWorker{} },
                1,
                EscalationPolicy::AlertHuman,
            };
    }

    // Unreachable for a complete enum, but required for compiler happiness.
    return RecoveryRecipe{
        scenario,
        { StepSkipAndContinue{} },
        1,
        EscalationPolicy::LogAndContinue,
    };
}

// ---------------------------------------------------------------------------
// attempt_recovery — mirrors Rust fn attempt_recovery(scenario, ctx)
// ---------------------------------------------------------------------------
//
// Algorithm (identical to Rust):
//  1. Look up the recipe.
//  2. If attempt_count >= max_attempts → emit RecoveryAttempted + Escalated,
//     return EscalationRequired.
//  3. Increment attempt_count.
//  4. Execute steps one by one, stopping at fail_at_step.
//  5. Determine result:
//     - executed.empty() && failed  → EscalationRequired "failed at first step"
//     - !executed.empty() && failed → PartialRecovery
//     - !failed                     → Recovered
//  6. Emit RecoveryAttempted event, then:
//     - Recovered         → emit RecoverySucceeded
//     - PartialRecovery   → emit RecoveryFailed
//     - EscalationRequired→ emit Escalated
//  7. Return the result.

RecoveryResult attempt_recovery(FailureScenario scenario,
                                RecoveryContext& ctx) {
    const std::string key(failure_scenario_name(scenario));
    auto& attempt_count = ctx.attempts[key];

    RecoveryRecipe recipe = recipe_for(scenario);

    // ---- enforce one-attempt-before-escalation policy ----
    if (attempt_count >= recipe.max_attempts) {
        std::string reason = std::format(
            "max recovery attempts ({}) exceeded for {}",
            recipe.max_attempts, key);

        RecoveryEscalation escalation_result{reason};

        ctx.events.push_back(RecoveryEvent{
            "recovery_attempted",
            std::format("scenario={} result=escalation_required reason={}", key, reason)
        });
        ctx.events.push_back(RecoveryEvent{"escalated", key});

        return escalation_result;
    }

    ++attempt_count;

    // ---- execute steps, honoring fail_at_step ----
    std::vector<RecoveryStep> executed;
    bool failed = false;

    for (std::size_t i = 0; i < recipe.steps.size(); ++i) {
        if (ctx.fail_at_step.has_value() && i == *ctx.fail_at_step) {
            failed = true;
            break;
        }
        executed.push_back(recipe.steps[i]);
    }

    // ---- build result ----
    RecoveryResult result = [&]() -> RecoveryResult {
        if (failed) {
            // Collect remaining steps (from the fail index onward)
            std::vector<RecoveryStep> remaining(
                recipe.steps.begin() + static_cast<std::ptrdiff_t>(executed.size()),
                recipe.steps.end());

            if (executed.empty()) {
                // Failed at the very first step → escalate immediately
                return RecoveryEscalation{
                    std::format("recovery failed at first step for {}", key)
                };
            }
            return RecoveryPartial{std::move(executed), std::move(remaining)};
        }
        // All steps completed successfully
        return RecoveryRecovered{std::move(executed)};
    }();

    // ---- emit structured RecoveryAttempted event ----
    const std::string result_kind = std::visit([](const auto& r) -> std::string {
        using T = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<T, RecoveryRecovered>)   return "recovered";
        if constexpr (std::is_same_v<T, RecoveryPartial>)     return "partial_recovery";
        if constexpr (std::is_same_v<T, RecoveryEscalation>)  return "escalation_required";
    }, result);

    ctx.events.push_back(RecoveryEvent{
        "recovery_attempted",
        std::format("scenario={} result={}", key, result_kind)
    });

    // ---- emit follow-up event ----
    std::visit([&](const auto& r) {
        using T = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<T, RecoveryRecovered>) {
            ctx.events.push_back(RecoveryEvent{"recovery_succeeded", key});
        } else if constexpr (std::is_same_v<T, RecoveryPartial>) {
            ctx.events.push_back(RecoveryEvent{"recovery_failed", key});
        } else if constexpr (std::is_same_v<T, RecoveryEscalation>) {
            ctx.events.push_back(RecoveryEvent{"escalated", key});
        }
    }, result);

    return result;
}

} // namespace claw::runtime
