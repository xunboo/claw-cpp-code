#include "tool_specs.hpp"

#include <algorithm>
#include <set>
#include <sstream>

namespace claw::tools {

// ── Permission mode (forward declaration resolved inline) ─────────────────────
// PermissionMode values must match what runtime defines.
// We use integer representation here to avoid a circular dependency:
//   ReadOnly = 0, WorkspaceWrite = 1, DangerFullAccess = 2
static constexpr PermissionMode PM_READ_ONLY       = static_cast<PermissionMode>(0);
static constexpr PermissionMode PM_WORKSPACE_WRITE = static_cast<PermissionMode>(1);
static constexpr PermissionMode PM_DANGER_FULL     = static_cast<PermissionMode>(2);

std::vector<ToolSpec> mvp_tool_specs() {
    using j = nlohmann::json;
    return {
        {
            "bash",
            "Execute a shell command in the current workspace.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "command": { "type": "string" },
                    "timeout": { "type": "integer", "minimum": 1 },
                    "description": { "type": "string" },
                    "run_in_background": { "type": "boolean" },
                    "dangerouslyDisableSandbox": { "type": "boolean" },
                    "namespaceRestrictions": { "type": "boolean" },
                    "isolateNetwork": { "type": "boolean" },
                    "filesystemMode": { "type": "string", "enum": ["off", "workspace-only", "allow-list"] },
                    "allowedMounts": { "type": "array", "items": { "type": "string" } }
                },
                "required": ["command"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "read_file",
            "Read a text file from the workspace.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "path": { "type": "string" },
                    "offset": { "type": "integer", "minimum": 0 },
                    "limit": { "type": "integer", "minimum": 1 }
                },
                "required": ["path"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "write_file",
            "Write a text file in the workspace.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "path": { "type": "string" },
                    "content": { "type": "string" }
                },
                "required": ["path", "content"],
                "additionalProperties": false
            })"),
            PM_WORKSPACE_WRITE
        },
        {
            "edit_file",
            "Replace text in a workspace file.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "path": { "type": "string" },
                    "old_string": { "type": "string" },
                    "new_string": { "type": "string" },
                    "replace_all": { "type": "boolean" }
                },
                "required": ["path", "old_string", "new_string"],
                "additionalProperties": false
            })"),
            PM_WORKSPACE_WRITE
        },
        {
            "glob_search",
            "Find files by glob pattern.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "pattern": { "type": "string" },
                    "path": { "type": "string" }
                },
                "required": ["pattern"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "grep_search",
            "Search file contents with a regex pattern.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "pattern": { "type": "string" },
                    "path": { "type": "string" },
                    "glob": { "type": "string" },
                    "output_mode": { "type": "string" },
                    "-B": { "type": "integer", "minimum": 0 },
                    "-A": { "type": "integer", "minimum": 0 },
                    "-C": { "type": "integer", "minimum": 0 },
                    "context": { "type": "integer", "minimum": 0 },
                    "-n": { "type": "boolean" },
                    "-i": { "type": "boolean" },
                    "type": { "type": "string" },
                    "head_limit": { "type": "integer", "minimum": 1 },
                    "offset": { "type": "integer", "minimum": 0 },
                    "multiline": { "type": "boolean" }
                },
                "required": ["pattern"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "WebFetch",
            "Fetch a URL, convert it into readable text, and answer a prompt about it.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "url": { "type": "string", "format": "uri" },
                    "prompt": { "type": "string" }
                },
                "required": ["url", "prompt"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "WebSearch",
            "Search the web for current information and return cited results.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "query": { "type": "string", "minLength": 2 },
                    "allowed_domains": { "type": "array", "items": { "type": "string" } },
                    "blocked_domains": { "type": "array", "items": { "type": "string" } }
                },
                "required": ["query"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "TodoWrite",
            "Update the structured task list for the current session.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "todos": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "content": { "type": "string" },
                                "activeForm": { "type": "string" },
                                "status": { "type": "string", "enum": ["pending", "in_progress", "completed"] }
                            },
                            "required": ["content", "activeForm", "status"],
                            "additionalProperties": false
                        }
                    }
                },
                "required": ["todos"],
                "additionalProperties": false
            })"),
            PM_WORKSPACE_WRITE
        },
        {
            "Skill",
            "Load a local skill definition and its instructions.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "skill": { "type": "string" },
                    "args": { "type": "string" }
                },
                "required": ["skill"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "Agent",
            "Launch a specialized agent task and persist its handoff metadata.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "description": { "type": "string" },
                    "prompt": { "type": "string" },
                    "subagent_type": { "type": "string" },
                    "name": { "type": "string" },
                    "model": { "type": "string" }
                },
                "required": ["description", "prompt"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "ToolSearch",
            "Search for deferred or specialized tools by exact name or keywords.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "query": { "type": "string" },
                    "max_results": { "type": "integer", "minimum": 1 }
                },
                "required": ["query"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "NotebookEdit",
            "Replace, insert, or delete a cell in a Jupyter notebook.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "notebook_path": { "type": "string" },
                    "cell_id": { "type": "string" },
                    "new_source": { "type": "string" },
                    "cell_type": { "type": "string", "enum": ["code", "markdown"] },
                    "edit_mode": { "type": "string", "enum": ["replace", "insert", "delete"] }
                },
                "required": ["notebook_path"],
                "additionalProperties": false
            })"),
            PM_WORKSPACE_WRITE
        },
        {
            "Sleep",
            "Wait for a specified duration without holding a shell process.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "duration_ms": { "type": "integer", "minimum": 0 }
                },
                "required": ["duration_ms"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "SendUserMessage",
            "Send a message to the user.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "message": { "type": "string" },
                    "attachments": { "type": "array", "items": { "type": "string" } },
                    "status": { "type": "string", "enum": ["normal", "proactive"] }
                },
                "required": ["message", "status"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "Config",
            "Get or set Claude Code settings.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "setting": { "type": "string" },
                    "value": { "type": ["string", "boolean", "number"] }
                },
                "required": ["setting"],
                "additionalProperties": false
            })"),
            PM_WORKSPACE_WRITE
        },
        {
            "EnterPlanMode",
            "Enable a worktree-local planning mode override.",
            j::parse(R"({ "type": "object", "properties": {}, "additionalProperties": false })"),
            PM_WORKSPACE_WRITE
        },
        {
            "ExitPlanMode",
            "Restore or clear the worktree-local planning mode override.",
            j::parse(R"({ "type": "object", "properties": {}, "additionalProperties": false })"),
            PM_WORKSPACE_WRITE
        },
        {
            "StructuredOutput",
            "Return structured output in the requested format.",
            j::parse(R"({ "type": "object", "additionalProperties": true })"),
            PM_READ_ONLY
        },
        {
            "REPL",
            "Execute code in a REPL-like subprocess.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "code": { "type": "string" },
                    "language": { "type": "string" },
                    "timeout_ms": { "type": "integer", "minimum": 1 }
                },
                "required": ["code", "language"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "PowerShell",
            "Execute a PowerShell command with optional timeout.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "command": { "type": "string" },
                    "timeout": { "type": "integer", "minimum": 1 },
                    "description": { "type": "string" },
                    "run_in_background": { "type": "boolean" }
                },
                "required": ["command"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "AskUserQuestion",
            "Ask the user a question and wait for their response.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "question": { "type": "string" },
                    "options": { "type": "array", "items": { "type": "string" } }
                },
                "required": ["question"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "TaskCreate",
            "Create a background task that runs in a separate subprocess.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "prompt": { "type": "string" },
                    "description": { "type": "string" }
                },
                "required": ["prompt"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "TaskGet",
            "Get the status and details of a background task by ID.",
            j::parse(R"({ "type": "object", "properties": { "task_id": { "type": "string" } }, "required": ["task_id"], "additionalProperties": false })"),
            PM_READ_ONLY
        },
        {
            "TaskList",
            "List all background tasks and their current status.",
            j::parse(R"({ "type": "object", "properties": {}, "additionalProperties": false })"),
            PM_READ_ONLY
        },
        {
            "TaskStop",
            "Stop a running background task by ID.",
            j::parse(R"({ "type": "object", "properties": { "task_id": { "type": "string" } }, "required": ["task_id"], "additionalProperties": false })"),
            PM_DANGER_FULL
        },
        {
            "TaskUpdate",
            "Send a message or update to a running background task.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "task_id": { "type": "string" },
                    "message": { "type": "string" }
                },
                "required": ["task_id", "message"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "TaskOutput",
            "Retrieve the output produced by a background task.",
            j::parse(R"({ "type": "object", "properties": { "task_id": { "type": "string" } }, "required": ["task_id"], "additionalProperties": false })"),
            PM_READ_ONLY
        },
        {
            "WorkerCreate",
            "Create a coding worker boot session with trust-gate and prompt-delivery guards.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "cwd": { "type": "string" },
                    "trusted_roots": { "type": "array", "items": { "type": "string" } },
                    "auto_recover_prompt_misdelivery": { "type": "boolean" }
                },
                "required": ["cwd"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "WorkerGet",
            "Fetch the current worker boot state, last error, and event history.",
            j::parse(R"({ "type": "object", "properties": { "worker_id": { "type": "string" } }, "required": ["worker_id"], "additionalProperties": false })"),
            PM_READ_ONLY
        },
        {
            "WorkerObserve",
            "Feed a terminal snapshot into worker boot detection.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "worker_id": { "type": "string" },
                    "screen_text": { "type": "string" }
                },
                "required": ["worker_id", "screen_text"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "WorkerResolveTrust",
            "Resolve a detected trust prompt so worker boot can continue.",
            j::parse(R"({ "type": "object", "properties": { "worker_id": { "type": "string" } }, "required": ["worker_id"], "additionalProperties": false })"),
            PM_DANGER_FULL
        },
        {
            "WorkerAwaitReady",
            "Return the current ready-handshake verdict for a coding worker.",
            j::parse(R"({ "type": "object", "properties": { "worker_id": { "type": "string" } }, "required": ["worker_id"], "additionalProperties": false })"),
            PM_READ_ONLY
        },
        {
            "WorkerSendPrompt",
            "Send a task prompt only after the worker reaches ready_for_prompt.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "worker_id": { "type": "string" },
                    "prompt": { "type": "string" }
                },
                "required": ["worker_id"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "WorkerRestart",
            "Restart worker boot state after a failed or stale startup.",
            j::parse(R"({ "type": "object", "properties": { "worker_id": { "type": "string" } }, "required": ["worker_id"], "additionalProperties": false })"),
            PM_DANGER_FULL
        },
        {
            "WorkerTerminate",
            "Terminate a worker and mark the lane finished from the control plane.",
            j::parse(R"({ "type": "object", "properties": { "worker_id": { "type": "string" } }, "required": ["worker_id"], "additionalProperties": false })"),
            PM_DANGER_FULL
        },
        {
            "TeamCreate",
            "Create a team of sub-agents for parallel task execution.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "name": { "type": "string" },
                    "tasks": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "prompt": { "type": "string" },
                                "description": { "type": "string" }
                            },
                            "required": ["prompt"]
                        }
                    }
                },
                "required": ["name", "tasks"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "TeamDelete",
            "Delete a team and stop all its running tasks.",
            j::parse(R"({ "type": "object", "properties": { "team_id": { "type": "string" } }, "required": ["team_id"], "additionalProperties": false })"),
            PM_DANGER_FULL
        },
        {
            "CronCreate",
            "Create a scheduled recurring task.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "schedule": { "type": "string" },
                    "prompt": { "type": "string" },
                    "description": { "type": "string" }
                },
                "required": ["schedule", "prompt"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "CronDelete",
            "Delete a scheduled recurring task by ID.",
            j::parse(R"({ "type": "object", "properties": { "cron_id": { "type": "string" } }, "required": ["cron_id"], "additionalProperties": false })"),
            PM_DANGER_FULL
        },
        {
            "CronList",
            "List all scheduled recurring tasks.",
            j::parse(R"({ "type": "object", "properties": {}, "additionalProperties": false })"),
            PM_READ_ONLY
        },
        {
            "LSP",
            "Query Language Server Protocol for code intelligence.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "action": { "type": "string", "enum": ["symbols", "references", "diagnostics", "definition", "hover"] },
                    "path": { "type": "string" },
                    "line": { "type": "integer", "minimum": 0 },
                    "character": { "type": "integer", "minimum": 0 },
                    "query": { "type": "string" }
                },
                "required": ["action"],
                "additionalProperties": false
            })"),
            PM_READ_ONLY
        },
        {
            "ListMcpResources",
            "List available resources from connected MCP servers.",
            j::parse(R"({ "type": "object", "properties": { "server": { "type": "string" } }, "additionalProperties": false })"),
            PM_READ_ONLY
        },
        {
            "ReadMcpResource",
            "Read a specific resource from an MCP server by URI.",
            j::parse(R"({ "type": "object", "properties": { "server": { "type": "string" }, "uri": { "type": "string" } }, "required": ["uri"], "additionalProperties": false })"),
            PM_READ_ONLY
        },
        {
            "McpAuth",
            "Authenticate with an MCP server that requires OAuth or credentials.",
            j::parse(R"({ "type": "object", "properties": { "server": { "type": "string" } }, "required": ["server"], "additionalProperties": false })"),
            PM_DANGER_FULL
        },
        {
            "RemoteTrigger",
            "Trigger a remote action or webhook endpoint.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "url": { "type": "string" },
                    "method": { "type": "string", "enum": ["GET", "POST", "PUT", "DELETE"] },
                    "headers": { "type": "object" },
                    "body": { "type": "string" }
                },
                "required": ["url"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "MCP",
            "Execute a tool provided by a connected MCP server.",
            j::parse(R"({
                "type": "object",
                "properties": {
                    "server": { "type": "string" },
                    "tool": { "type": "string" },
                    "arguments": { "type": "object" }
                },
                "required": ["server", "tool"],
                "additionalProperties": false
            })"),
            PM_DANGER_FULL
        },
        {
            "TestingPermission",
            "Test-only tool for verifying permission enforcement behavior.",
            j::parse(R"({ "type": "object", "properties": { "action": { "type": "string" } }, "required": ["action"], "additionalProperties": false })"),
            PM_DANGER_FULL
        },
    };
}

std::vector<ToolSpec> deferred_tool_specs() {
    auto all = mvp_tool_specs();
    static const std::set<std::string_view> base_tools = {
        "bash", "read_file", "write_file", "edit_file", "glob_search", "grep_search"
    };
    all.erase(std::remove_if(all.begin(), all.end(),
        [](const ToolSpec& s) { return base_tools.contains(s.name); }),
        all.end());
    return all;
}

std::vector<ToolSpec>
tool_specs_for_allowed_tools(const std::set<std::string>* allowed_tools) {
    auto all = mvp_tool_specs();
    if (!allowed_tools) return all;
    all.erase(std::remove_if(all.begin(), all.end(),
        [&](const ToolSpec& s) { return !allowed_tools->contains(s.name); }),
        all.end());
    return all;
}

// ── normalize helpers ─────────────────────────────────────────────────────────

std::string normalize_tool_name(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '-') out.push_back('_');
        else           out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    // trim
    auto s = out.find_first_not_of(" \t");
    if (s == std::string::npos) return {};
    auto e = out.find_last_not_of(" \t");
    return out.substr(s, e - s + 1);
}

std::string canonical_tool_token(std::string_view value) {
    std::string out;
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)))
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (out.size() > 4 && out.ends_with("tool"))
        out.resize(out.size() - 4);
    return out;
}

std::string normalize_tool_search_query(const std::string& query) {
    std::istringstream ss(query);
    std::string term;
    std::vector<std::string> tokens;
    while (ss >> term)
        tokens.push_back(canonical_tool_token(term));
    std::string result;
    for (auto& t : tokens) { if (!result.empty()) result += ' '; result += t; }
    return result;
}

std::vector<std::string>
search_tool_specs(const std::string& query,
                  std::size_t max_results,
                  const std::vector<SearchableToolSpec>& specs)
{
    if (max_results == 0) max_results = 1;
    std::string lowered = query;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char c) { return std::tolower(c); });

    // select: prefix
    if (lowered.starts_with("select:")) {
        auto selection = lowered.substr(7);
        std::vector<std::string> result;
        std::istringstream ss(selection);
        std::string part;
        while (std::getline(ss, part, ',')) {
            auto wanted = canonical_tool_token(part);
            for (auto& spec : specs) {
                if (canonical_tool_token(spec.name) == wanted) {
                    result.push_back(spec.name);
                    break;
                }
            }
            if (result.size() >= max_results) break;
        }
        return result;
    }

    // parse required (+) vs optional terms
    std::vector<std::string> required, optional;
    std::istringstream ss(lowered);
    std::string term;
    while (ss >> term) {
        if (term.starts_with('+') && term.size() > 1)
            required.push_back(term.substr(1));
        else
            optional.push_back(term);
    }

    std::vector<std::pair<int32_t, std::string>> scored;
    for (auto& spec : specs) {
        std::string name    = spec.name;
        std::string name_lc = name;
        std::transform(name_lc.begin(), name_lc.end(), name_lc.begin(),
            [](unsigned char c) { return std::tolower(c); });
        std::string canonical_name = canonical_tool_token(name);
        std::string desc_lc = spec.description;
        std::transform(desc_lc.begin(), desc_lc.end(), desc_lc.begin(),
            [](unsigned char c) { return std::tolower(c); });
        std::string haystack = name_lc + " " + desc_lc + " " + canonical_name;
        std::string normalized_desc = normalize_tool_search_query(spec.description);
        std::string normalized_haystack = canonical_name + " " + normalized_desc;

        // check required terms
        bool pass = true;
        for (auto& req : required)
            if (haystack.find(req) == std::string::npos) { pass = false; break; }
        if (!pass) continue;

        int32_t score = 0;
        auto all_terms = required;
        all_terms.insert(all_terms.end(), optional.begin(), optional.end());
        for (auto& t : all_terms) {
            auto ct = canonical_tool_token(t);
            if (haystack.find(t) != std::string::npos) score += 2;
            if (name_lc == t)    score += 8;
            if (name_lc.find(t) != std::string::npos) score += 4;
            if (canonical_name == ct) score += 12;
            if (normalized_haystack.find(ct) != std::string::npos) score += 3;
        }
        if (score == 0 && !lowered.empty()) continue;
        scored.emplace_back(score, spec.name);
    }

    std::stable_sort(scored.begin(), scored.end(),
        [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });

    std::vector<std::string> result;
    for (std::size_t i = 0; i < scored.size() && i < max_results; ++i)
        result.push_back(scored[i].second);
    return result;
}

}  // namespace claw::tools
