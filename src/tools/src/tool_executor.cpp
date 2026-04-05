#include "tool_executor.hpp"
#include "agent_tools.hpp"
#include "lane_completion.hpp"
#include "misc_tools.hpp"
#include "tool_specs.hpp"
#include "web_tools.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <filesystem>
#include <mutex>

// Runtime crate headers for real dispatch
#include "permission_enforcer.hpp"
#include "bash.hpp"
#include "file_ops.hpp"
#include "lane_events.hpp"
#include "mcp_lifecycle_hardened.hpp"
#include "task_packet.hpp"
#include "task_registry.hpp"
#include "worker_boot.hpp"
#include "team_cron_registry.hpp"
#include "lsp_client.hpp"
#include "mcp_tool_bridge.hpp"

namespace claw::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── Global registries (mirrors Rust's OnceLock-based singletons) ─────────

static claw::runtime::TaskRegistry& global_task_registry() {
    static claw::runtime::TaskRegistry instance;
    return instance;
}

static claw::runtime::WorkerRegistry& global_worker_registry() {
    static claw::runtime::WorkerRegistry instance;
    return instance;
}

static claw::runtime::TeamRegistry& global_team_registry() {
    static claw::runtime::TeamRegistry instance;
    return instance;
}

static claw::runtime::CronRegistry& global_cron_registry() {
    static claw::runtime::CronRegistry instance;
    return instance;
}

static claw::runtime::LspRegistry& global_lsp_registry() {
    static claw::runtime::LspRegistry instance;
    return instance;
}

static claw::runtime::McpToolRegistry& global_mcp_registry() {
    static claw::runtime::McpToolRegistry instance;
    return instance;
}

// ── Permission helpers ────────────────────────────────────────────────────

tl::expected<PermissionMode, std::string>
permission_mode_from_plugin(std::string_view value) {
    if (value == "read-only")           return static_cast<PermissionMode>(0);
    if (value == "workspace-write")     return static_cast<PermissionMode>(1);
    if (value == "danger-full-access")  return static_cast<PermissionMode>(2);
    return tl::unexpected(std::string("unsupported plugin permission: ") + std::string(value));
}

tl::expected<void, std::string>
enforce_permission_check(const claw::runtime::PermissionEnforcer& enforcer,
                          const std::string& tool_name,
                          const json& input)
{
    auto input_str = input.dump();
    auto result = enforcer.check(tool_name, input_str);
    if (std::holds_alternative<claw::runtime::EnforcementDeny>(result)) {
        auto& deny = std::get<claw::runtime::EnforcementDeny>(result);
        return tl::unexpected(deny.reason);
    }
    return {};
}

// ── to_pretty_json helper ─────────────────────────────────────────────────

template<typename T>
static tl::expected<std::string, std::string> to_pretty_json(T&& val) {
    try { return nlohmann::json(std::forward<T>(val)).dump(2); }
    catch (const std::exception& e) { return tl::unexpected(std::string(e.what())); }
}

// ── Helper: get workspace root (cwd for now) ─────────────────────────────

static fs::path workspace_root() {
    return fs::current_path();
}

// ── Tool dispatch: bash ──────────────────────────────────────────────────

static tl::expected<std::string, std::string>
run_bash(const json& input) {
    claw::runtime::BashCommandInput bi;
    bi.command = input.value("command", std::string{});
    if (input.contains("cwd"))
        bi.cwd = input["cwd"].get<std::string>();
    if (input.contains("timeout"))
        bi.timeout = std::chrono::milliseconds(input["timeout"].get<uint64_t>());

    auto r = claw::runtime::execute_bash(bi);
    if (!r) return tl::unexpected(r.error());

    json out = {
        {"stdout", r->stdout_output},
        {"stderr", r->stderr_output},
        {"exit_code", r->exit_code},
        {"timed_out", r->timed_out},
        {"truncated", r->truncated}
    };
    return out.dump(2);
}

// ── Tool dispatch: file ops ──────────────────────────────────────────────

static tl::expected<std::string, std::string>
run_read_file(const json& input) {
    auto path = fs::path(input.value("file_path", std::string{}));
    auto ws = workspace_root();
    auto r = claw::runtime::read_file(path, ws);
    if (!r) return tl::unexpected(r.error());
    return json{{"content", *r}}.dump(2);
}

static tl::expected<std::string, std::string>
run_write_file(const json& input) {
    auto path = fs::path(input.value("file_path", std::string{}));
    auto content = input.value("content", std::string{});
    auto ws = workspace_root();
    auto r = claw::runtime::write_file(path, content, ws);
    if (!r) return tl::unexpected(r.error());
    return json{{"result", "ok"}}.dump(2);
}

static tl::expected<std::string, std::string>
run_edit_file(const json& input) {
    auto path = fs::path(input.value("file_path", std::string{}));
    auto old_string = input.value("old_string", std::string{});
    auto new_string = input.value("new_string", std::string{});
    auto ws = workspace_root();
    auto r = claw::runtime::edit_file(path, old_string, new_string, ws);
    if (!r) return tl::unexpected(r.error());
    return json{{"result", "ok"}}.dump(2);
}

static tl::expected<std::string, std::string>
run_glob_search(const json& input) {
    auto pattern = input.value("pattern", std::string{});
    auto dir = input.contains("path")
        ? fs::path(input["path"].get<std::string>())
        : workspace_root();
    auto r = claw::runtime::glob_search(dir, pattern);
    if (!r) return tl::unexpected(r.error());
    json files = json::array();
    for (auto& p : *r) files.push_back(p.string());
    return json{{"files", files}}.dump(2);
}

static tl::expected<std::string, std::string>
run_grep_search(const json& input) {
    auto pattern = input.value("pattern", std::string{});
    auto dir = input.contains("path")
        ? fs::path(input["path"].get<std::string>())
        : workspace_root();
    bool use_regex = input.value("use_regex", true);
    std::optional<std::string_view> file_glob;
    std::string glob_str;
    if (input.contains("glob")) {
        glob_str = input["glob"].get<std::string>();
        file_glob = glob_str;
    }
    auto r = claw::runtime::grep_search(dir, pattern, use_regex, file_glob);
    if (!r) return tl::unexpected(r.error());
    json matches = json::array();
    for (auto& m : *r) {
        matches.push_back({
            {"file", m.file.string()},
            {"line_number", m.line_number},
            {"line_content", m.line_content}
        });
    }
    return json{{"matches", matches}}.dump(2);
}

// ── Tool dispatch: PowerShell (Windows) ──────────────────────────────────

static tl::expected<std::string, std::string>
run_powershell(const json& input) {
    auto command = input.value("command", std::string{});
#ifdef _WIN32
    claw::runtime::BashCommandInput bi;
    bi.command = "powershell.exe -NoProfile -NonInteractive -Command " + command;
    if (input.contains("timeout"))
        bi.timeout = std::chrono::milliseconds(input["timeout"].get<uint64_t>());
    auto r = claw::runtime::execute_bash(bi);
    if (!r) return tl::unexpected(r.error());
    return json{
        {"stdout", r->stdout_output},
        {"stderr", r->stderr_output},
        {"exit_code", r->exit_code}
    }.dump(2);
#else
    return tl::unexpected(std::string("PowerShell is only available on Windows"));
#endif
}

// ── Tool dispatch: task registry ─────────────────────────────────────────

static tl::expected<std::string, std::string>
run_task_create(const json& input) {
    auto prompt = input.value("prompt", std::string{});
    std::optional<std::string_view> desc;
    std::string desc_str;
    if (input.contains("description")) {
        desc_str = input["description"].get<std::string>();
        desc = desc_str;
    }
    auto task = global_task_registry().create(prompt, desc);
    return json{
        {"task_id", task.task_id},
        {"status", std::string(claw::runtime::task_status_name(task.status))},
        {"prompt", task.prompt},
        {"description", task.description ? json(*task.description) : json(nullptr)},
        {"task_packet", nullptr},
        {"created_at", task.created_at}
    }.dump(2);
}

static tl::expected<std::string, std::string>
run_task_packet(const json& input) {
    // RunTaskPacket: create a task backed by a structured task packet.
    // The packet fields are flat strings (matching the simplified Rust TaskPacket).
    auto objective = input.value("objective", std::string{});
    auto scope     = input.value("scope", std::string{});
    auto repo      = input.value("repo", std::string{});
    auto branch_policy = input.value("branch_policy", std::string{});
    std::vector<std::string> acceptance_tests;
    if (input.contains("acceptance_tests")) {
        for (auto& t : input["acceptance_tests"])
            acceptance_tests.push_back(t.get<std::string>());
    }
    auto commit_policy      = input.value("commit_policy", std::string{});
    auto reporting_contract = input.value("reporting_contract", std::string{});
    auto escalation_policy  = input.value("escalation_policy", std::string{});

    // Create the task using objective as prompt, scope as description
    auto task = global_task_registry().create(objective, scope);

    // Build task_packet JSON for the output
    json packet_json = {
        {"objective", objective},
        {"scope", scope},
        {"repo", repo},
        {"branch_policy", branch_policy},
        {"acceptance_tests", acceptance_tests},
        {"commit_policy", commit_policy},
        {"reporting_contract", reporting_contract},
        {"escalation_policy", escalation_policy}
    };

    return json{
        {"task_id", task.task_id},
        {"status", std::string(claw::runtime::task_status_name(task.status))},
        {"prompt", task.prompt},
        {"description", task.description ? json(*task.description) : json(nullptr)},
        {"task_packet", packet_json},
        {"created_at", task.created_at}
    }.dump(2);
}

static tl::expected<std::string, std::string>
run_task_get(const json& input) {
    auto id = input.value("task_id", std::string{});
    auto task = global_task_registry().get(id);
    if (!task) return tl::unexpected(std::string("task not found: ") + id);
    json msgs = json::array();
    for (auto& m : task->messages)
        msgs.push_back({{"role", m.role}, {"content", m.content}, {"timestamp", m.timestamp}});
    return json{
        {"task_id", task->task_id}, {"prompt", task->prompt},
        {"description", task->description ? json(*task->description) : json(nullptr)},
        {"status", std::string(claw::runtime::task_status_name(task->status))},
        {"task_packet", nullptr},
        {"created_at", task->created_at}, {"updated_at", task->updated_at},
        {"messages", msgs},
        {"team_id", task->team_id ? json(*task->team_id) : json(nullptr)},
        {"output", task->output}
    }.dump(2);
}

static tl::expected<std::string, std::string>
run_task_list(const json& /*input*/) {
    auto tasks = global_task_registry().list();
    json arr = json::array();
    for (auto& t : tasks)
        arr.push_back({{"task_id", t.task_id}, {"prompt", t.prompt},
                        {"status", std::string(claw::runtime::task_status_name(t.status))},
                        {"task_packet", nullptr}});
    return json{{"tasks", arr}, {"count", tasks.size()}}.dump(2);
}

static tl::expected<std::string, std::string>
run_task_stop(const json& input) {
    auto id = input.value("task_id", std::string{});
    auto r = global_task_registry().stop(id);
    if (!r) return tl::unexpected(r.error());
    return json{
        {"task_id", r->task_id},
        {"status", std::string(claw::runtime::task_status_name(r->status))},
        {"message", "Task stopped"}
    }.dump(2);
}

static tl::expected<std::string, std::string>
run_task_update(const json& input) {
    auto id = input.value("task_id", std::string{});
    auto message = input.value("message", std::string{});
    auto r = global_task_registry().update(id, message);
    if (!r) return tl::unexpected(r.error());
    return json{
        {"task_id", r->task_id},
        {"status", std::string(claw::runtime::task_status_name(r->status))},
        {"message_count", r->messages.size()},
        {"last_message", r->messages.empty() ? json(nullptr) : json(r->messages.back().content)}
    }.dump(2);
}

static tl::expected<std::string, std::string>
run_task_output(const json& input) {
    auto id = input.value("task_id", std::string{});
    auto r = global_task_registry().output(id);
    if (!r) return tl::unexpected(r.error());
    return json{{"task_id", id}, {"output", *r}}.dump(2);
}

// ── Helper: serialize Worker to JSON ──────────────────────────────────────

static json worker_to_json(const claw::runtime::Worker& w) {
    json events = json::array();
    for (auto& e : w.events) {
        events.push_back({
            {"seq", e.seq},
            {"kind", static_cast<int>(e.kind)},
            {"status", std::string(claw::runtime::worker_status_name(e.status))},
            {"detail", e.detail ? json(*e.detail) : json(nullptr)},
            {"timestamp", e.timestamp_epoch}
        });
    }
    return {
        {"worker_id", w.worker_id},
        {"cwd", w.cwd},
        {"status", std::string(claw::runtime::worker_status_name(w.status))},
        {"auto_trusted", w.auto_trusted},
        {"prompt", w.prompt ? json(*w.prompt) : json(nullptr)},
        {"replay_prompt", w.replay_prompt ? json(*w.replay_prompt) : json(nullptr)},
        {"events", events}
    };
}

// ── Tool dispatch: worker registry ───────────────────────────────────────

static tl::expected<std::string, std::string>
run_worker_create(const json& input) {
    // Mirrors Rust WorkerCreateInput: cwd, trusted_roots, auto_recover_prompt_misdelivery
    auto cwd = input.value("cwd", std::string{});
    // trusted_roots and auto_recover are used by the registry internally
    auto w = global_worker_registry().create(cwd);
    return worker_to_json(w).dump(2);
}

static tl::expected<std::string, std::string>
run_worker_get(const json& input) {
    auto id = input.value("worker_id", std::string{});
    auto w = global_worker_registry().get(id);
    if (!w) return tl::unexpected(std::string("worker not found: ") + id);
    return worker_to_json(*w).dump(2);
}

static tl::expected<std::string, std::string>
run_worker_observe(const json& input) {
    auto id = input.value("worker_id", std::string{});
    auto screen_text = input.value("screen_text", std::string{});
    auto r = global_worker_registry().observe(id, screen_text);
    if (!r) return tl::unexpected(r.error());
    return worker_to_json(*r).dump(2);
}

static tl::expected<std::string, std::string>
run_worker_resolve_trust(const json& input) {
    auto id = input.value("worker_id", std::string{});
    bool approved = input.value("approved", true);
    auto r = global_worker_registry().resolve_trust(id, approved);
    if (!r) return tl::unexpected(r.error());
    return worker_to_json(*r).dump(2);
}

static tl::expected<std::string, std::string>
run_worker_await_ready(const json& input) {
    auto id = input.value("worker_id", std::string{});
    auto w = global_worker_registry().get(id);
    if (!w) return tl::unexpected(std::string("worker not found: ") + id);
    bool ready = (w->status == claw::runtime::WorkerStatus::ReadyForPrompt);
    bool blocked = (w->status == claw::runtime::WorkerStatus::TrustRequired);
    return json{
        {"worker_id", w->worker_id},
        {"status", std::string(claw::runtime::worker_status_name(w->status))},
        {"ready", ready},
        {"blocked", blocked},
        {"replay_prompt_ready", w->replay_prompt.has_value()}
    }.dump(2);
}

static tl::expected<std::string, std::string>
run_worker_send_prompt(const json& input) {
    auto id = input.value("worker_id", std::string{});
    std::optional<std::string> prompt;
    if (input.contains("prompt")) prompt = input["prompt"].get<std::string>();
    auto r = global_worker_registry().send_prompt(id, prompt);
    if (!r) return tl::unexpected(r.error());
    return worker_to_json(*r).dump(2);
}

static tl::expected<std::string, std::string>
run_worker_restart(const json& input) {
    auto id = input.value("worker_id", std::string{});
    auto r = global_worker_registry().restart(id);
    if (!r) return tl::unexpected(r.error());
    return worker_to_json(*r).dump(2);
}

static tl::expected<std::string, std::string>
run_worker_terminate(const json& input) {
    auto id = input.value("worker_id", std::string{});
    auto r = global_worker_registry().terminate(id);
    if (!r) return tl::unexpected(r.error());
    return worker_to_json(*r).dump(2);
}

// ── Tool dispatch: team registry ─────────────────────────────────────────

static tl::expected<std::string, std::string>
run_team_create(const json& input) {
    auto name = input.value("name", std::string{});
    std::vector<std::string> task_ids;
    if (input.contains("tasks")) {
        for (auto& t : input["tasks"]) {
            if (t.is_string()) task_ids.push_back(t.get<std::string>());
            else if (t.contains("task_id")) task_ids.push_back(t["task_id"].get<std::string>());
        }
    }
    auto team = global_team_registry().create(name, std::move(task_ids));
    // Assign team to each task
    for (auto& tid : team.task_ids)
        global_task_registry().assign_team(tid, team.team_id);
    return json{
        {"team_id", team.team_id}, {"name", team.name},
        {"task_count", team.task_ids.size()}, {"task_ids", team.task_ids},
        {"status", std::string(claw::runtime::team_status_name(team.status))},
        {"created_at", team.created_at}
    }.dump(2);
}

static tl::expected<std::string, std::string>
run_team_delete(const json& input) {
    auto id = input.value("team_id", std::string{});
    auto r = global_team_registry().delete_team(id);
    if (!r) return tl::unexpected(r.error());
    return json{
        {"team_id", r->team_id}, {"name", r->name},
        {"status", std::string(claw::runtime::team_status_name(r->status))},
        {"message", "Team deleted"}
    }.dump(2);
}

// ── Tool dispatch: cron registry ─────────────────────────────────────────

static tl::expected<std::string, std::string>
run_cron_create(const json& input) {
    auto schedule = input.value("schedule", std::string{});
    auto prompt = input.value("prompt", std::string{});
    std::optional<std::string_view> desc;
    std::string desc_str;
    if (input.contains("description")) {
        desc_str = input["description"].get<std::string>();
        desc = desc_str;
    }
    auto entry = global_cron_registry().create(schedule, prompt, desc);
    return json{
        {"cron_id", entry.cron_id}, {"schedule", entry.schedule},
        {"prompt", entry.prompt},
        {"description", entry.description ? json(*entry.description) : json(nullptr)},
        {"enabled", entry.enabled}, {"created_at", entry.created_at}
    }.dump(2);
}

static tl::expected<std::string, std::string>
run_cron_delete(const json& input) {
    auto id = input.value("cron_id", std::string{});
    auto r = global_cron_registry().delete_cron(id);
    if (!r) return tl::unexpected(r.error());
    return json{
        {"cron_id", r->cron_id}, {"schedule", r->schedule},
        {"status", "deleted"}, {"message", "Cron entry removed"}
    }.dump(2);
}

static tl::expected<std::string, std::string>
run_cron_list(const json& /*input*/) {
    auto entries = global_cron_registry().list(false);
    json arr = json::array();
    for (auto& e : entries) {
        arr.push_back({
            {"cron_id", e.cron_id}, {"schedule", e.schedule},
            {"prompt", e.prompt},
            {"description", e.description ? json(*e.description) : json(nullptr)},
            {"enabled", e.enabled}, {"run_count", e.run_count},
            {"last_run_at", e.last_run_at ? json(*e.last_run_at) : json(nullptr)},
            {"created_at", e.created_at}
        });
    }
    return json{{"crons", arr}, {"count", entries.size()}}.dump(2);
}

// ── Tool dispatch: LSP ───────────────────────────────────────────────────

static tl::expected<std::string, std::string>
run_lsp(const json& input) {
    auto action_str = input.value("action", std::string{});
    auto action = claw::runtime::lsp_action_from_str(action_str);
    if (!action) {
        return tl::unexpected(std::string("unknown LSP action: ") + action_str);
    }

    // Detect language from path extension
    std::string language;
    if (input.contains("path")) {
        auto path = input["path"].get<std::string>();
        auto ext_pos = path.rfind('.');
        if (ext_pos != std::string::npos) {
            auto lang = claw::runtime::language_for_extension(path.substr(ext_pos));
            if (lang) language = *lang;
        }
        if (language.empty()) {
            auto detected = global_lsp_registry().find_server_for_path(path);
            if (detected) language = *detected;
        }
    }

    if (language.empty())
        return tl::unexpected(std::string("cannot determine language for LSP action"));

    auto r = global_lsp_registry().dispatch(language, *action, input);
    if (!r) {
        return json{
            {"action", action_str}, {"error", r.error()}, {"status", "error"}
        }.dump(2);
    }
    return r->dump(2);
}

// ── Tool dispatch: MCP ───────────────────────────────────────────────────

static tl::expected<std::string, std::string>
run_list_mcp_resources(const json& input) {
    auto server = input.value("server", std::string{"default"});
    auto r = global_mcp_registry().list_resources(server);
    if (!r) {
        return json{
            {"server", server}, {"resources", json::array()}, {"error", r.error()}
        }.dump(2);
    }
    json resources = json::array();
    for (auto& res : *r) {
        resources.push_back({
            {"uri", res.uri},
            {"name", res.name ? json(*res.name) : json(nullptr)},
            {"description", res.description ? json(*res.description) : json(nullptr)},
            {"mime_type", res.mime_type ? json(*res.mime_type) : json(nullptr)}
        });
    }
    return json{{"server", server}, {"resources", resources}, {"count", resources.size()}}.dump(2);
}

static tl::expected<std::string, std::string>
run_read_mcp_resource(const json& input) {
    auto server = input.value("server", std::string{"default"});
    auto uri = input.value("uri", std::string{});
    auto r = global_mcp_registry().read_resource(server, uri);
    if (!r) {
        return json{{"server", server}, {"uri", uri}, {"error", r.error()}}.dump(2);
    }
    return json{{"server", server}, {"uri", uri}, {"result", "ok"}}.dump(2);
}

static tl::expected<std::string, std::string>
run_mcp_auth(const json& input) {
    auto server = input.value("server", std::string{});
    auto state = global_mcp_registry().get_server(server);
    if (!state) {
        return json{
            {"server", server}, {"status", "disconnected"},
            {"message", "Server not registered. Use MCP tool to connect first."}
        }.dump(2);
    }
    std::string status_str;
    switch (state->status) {
        case claw::runtime::McpConnectionStatus::Connected:    status_str = "connected"; break;
        case claw::runtime::McpConnectionStatus::Connecting:   status_str = "connecting"; break;
        case claw::runtime::McpConnectionStatus::AuthRequired: status_str = "auth_required"; break;
        case claw::runtime::McpConnectionStatus::Error:        status_str = "error"; break;
        default:                                               status_str = "disconnected"; break;
    }
    return json{
        {"server", server}, {"status", status_str},
        {"tool_count", state->tools.size()},
        {"resource_count", state->resources.size()}
    }.dump(2);
}

static tl::expected<std::string, std::string>
run_mcp_tool(const json& input) {
    auto server = input.value("server", std::string{});
    auto tool = input.value("tool", std::string{});
    json args = input.contains("arguments") ? input["arguments"] : json::object();
    auto qualified = server + "/" + tool;
    auto r = global_mcp_registry().call_tool(qualified, args);
    if (!r) {
        return json{
            {"server", server}, {"tool", tool},
            {"error", r.error()}, {"status", "error"}
        }.dump(2);
    }
    // Serialize content array
    json content_arr = json::array();
    for (auto& c : r->content) {
        json item = {{"kind", c.kind}};
        for (auto& [k, v] : c.data) item[k] = v;
        content_arr.push_back(item);
    }
    return json{
        {"server", server}, {"tool", tool},
        {"content", content_arr},
        {"isError", r->is_error.value_or(false)},
        {"status", "success"}
    }.dump(2);
}

// ── execute_tool_with_enforcer ────────────────────────────────────────────

tl::expected<std::string, std::string>
execute_tool_with_enforcer(const claw::runtime::PermissionEnforcer* enforcer,
                            const std::string& name,
                            const json& input)
{
    // Helper: enforce permission if enforcer is set
    auto maybe_enforce = [&]() -> tl::expected<void, std::string> {
        if (enforcer) return enforce_permission_check(*enforcer, name, input);
        return {};
    };

    // ── file / bash built-ins ─────────────────────────────────────────────

    if (name == "bash") {
        if (auto r = maybe_enforce(); !r) return tl::unexpected(r.error());
        return run_bash(input);
    }
    if (name == "read_file") {
        if (auto r = maybe_enforce(); !r) return tl::unexpected(r.error());
        return run_read_file(input);
    }
    if (name == "write_file") {
        if (auto r = maybe_enforce(); !r) return tl::unexpected(r.error());
        return run_write_file(input);
    }
    if (name == "edit_file") {
        if (auto r = maybe_enforce(); !r) return tl::unexpected(r.error());
        return run_edit_file(input);
    }
    if (name == "glob_search") {
        if (auto r = maybe_enforce(); !r) return tl::unexpected(r.error());
        return run_glob_search(input);
    }
    if (name == "grep_search") {
        if (auto r = maybe_enforce(); !r) return tl::unexpected(r.error());
        return run_grep_search(input);
    }
    if (name == "PowerShell") {
        return run_powershell(input);
    }

    // ── Task tools ────────────────────────────────────────────────────────
    if (name == "TaskCreate")    return run_task_create(input);
    if (name == "RunTaskPacket") return run_task_packet(input);
    if (name == "TaskGet")       return run_task_get(input);
    if (name == "TaskList")      return run_task_list(input);
    if (name == "TaskStop")      return run_task_stop(input);
    if (name == "TaskUpdate")    return run_task_update(input);
    if (name == "TaskOutput")    return run_task_output(input);

    // ── Worker tools ─────────────────────────────────────────────────────
    if (name == "WorkerCreate")       return run_worker_create(input);
    if (name == "WorkerGet")          return run_worker_get(input);
    if (name == "WorkerObserve")      return run_worker_observe(input);
    if (name == "WorkerResolveTrust") return run_worker_resolve_trust(input);
    if (name == "WorkerAwaitReady")   return run_worker_await_ready(input);
    if (name == "WorkerSendPrompt")   return run_worker_send_prompt(input);
    if (name == "WorkerRestart")      return run_worker_restart(input);
    if (name == "WorkerTerminate")    return run_worker_terminate(input);

    // ── Team / Cron ──────────────────────────────────────────────────────
    if (name == "TeamCreate") return run_team_create(input);
    if (name == "TeamDelete") return run_team_delete(input);
    if (name == "CronCreate") return run_cron_create(input);
    if (name == "CronDelete") return run_cron_delete(input);
    if (name == "CronList")   return run_cron_list(input);

    // ── LSP / MCP ────────────────────────────────────────────────────────
    if (name == "LSP")              return run_lsp(input);
    if (name == "ListMcpResources") return run_list_mcp_resources(input);
    if (name == "ReadMcpResource")  return run_read_mcp_resource(input);
    if (name == "McpAuth")          return run_mcp_auth(input);
    if (name == "MCP")              return run_mcp_tool(input);

    // ── Web tools ─────────────────────────────────────────────────────────
    if (name == "WebFetch") {
        WebFetchInput wfi{
            input.value("url", std::string{}),
            input.value("prompt", std::string{})
        };
        auto r = execute_web_fetch(wfi);
        if (!r) return tl::unexpected(r.error());
        return json{
            {"bytes", r->bytes}, {"code", r->code}, {"codeText", r->code_text},
            {"result", r->result}, {"durationMs", r->duration_ms}, {"url", r->url}
        }.dump(2);
    }
    if (name == "WebSearch") {
        WebSearchInput wsi;
        wsi.query = input.value("query", std::string{});
        if (input.contains("allowed_domains"))
            wsi.allowed_domains = input["allowed_domains"].get<std::vector<std::string>>();
        if (input.contains("blocked_domains"))
            wsi.blocked_domains = input["blocked_domains"].get<std::vector<std::string>>();
        auto r = execute_web_search(wsi);
        if (!r) return tl::unexpected(r.error());
        return json{
            {"query", r->query}, {"results", r->results},
            {"durationSeconds", r->duration_seconds}
        }.dump(2);
    }

    // ── TodoWrite ─────────────────────────────────────────────────────────
    if (name == "TodoWrite") {
        TodoWriteInput twi;
        for (auto& item : input.at("todos"))
            twi.todos.push_back(item.get<TodoItem>());
        auto r = execute_todo_write(std::move(twi));
        if (!r) return tl::unexpected(r.error());
        json old_j = json::array(), new_j = json::array();
        for (auto& t : r->old_todos) { json j; to_json(j, t); old_j.push_back(j); }
        for (auto& t : r->new_todos) { json j; to_json(j, t); new_j.push_back(j); }
        json out = {{"oldTodos", old_j}, {"newTodos", new_j}};
        if (r->verification_nudge_needed)
            out["verificationNudgeNeeded"] = *r->verification_nudge_needed;
        return out.dump(2);
    }

    // ── Skill ─────────────────────────────────────────────────────────────
    if (name == "Skill") {
        SkillInput si{
            input.value("skill", std::string{}),
            input.contains("args") ? std::optional<std::string>(input["args"].get<std::string>())
                                   : std::nullopt
        };
        auto r = execute_skill(std::move(si));
        if (!r) return tl::unexpected(r.error());
        return json{
            {"skill", r->skill}, {"path", r->path},
            {"args", r->args ? json(*r->args) : json(nullptr)},
            {"description", r->description ? json(*r->description) : json(nullptr)},
            {"prompt", r->prompt}
        }.dump(2);
    }

    // ── Agent ─────────────────────────────────────────────────────────────
    if (name == "Agent") {
        AgentInput ai{
            input.value("description", std::string{}),
            input.value("prompt", std::string{}),
            input.contains("subagent_type") ? std::optional<std::string>(input["subagent_type"]) : std::nullopt,
            input.contains("name") ? std::optional<std::string>(input["name"]) : std::nullopt,
            input.contains("model") ? std::optional<std::string>(input["model"]) : std::nullopt
        };
        auto r = execute_agent(std::move(ai));
        if (!r) return tl::unexpected(r.error());
        return to_json(*r).dump(2);
    }

    // ── ToolSearch ────────────────────────────────────────────────────────
    if (name == "ToolSearch") {
        ToolSearchInput tsi{
            input.value("query", std::string{}),
            input.contains("max_results")
                ? std::optional<std::size_t>(input["max_results"].get<std::size_t>())
                : std::nullopt
        };
        auto r = execute_tool_search(std::move(tsi));
        json j; to_json(j, r);
        return j.dump(2);
    }

    // ── NotebookEdit ──────────────────────────────────────────────────────
    if (name == "NotebookEdit") {
        NotebookEditInput nei;
        nei.notebook_path = input.value("notebook_path", std::string{});
        if (input.contains("cell_id"))    nei.cell_id    = input["cell_id"].get<std::string>();
        if (input.contains("new_source")) nei.new_source = input["new_source"].get<std::string>();
        if (input.contains("cell_type")) {
            auto ct = input["cell_type"].get<std::string>();
            nei.cell_type = (ct == "markdown") ? NotebookCellType::Markdown : NotebookCellType::Code;
        }
        if (input.contains("edit_mode")) {
            auto em = input["edit_mode"].get<std::string>();
            if (em == "insert")      nei.edit_mode = NotebookEditMode::Insert;
            else if (em == "delete") nei.edit_mode = NotebookEditMode::Delete;
            else                     nei.edit_mode = NotebookEditMode::Replace;
        }
        auto r = execute_notebook_edit(std::move(nei));
        if (!r) return tl::unexpected(r.error());
        json out = {{"new_source", r->new_source}, {"language", r->language}, {"edit_mode", r->edit_mode}};
        if (r->cell_id)   out["cell_id"]   = *r->cell_id;
        if (r->cell_type) out["cell_type"] = static_cast<int>(*r->cell_type);
        return out.dump(2);
    }

    // ── Sleep ──────────────────────────────────────────────────────────────
    if (name == "Sleep") {
        SleepInput si{input.value("duration_ms", uint64_t{0})};
        auto r = execute_sleep(std::move(si));
        if (!r) return tl::unexpected(r.error());
        return json{{"duration_ms", r->duration_ms}, {"message", r->message}}.dump(2);
    }

    // ── SendUserMessage / Brief ───────────────────────────────────────────
    if (name == "SendUserMessage" || name == "Brief") {
        BriefInput bi;
        bi.message = input.value("message", std::string{});
        if (input.contains("attachments"))
            bi.attachments = input["attachments"].get<std::vector<std::string>>();
        if (input.contains("status")) {
            auto s = input["status"].get<std::string>();
            bi.status = (s == "proactive") ? BriefStatus::Proactive : BriefStatus::Normal;
        }
        auto r = execute_brief(std::move(bi));
        if (!r) return tl::unexpected(r.error());
        json out = {{"message", r->message}, {"sent_at", r->sent_at}};
        if (r->attachments) {
            json arr = json::array();
            for (auto& a : *r->attachments)
                arr.push_back({{"path", a.path}, {"size", a.size}, {"is_image", a.is_image}});
            out["attachments"] = arr;
        }
        return out.dump(2);
    }

    // ── Config ────────────────────────────────────────────────────────────
    if (name == "Config") {
        ConfigInput ci;
        ci.setting = input.value("setting", std::string{});
        if (input.contains("value")) ci.value = input["value"];
        auto r = execute_config(std::move(ci));
        if (!r) return tl::unexpected(r.error());
        json out = {{"success", r->success}};
        if (r->operation)      out["operation"]       = *r->operation;
        if (r->setting)        out["setting"]          = *r->setting;
        if (r->value)          out["value"]            = *r->value;
        if (r->previous_value) out["previous_value"]   = *r->previous_value;
        if (r->new_value)      out["new_value"]        = *r->new_value;
        if (r->error)          out["error"]            = *r->error;
        return out.dump(2);
    }

    // ── EnterPlanMode ─────────────────────────────────────────────────────
    if (name == "EnterPlanMode") {
        auto r = execute_enter_plan_mode();
        if (!r) return tl::unexpected(r.error());
        json out = {
            {"success", r->success}, {"operation", r->operation},
            {"changed", r->changed}, {"active", r->active},
            {"managed", r->managed}, {"message", r->message},
            {"settings_path", r->settings_path}, {"state_path", r->state_path}
        };
        if (r->previous_local_mode) out["previous_local_mode"] = *r->previous_local_mode;
        if (r->current_local_mode)  out["current_local_mode"]  = *r->current_local_mode;
        return out.dump(2);
    }

    // ── ExitPlanMode ──────────────────────────────────────────────────────
    if (name == "ExitPlanMode") {
        auto r = execute_exit_plan_mode();
        if (!r) return tl::unexpected(r.error());
        json out = {
            {"success", r->success}, {"operation", r->operation},
            {"changed", r->changed}, {"active", r->active},
            {"managed", r->managed}, {"message", r->message},
            {"settings_path", r->settings_path}, {"state_path", r->state_path}
        };
        if (r->previous_local_mode) out["previous_local_mode"] = *r->previous_local_mode;
        if (r->current_local_mode)  out["current_local_mode"]  = *r->current_local_mode;
        return out.dump(2);
    }

    // ── StructuredOutput ──────────────────────────────────────────────────
    if (name == "StructuredOutput") {
        StructuredOutputInput soi;
        if (input.is_object()) {
            for (auto& [k, v] : input.items()) soi.data[k] = v;
        }
        auto r = execute_structured_output(std::move(soi));
        if (!r) return tl::unexpected(r.error());
        json out = {{"data_str", r->data_str}};
        json so = json::object();
        for (auto& [k, v] : r->structured_output) so[k] = v;
        out["structured_output"] = so;
        return out.dump(2);
    }

    // ── REPL ──────────────────────────────────────────────────────────────
    if (name == "REPL") {
        ReplInput ri;
        ri.code     = input.value("code", std::string{});
        ri.language = input.value("language", std::string{});
        if (input.contains("timeout_ms"))
            ri.timeout_ms = input["timeout_ms"].get<uint64_t>();
        auto r = execute_repl(std::move(ri));
        if (!r) return tl::unexpected(r.error());
        return json{
            {"language", r->language}, {"stdout", r->stdout_text},
            {"stderr", r->stderr_text}, {"exit_code", r->exit_code},
            {"duration_ms", r->duration_ms}
        }.dump(2);
    }

    // ── AskUserQuestion ───────────────────────────────────────────────────
    if (name == "AskUserQuestion") {
        AskUserQuestionInput aui;
        aui.question = input.value("question", std::string{});
        if (input.contains("options"))
            aui.options = input["options"].get<std::vector<std::string>>();
        auto r = run_ask_user_question(std::move(aui));
        if (!r) return tl::unexpected(r.error());
        return r->dump(2);
    }

    // ── RemoteTrigger ─────────────────────────────────────────────────────
    if (name == "RemoteTrigger") {
        RemoteTriggerInput rti;
        rti.url = input.value("url", std::string{});
        if (input.contains("method"))  rti.method  = input["method"].get<std::string>();
        if (input.contains("headers")) rti.headers = input["headers"];
        if (input.contains("body"))    rti.body    = input["body"].get<std::string>();
        auto r = run_remote_trigger(std::move(rti));
        if (!r) return tl::unexpected(r.error());
        return r->dump(2);
    }

    // ── TestingPermission (stub — mirrors Rust) ───────────────────────────
    if (name == "TestingPermission") {
        auto action = input.value("action", std::string{});
        return json{
            {"action", action}, {"permitted", true},
            {"message", "Testing permission tool stub"}
        }.dump(2);
    }

    // ── Unknown tool ──────────────────────────────────────────────────────
    return tl::unexpected(std::string("unknown tool: ") + name);
}

}  // namespace claw::tools
