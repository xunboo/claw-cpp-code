#include "agent_tools.hpp"
#include "tool_specs.hpp"
#include "summary_compression.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace claw::tools {

namespace fs = std::filesystem;

// ── JSON helpers for lane types ───────────────────────────────────────────────
// Now delegated to runtime::lane_events; kept as thin wrappers for tools code.

std::string_view lane_failure_class_name(LaneFailureClass fc) noexcept {
    return claw::runtime::lane_failure_class_wire(fc);
}

nlohmann::json to_json(const AgentOutput& ao) {
    nlohmann::json j = {
        {"agentId",     ao.agent_id},
        {"name",        ao.name},
        {"description", ao.description},
        {"status",      ao.status},
        {"outputFile",  ao.output_file},
        {"manifestFile",ao.manifest_file},
        {"createdAt",   ao.created_at}
    };
    if (ao.subagent_type) j["subagentType"] = *ao.subagent_type;
    else                  j["subagentType"] = nullptr;
    if (ao.model)         j["model"]        = *ao.model;
    else                  j["model"]        = nullptr;
    if (ao.started_at)    j["startedAt"]    = *ao.started_at;
    if (ao.completed_at)  j["completedAt"]  = *ao.completed_at;
    if (ao.error)         j["error"]        = *ao.error;
    if (ao.current_blocker) {
        nlohmann::json blocker_j;
        claw::runtime::to_json(blocker_j, *ao.current_blocker);
        j["currentBlocker"] = blocker_j;
    }

    auto events_arr = nlohmann::json::array();
    for (auto& ev : ao.lane_events) {
        nlohmann::json ev_j;
        claw::runtime::to_json(ev_j, ev);
        events_arr.push_back(ev_j);
    }
    j["laneEvents"] = events_arr;
    return j;
}

// ── Time helpers ──────────────────────────────────────────────────────────────

std::string iso8601_now() {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(secs);
}

std::string make_agent_id() {
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "agent-" + std::to_string(nanos);
}

std::string slugify_agent_name(const std::string& description) {
    std::string out;
    for (char ch : description) {
        if (std::isalnum(static_cast<unsigned char>(ch)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        else
            out += '-';
    }
    // collapse consecutive dashes
    std::string collapsed;
    bool prev_dash = false;
    for (char c : out) {
        if (c == '-') {
            if (!prev_dash) collapsed += c;
            prev_dash = true;
        } else {
            collapsed += c;
            prev_dash = false;
        }
    }
    // trim leading/trailing dashes
    auto s = collapsed.find_first_not_of('-');
    auto e = collapsed.find_last_not_of('-');
    if (s == std::string::npos) return {};
    collapsed = collapsed.substr(s, e - s + 1);
    // max 32 chars
    if (collapsed.size() > 32) collapsed.resize(32);
    return collapsed;
}

std::string normalize_subagent_type(const std::optional<std::string>& subagent_type) {
    std::string trimmed = subagent_type.value_or("");
    auto s = trimmed.find_first_not_of(" \t");
    if (s == std::string::npos) return "general-purpose";
    trimmed = trimmed.substr(s);
    auto e = trimmed.find_last_not_of(" \t");
    trimmed = trimmed.substr(0, e + 1);
    if (trimmed.empty()) return "general-purpose";

    auto ct = canonical_tool_token(trimmed);
    if (ct == "general" || ct == "generalpurpose" || ct == "generalpurposeagent")
        return "general-purpose";
    if (ct == "explore" || ct == "explorer" || ct == "exploreagent")
        return "Explore";
    if (ct == "plan" || ct == "planagent")
        return "Plan";
    if (ct == "verification" || ct == "verificationagent" || ct == "verify" || ct == "verifier")
        return "Verification";
    if (ct == "clawguide" || ct == "clawguideagent" || ct == "guide")
        return "claw-guide";
    if (ct == "statusline" || ct == "statuslinesetup")
        return "statusline-setup";
    return trimmed;
}

std::set<std::string> allowed_tools_for_subagent(const std::string& subagent_type) {
    std::vector<const char*> tools;
    if (subagent_type == "Explore") {
        tools = {"read_file", "glob_search", "grep_search", "WebFetch", "WebSearch",
                 "ToolSearch", "Skill", "StructuredOutput"};
    } else if (subagent_type == "Plan") {
        tools = {"read_file", "glob_search", "grep_search", "WebFetch", "WebSearch",
                 "ToolSearch", "Skill", "TodoWrite", "StructuredOutput", "SendUserMessage"};
    } else if (subagent_type == "Verification") {
        tools = {"bash", "read_file", "glob_search", "grep_search", "WebFetch", "WebSearch",
                 "ToolSearch", "TodoWrite", "StructuredOutput", "SendUserMessage", "PowerShell"};
    } else if (subagent_type == "claw-guide") {
        tools = {"read_file", "glob_search", "grep_search", "WebFetch", "WebSearch",
                 "ToolSearch", "Skill", "StructuredOutput", "SendUserMessage"};
    } else if (subagent_type == "statusline-setup") {
        tools = {"bash", "read_file", "write_file", "edit_file", "glob_search",
                 "grep_search", "ToolSearch"};
    } else {
        tools = {"bash", "read_file", "write_file", "edit_file", "glob_search", "grep_search",
                 "WebFetch", "WebSearch", "TodoWrite", "Skill", "ToolSearch", "NotebookEdit",
                 "Sleep", "SendUserMessage", "Config", "StructuredOutput", "REPL", "PowerShell"};
    }
    std::set<std::string> result;
    for (const char* t : tools) result.insert(t);
    return result;
}

// ── Lane failure classification ───────────────────────────────────────────────

LaneFailureClass classify_lane_failure(const std::string& error) {
    std::string n = error;
    std::transform(n.begin(), n.end(), n.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (n.find("prompt") != std::string::npos && n.find("deliver") != std::string::npos)
        return LaneFailureClass::PromptDelivery;
    if (n.find("trust") != std::string::npos)
        return LaneFailureClass::TrustGate;
    if (n.find("branch") != std::string::npos &&
        (n.find("stale") != std::string::npos || n.find("diverg") != std::string::npos))
        return LaneFailureClass::BranchDivergence;
    // GatewayRouting moved before Compile/Test (mirrors Rust reorder)
    if (n.find("gateway") != std::string::npos || n.find("routing") != std::string::npos)
        return LaneFailureClass::GatewayRouting;
    if (n.find("compile") != std::string::npos || n.find("build failed") != std::string::npos ||
        n.find("cargo check") != std::string::npos)
        return LaneFailureClass::Compile;
    if (n.find("test") != std::string::npos)
        return LaneFailureClass::Test;
    // ToolRuntime now uses narrower matching (mirrors Rust change)
    if (n.find("tool failed") != std::string::npos ||
        n.find("runtime tool") != std::string::npos ||
        n.find("tool runtime") != std::string::npos)
        return LaneFailureClass::ToolRuntime;
    if (n.find("plugin") != std::string::npos)
        return LaneFailureClass::PluginStartup;
    if (n.find("mcp") != std::string::npos && n.find("handshake") != std::string::npos)
        return LaneFailureClass::McpHandshake;
    if (n.find("mcp") != std::string::npos)
        return LaneFailureClass::McpStartup;
    return LaneFailureClass::Infra;
}

LaneBlocker classify_lane_blocker(const std::string& error) {
    return {classify_lane_failure(error), error};
}

// ── File helpers ──────────────────────────────────────────────────────────────

static tl::expected<fs::path, std::string> agent_store_dir() {
    if (const char* env = std::getenv("CLAWD_AGENT_STORE"))
        return fs::path(env);
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) return tl::unexpected("cannot get cwd: " + ec.message());
    // go up two ancestors (like Rust's cwd.ancestors().nth(2))
    auto p = cwd;
    int steps = 2;
    while (steps-- > 0 && p.has_parent_path())
        p = p.parent_path();
    return p / ".clawd-agents";
}

static tl::expected<void, std::string> write_agent_manifest(const AgentOutput& ao) {
    std::ofstream f(ao.manifest_file, std::ios::out | std::ios::trunc);
    if (!f) return tl::unexpected("cannot open manifest file: " + ao.manifest_file);
    f << to_json(ao).dump(2);
    return {};
}

static tl::expected<void, std::string>
append_agent_output(const std::string& path, const std::string& suffix) {
    std::ofstream f(path, std::ios::app);
    if (!f) return tl::unexpected("cannot open output file: " + path);
    f << suffix;
    return {};
}

static std::string format_agent_terminal_output(
    const std::string& status,
    const std::optional<std::string>& result,
    const std::optional<LaneEventBlocker>& blocker,
    const std::optional<std::string>& error)
{
    std::string out = "\n## Result\n\n- status: " + status + "\n";
    if (blocker) {
        out += "\n### Blocker\n\n- failure_class: ";
        out += lane_failure_class_name(blocker->failure_class);
        out += "\n- detail: " + blocker->detail + "\n";
    }
    if (result && !result->empty()) {
        std::string r = *result;
        while (!r.empty() && std::isspace(static_cast<unsigned char>(r.back()))) r.pop_back();
        out += "\n### Final response\n\n" + r + "\n";
    }
    if (error && !error->empty()) {
        std::string e = *error;
        while (!e.empty() && std::isspace(static_cast<unsigned char>(e.back()))) e.pop_back();
        out += "\n### Error\n\n" + e + "\n";
    }
    return out;
}

static tl::expected<void, std::string>
persist_agent_terminal_state(AgentOutput& manifest,
                              const std::string& status,
                              const std::optional<std::string>& result,
                              const std::optional<std::string>& error)
{
    std::optional<LaneEventBlocker> blocker;
    if (error) blocker = classify_lane_blocker(*error);

    auto suffix = format_agent_terminal_output(status, result, blocker, error);
    if (auto r = append_agent_output(manifest.output_file, suffix); !r)
        return tl::unexpected(r.error());

    manifest.status       = status;
    manifest.completed_at = iso8601_now();
    manifest.error        = error;
    manifest.current_blocker = blocker;

    if (blocker) {
        manifest.lane_events.push_back(
            LaneEvent::blocked(iso8601_now(), *blocker));
        manifest.lane_events.push_back(
            LaneEvent::failed(iso8601_now(), *blocker));
    } else {
        manifest.current_blocker = std::nullopt;
        // Compress detail from result, matching Rust's SummaryCompressor wire-up
        std::optional<std::string> compressed_detail;
        if (result && !result->empty()) {
            auto trimmed = *result;
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
                trimmed.pop_back();
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
                trimmed.erase(trimmed.begin());
            if (!trimmed.empty())
                compressed_detail = claw::runtime::compress_summary_text(trimmed);
        }
        manifest.lane_events.push_back(
            LaneEvent::finished(iso8601_now(), compressed_detail));
    }

    return write_agent_manifest(manifest);
}

// ── Main execute_agent ────────────────────────────────────────────────────────

tl::expected<AgentOutput, std::string> execute_agent(AgentInput input) {
    if (input.description.find_first_not_of(" \t\r\n") == std::string::npos)
        return tl::unexpected("description must not be empty");
    if (input.prompt.find_first_not_of(" \t\r\n") == std::string::npos)
        return tl::unexpected("prompt must not be empty");

    auto store_dir_result = agent_store_dir();
    if (!store_dir_result) return tl::unexpected(store_dir_result.error());
    auto& store_dir = *store_dir_result;

    std::error_code ec;
    fs::create_directories(store_dir, ec);
    if (ec) return tl::unexpected("cannot create agent store dir: " + ec.message());

    auto agent_id = make_agent_id();
    auto output_file   = (store_dir / (agent_id + ".md")).string();
    auto manifest_file = (store_dir / (agent_id + ".json")).string();
    auto normalized_type = normalize_subagent_type(input.subagent_type);

    std::string model = input.model.value_or("claude-opus-4-6");
    auto it = model.find_first_not_of(" \t");
    if (it == std::string::npos || model.substr(it).empty()) model = "claude-opus-4-6";

    auto agent_name = input.name
        ? slugify_agent_name(*input.name)
        : slugify_agent_name(input.description);
    if (agent_name.empty()) agent_name = "agent";

    auto created_at = iso8601_now();

    // Write initial output file
    {
        std::ofstream f(output_file, std::ios::out | std::ios::trunc);
        if (!f) return tl::unexpected("cannot create output file: " + output_file);
        f << "# Agent Task\n\n"
          << "- id: "           << agent_id       << "\n"
          << "- name: "         << agent_name      << "\n"
          << "- description: "  << input.description << "\n"
          << "- subagent_type: "<< normalized_type  << "\n"
          << "- created_at: "   << created_at      << "\n\n"
          << "## Prompt\n\n"   << input.prompt     << "\n";
    }

    AgentOutput manifest;
    manifest.agent_id     = agent_id;
    manifest.name         = agent_name;
    manifest.description  = input.description;
    manifest.subagent_type = normalized_type;
    manifest.model        = model;
    manifest.status       = "running";
    manifest.output_file  = output_file;
    manifest.manifest_file = manifest_file;
    manifest.created_at   = created_at;
    manifest.started_at   = created_at;
    manifest.lane_events  = {LaneEvent::started(iso8601_now())};

    if (auto r = write_agent_manifest(manifest); !r)
        return tl::unexpected(r.error());

    // Spawn a detached thread to "run" the agent (sub-agent execution is
    // delegated to the runtime crate in the full system; here we emit a
    // stub that marks the agent as completed immediately so callers can
    // observe the manifest being written).
    std::thread([manifest]() mutable {
        // The runtime integration would call build_agent_runtime / run_turn
        // here.  For the C++ port this is a placeholder that simply marks
        // the agent completed so the caller can read the manifest.
        (void)persist_agent_terminal_state(
            manifest, "completed",
            std::string{"[sub-agent spawned — runtime integration pending]"},
            std::nullopt);
    }).detach();

    return manifest;
}

}  // namespace claw::tools
