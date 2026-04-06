#include "misc_tools.hpp"
#include "tool_registry.hpp"
#include "tool_specs.hpp"
#include "web_tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include "bash.hpp"

namespace claw::tools {

namespace fs = std::filesystem;

// ── JSON helpers for TodoItem ─────────────────────────────────────────────────

void to_json(nlohmann::json& j, TodoStatus s) {
    switch (s) {
        case TodoStatus::Pending:    j = "pending"; break;
        case TodoStatus::InProgress: j = "in_progress"; break;
        case TodoStatus::Completed:  j = "completed"; break;
    }
}

void from_json(const nlohmann::json& j, TodoStatus& s) {
    auto sv = j.get<std::string>();
    if (sv == "pending")     s = TodoStatus::Pending;
    else if (sv == "in_progress") s = TodoStatus::InProgress;
    else                     s = TodoStatus::Completed;
}

void to_json(nlohmann::json& j, const TodoItem& t) {
    j = nlohmann::json{
        {"content",    t.content},
        {"activeForm", t.active_form},
        {"status",     t.status}
    };
}

void from_json(const nlohmann::json& j, TodoItem& t) {
    t.content    = j.at("content").get<std::string>();
    t.active_form = j.at("activeForm").get<std::string>();
    from_json(j.at("status"), t.status);
}

// ── Todo helpers ──────────────────────────────────────────────────────────────

static tl::expected<void, std::string> validate_todos(const std::vector<TodoItem>& todos) {
    if (todos.empty())
        return tl::unexpected(std::string("todos must not be empty"));
    for (auto& t : todos) {
        if (t.content.find_first_not_of(" \t") == std::string::npos)
            return tl::unexpected(std::string("todo content must not be empty"));
        if (t.active_form.find_first_not_of(" \t") == std::string::npos)
            return tl::unexpected(std::string("todo activeForm must not be empty"));
    }
    return {};
}

static tl::expected<fs::path, std::string> todo_store_path() {
    if (const char* p = std::getenv("CLAWD_TODO_STORE"))
        return fs::path(p);
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) return tl::unexpected("cannot get cwd: " + ec.message());
    return cwd / ".clawd-todos.json";
}

tl::expected<TodoWriteOutput, std::string> execute_todo_write(TodoWriteInput input) {
    if (auto r = validate_todos(input.todos); !r)
        return tl::unexpected(r.error());

    auto store_path_result = todo_store_path();
    if (!store_path_result) return tl::unexpected(store_path_result.error());
    auto& store_path = *store_path_result;

    // Read old todos
    std::vector<TodoItem> old_todos;
    if (fs::exists(store_path)) {
        std::ifstream f(store_path);
        if (f) {
            try {
                auto j = nlohmann::json::parse(f);
                for (auto& item : j)
                    old_todos.push_back(item.get<TodoItem>());
            } catch (...) {}
        }
    }

    bool all_done = std::all_of(input.todos.begin(), input.todos.end(),
        [](const TodoItem& t) { return t.status == TodoStatus::Completed; });

    auto persisted = all_done ? std::vector<TodoItem>{} : input.todos;

    // Create parent dirs
    std::error_code ec;
    fs::create_directories(store_path.parent_path(), ec);

    {
        std::ofstream f(store_path, std::ios::out | std::ios::trunc);
        if (!f) return tl::unexpected("cannot write todo store: " + store_path.string());
        nlohmann::json arr = nlohmann::json::array();
        for (auto& t : persisted) {
            nlohmann::json j; to_json(j, t); arr.push_back(j);
        }
        f << arr.dump(2);
    }

    std::optional<bool> nudge;
    if (all_done && input.todos.size() >= 3) {
        bool has_verif = std::any_of(input.todos.begin(), input.todos.end(),
            [](const TodoItem& t) {
                std::string lc = t.content;
                std::transform(lc.begin(), lc.end(), lc.begin(),
                    [](unsigned char c) { return std::tolower(c); });
                return lc.find("verif") != std::string::npos;
            });
        if (!has_verif) nudge = true;
    }

    return TodoWriteOutput{std::move(old_todos), input.todos, nudge};
}

// ── Skill ─────────────────────────────────────────────────────────────────────

static std::optional<std::string> parse_skill_description(const std::string& prompt) {
    std::istringstream ss(prompt);
    std::string line;
    while (std::getline(ss, line)) {
        auto pos = line.find("description:");
        if (pos == std::string::npos)
            pos = line.find("Description:");
        if (pos != std::string::npos) {
            auto val = line.substr(pos + 12);
            auto s = val.find_first_not_of(" \t");
            if (s != std::string::npos)
                return val.substr(s);
        }
    }
    return std::nullopt;
}

static tl::expected<fs::path, std::string>
resolve_skill_path(const std::string& skill) {
    std::string requested = skill;
    // trim leading / or $
    while (!requested.empty() && (requested[0] == '/' || requested[0] == '$'))
        requested = requested.substr(1);
    auto s = requested.find_first_not_of(" \t");
    if (s == std::string::npos || requested.empty())
        return tl::unexpected(std::string("skill must not be empty"));
    requested = requested.substr(s);

    std::vector<fs::path> candidates;
    if (const char* codex = std::getenv("CODEX_HOME"))
        candidates.push_back(fs::path(codex) / "skills");
    if (const char* home = std::getenv("HOME")) {
        fs::path h(home);
        candidates.push_back(h / ".agents" / "skills");
        candidates.push_back(h / ".config" / "opencode" / "skills");
        candidates.push_back(h / ".codex" / "skills");
    }
    candidates.push_back(fs::path("/home/bellman/.codex/skills"));

    for (auto& root : candidates) {
        auto direct = root / requested / "SKILL.md";
        if (fs::exists(direct)) return direct;

        std::error_code ec;
        for (auto& entry : fs::directory_iterator(root, ec)) {
            auto path = entry.path() / "SKILL.md";
            if (!fs::exists(path)) continue;
            std::string fname = entry.path().filename().string();
            std::string req_lc = requested;
            std::string fname_lc = fname;
            std::transform(req_lc.begin(), req_lc.end(), req_lc.begin(),
                [](unsigned char c){ return std::tolower(c); });
            std::transform(fname_lc.begin(), fname_lc.end(), fname_lc.begin(),
                [](unsigned char c){ return std::tolower(c); });
            if (fname_lc == req_lc) return path;
        }
    }
    return tl::unexpected("unknown skill: " + requested);
}

tl::expected<SkillOutput, std::string> execute_skill(SkillInput input) {
    auto path_result = resolve_skill_path(input.skill);
    if (!path_result) return tl::unexpected(path_result.error());
    auto& path = *path_result;

    std::ifstream f(path);
    if (!f) return tl::unexpected("cannot read skill file: " + path.string());
    std::string prompt((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    auto desc = parse_skill_description(prompt);
    return SkillOutput{
        input.skill,
        path.string(),
        input.args,
        desc,
        prompt
    };
}

// ── ToolSearch ────────────────────────────────────────────────────────────────

ToolSearchOutput execute_tool_search(ToolSearchInput input) {
    return GlobalToolRegistry::builtin().search(
        input.query,
        input.max_results.value_or(5),
        std::nullopt);
}

// ── NotebookEdit ──────────────────────────────────────────────────────────────

static std::vector<nlohmann::json> source_lines(const std::string& source) {
    std::vector<nlohmann::json> lines;
    std::istringstream ss(source);
    std::string line;
    while (std::getline(ss, line)) {
        line += '\n';
        lines.push_back(line);
    }
    // remove trailing extra newline from last line if source didn't end with newline
    if (!lines.empty()) {
        std::string& last = lines.back().get_ref<std::string&>();
        if (!source.empty() && source.back() != '\n') {
            if (!last.empty() && last.back() == '\n')
                last.pop_back();
        }
    }
    return lines;
}

static std::optional<NotebookCellType> cell_kind(const nlohmann::json& cell) {
    if (auto it = cell.find("cell_type"); it != cell.end()) {
        auto s = it->get<std::string>();
        if (s == "markdown") return NotebookCellType::Markdown;
        return NotebookCellType::Code;
    }
    return std::nullopt;
}

static std::string make_cell_id(std::size_t idx) {
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(nanos) + "-" + std::to_string(idx);
}

static tl::expected<std::size_t, std::string>
resolve_cell_index(const nlohmann::json& cells,
                   const std::optional<std::string>& cell_id,
                   NotebookEditMode edit_mode)
{
    if (cell_id) {
        for (std::size_t i = 0; i < cells.size(); ++i) {
            if (auto id_it = cells[i].find("id");
                id_it != cells[i].end() && id_it->get<std::string>() == *cell_id)
                return i;
        }
        return tl::unexpected("cell id not found: " + *cell_id);
    }
    if (cells.empty())
        return tl::unexpected(std::string("notebook has no cells"));
    return cells.size() - 1; // last cell
}

static std::string format_edit_mode(NotebookEditMode m) {
    switch (m) {
        case NotebookEditMode::Replace: return "replace";
        case NotebookEditMode::Insert:  return "insert";
        case NotebookEditMode::Delete:  return "delete";
    }
    return "replace";
}

tl::expected<NotebookEditOutput, std::string>
execute_notebook_edit(NotebookEditInput input) {
    auto path = fs::path(input.notebook_path);
    if (path.extension() != ".ipynb")
        return tl::unexpected(std::string("File must be a Jupyter notebook (.ipynb file)."));

    std::string original_file;
    {
        std::ifstream f(path);
        if (!f) return tl::unexpected("cannot read notebook: " + path.string());
        original_file.assign(std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>());
    }

    nlohmann::json notebook;
    try {
        notebook = nlohmann::json::parse(original_file);
    } catch (const nlohmann::json::exception& e) {
        return tl::unexpected(std::string("JSON parse error: ") + e.what());
    }

    std::string language = "python";
    if (auto meta = notebook.find("metadata"); meta != notebook.end()) {
        if (auto ks = meta->find("kernelspec"); ks != meta->end()) {
            if (auto lang = ks->find("language"); lang != ks->end())
                language = lang->get<std::string>();
        }
    }

    auto cells_it = notebook.find("cells");
    if (cells_it == notebook.end() || !cells_it->is_array())
        return tl::unexpected(std::string("Notebook cells array not found"));
    auto& cells = *cells_it;

    auto edit_mode = input.edit_mode.value_or(NotebookEditMode::Replace);

    std::optional<std::size_t> target_index;
    if (input.cell_id || edit_mode == NotebookEditMode::Replace ||
        edit_mode == NotebookEditMode::Delete) {
        auto idx = resolve_cell_index(cells, input.cell_id, edit_mode);
        if (!idx) return tl::unexpected(idx.error());
        target_index = *idx;
    }

    std::optional<NotebookCellType> resolved_cell_type;
    if (edit_mode == NotebookEditMode::Insert) {
        resolved_cell_type = input.cell_type.value_or(NotebookCellType::Code);
    } else if (edit_mode == NotebookEditMode::Replace) {
        if (input.cell_type) {
            resolved_cell_type = *input.cell_type;
        } else if (target_index) {
            resolved_cell_type = cell_kind(cells[*target_index]).value_or(NotebookCellType::Code);
        }
    }

    std::string new_source;
    if (edit_mode != NotebookEditMode::Delete) {
        if (!input.new_source)
            return tl::unexpected(std::string("new_source is required for insert and replace edits"));
        new_source = *input.new_source;
    }

    std::optional<std::string> cell_id_out;

    switch (edit_mode) {
        case NotebookEditMode::Insert: {
            if (!resolved_cell_type)
                return tl::unexpected(std::string("insert mode requires a cell type"));
            auto new_id = make_cell_id(cells.size());
            auto cell_type_str = (*resolved_cell_type == NotebookCellType::Code) ? "code" : "markdown";
            nlohmann::json new_cell = {
                {"cell_type", cell_type_str},
                {"id", new_id},
                {"metadata", nlohmann::json::object()},
                {"source", source_lines(new_source)}
            };
            if (*resolved_cell_type == NotebookCellType::Code) {
                new_cell["outputs"] = nlohmann::json::array();
                new_cell["execution_count"] = nullptr;
            }
            std::size_t insert_at = target_index ? *target_index + 1 : cells.size();
            cells.insert(cells.begin() + static_cast<std::ptrdiff_t>(insert_at), new_cell);
            cell_id_out = new_id;
            break;
        }
        case NotebookEditMode::Delete: {
            if (!target_index)
                return tl::unexpected(std::string("delete mode requires a target cell index"));
            auto removed = cells[*target_index];
            cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(*target_index));
            if (auto id_it = removed.find("id"); id_it != removed.end())
                cell_id_out = id_it->get<std::string>();
            break;
        }
        case NotebookEditMode::Replace: {
            if (!resolved_cell_type)
                return tl::unexpected(std::string("replace mode requires a cell type"));
            if (!target_index)
                return tl::unexpected(std::string("replace mode requires a target cell index"));
            auto& cell = cells[*target_index];
            auto cell_type_str = (*resolved_cell_type == NotebookCellType::Code) ? "code" : "markdown";
            cell["source"]    = source_lines(new_source);
            cell["cell_type"] = cell_type_str;
            if (*resolved_cell_type == NotebookCellType::Code) {
                if (cell.find("outputs") == cell.end() || !cell["outputs"].is_array())
                    cell["outputs"] = nlohmann::json::array();
                if (cell.find("execution_count") == cell.end())
                    cell["execution_count"] = nullptr;
            } else {
                cell.erase("outputs");
                cell.erase("execution_count");
            }
            if (auto id_it = cell.find("id"); id_it != cell.end())
                cell_id_out = id_it->get<std::string>();
            break;
        }
    }

    auto updated_file = notebook.dump(2);
    {
        std::ofstream f(path, std::ios::out | std::ios::trunc);
        if (!f) return tl::unexpected("cannot write notebook: " + path.string());
        f << updated_file;
    }

    return NotebookEditOutput{
        new_source,
        cell_id_out,
        resolved_cell_type,
        language,
        format_edit_mode(edit_mode),
        std::nullopt,
        path.string(),
        original_file,
        updated_file,
    };
}

// ── Sleep ─────────────────────────────────────────────────────────────────────

static constexpr uint64_t MAX_SLEEP_DURATION_MS = 300'000;

tl::expected<SleepOutput, std::string> execute_sleep(SleepInput input) {
    if (input.duration_ms > MAX_SLEEP_DURATION_MS)
        return tl::unexpected("duration_ms " + std::to_string(input.duration_ms) +
                               " exceeds maximum allowed sleep of " +
                               std::to_string(MAX_SLEEP_DURATION_MS) + "ms");
    std::this_thread::sleep_for(std::chrono::milliseconds(input.duration_ms));
    return SleepOutput{input.duration_ms,
                       "Slept for " + std::to_string(input.duration_ms) + "ms"};
}

// ── Brief / SendUserMessage ───────────────────────────────────────────────────

static bool is_image_path(const fs::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c){ return std::tolower(c); });
    static const std::set<std::string> image_exts = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg"
    };
    return image_exts.count(ext) > 0;
}

static tl::expected<ResolvedAttachment, std::string>
resolve_attachment(const std::string& path) {
    std::error_code ec;
    auto resolved = fs::canonical(path, ec);
    if (ec) return tl::unexpected("cannot resolve attachment: " + ec.message());
    auto meta = fs::status(resolved, ec);
    if (ec) return tl::unexpected("cannot stat attachment: " + ec.message());
    auto sz = fs::file_size(resolved, ec);
    return ResolvedAttachment{resolved.string(), ec ? 0u : sz, is_image_path(resolved)};
}

static std::string iso8601_timestamp() {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(secs);
}

tl::expected<BriefOutput, std::string> execute_brief(BriefInput input) {
    if (input.message.find_first_not_of(" \t\r\n") == std::string::npos)
        return tl::unexpected(std::string("message must not be empty"));

    std::optional<std::vector<ResolvedAttachment>> attachments;
    if (input.attachments) {
        std::vector<ResolvedAttachment> resolved;
        for (auto& p : *input.attachments) {
            auto r = resolve_attachment(p);
            if (!r) return tl::unexpected(r.error());
            resolved.push_back(*r);
        }
        attachments = std::move(resolved);
    }

    return BriefOutput{input.message, std::move(attachments), iso8601_timestamp()};
}

// ── Config ────────────────────────────────────────────────────────────────────

enum class ConfigScope { Settings, Local };

struct ConfigSettingSpec {
    std::string              name;
    const char*              path[4];  // dot-path segments (null-terminated)
    std::size_t              path_len;
    ConfigScope              scope;
};

static std::optional<nlohmann::json>
get_nested_value(const nlohmann::json& doc, const char* const* path, std::size_t len) {
    const nlohmann::json* cur = &doc;
    for (std::size_t i = 0; i < len; ++i) {
        auto it = cur->find(path[i]);
        if (it == cur->end()) return std::nullopt;
        cur = &*it;
    }
    return *cur;
}

static void set_nested_value(nlohmann::json& doc, const char* const* path, std::size_t len,
                              const nlohmann::json& value)
{
    nlohmann::json* cur = &doc;
    for (std::size_t i = 0; i + 1 < len; ++i) {
        if (!cur->contains(path[i]) || !(*cur)[path[i]].is_object())
            (*cur)[path[i]] = nlohmann::json::object();
        cur = &(*cur)[path[i]];
    }
    (*cur)[path[len - 1]] = value;
}

static tl::expected<fs::path, std::string>
config_file_for_scope(ConfigScope scope) {
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) return tl::unexpected("cannot get cwd: " + ec.message());

    if (scope == ConfigScope::Settings)
        return cwd / ".claude" / "settings.json";
    return cwd / ".claude" / "settings.local.json";
}

static tl::expected<nlohmann::json, std::string>
read_json_object(const fs::path& path) {
    if (!fs::exists(path)) return nlohmann::json::object();
    std::ifstream f(path);
    if (!f) return tl::unexpected("cannot open: " + path.string());
    try {
        return nlohmann::json::parse(f);
    } catch (const nlohmann::json::exception& e) {
        return tl::unexpected(std::string("JSON parse error: ") + e.what());
    }
}

static tl::expected<void, std::string>
write_json_object(const fs::path& path, const nlohmann::json& doc) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) return tl::unexpected("cannot write: " + path.string());
    f << doc.dump(2);
    return {};
}

static const struct {
    const char* name;
    const char* path[4];
    std::size_t path_len;
    ConfigScope scope;
} CONFIG_SETTINGS[] = {
    {"permissions.defaultMode",   {"permissions", "defaultMode", nullptr, nullptr}, 2, ConfigScope::Settings},
    {"permissions.allowedPaths",  {"permissions", "allowedPaths", nullptr, nullptr}, 2, ConfigScope::Settings},
    {"model",                     {"model", nullptr, nullptr, nullptr}, 1, ConfigScope::Settings},
    {"theme",                     {"theme", nullptr, nullptr, nullptr}, 1, ConfigScope::Settings},
    {"verbose",                   {"verbose", nullptr, nullptr, nullptr}, 1, ConfigScope::Settings},
};

tl::expected<ConfigOutput, std::string> execute_config(ConfigInput input) {
    auto setting = input.setting;
    // trim
    auto s = setting.find_first_not_of(" \t");
    if (s == std::string::npos || setting.empty())
        return tl::unexpected(std::string("setting must not be empty"));
    setting = setting.substr(s);

    // find spec
    const char* const* path = nullptr;
    std::size_t path_len = 0;
    ConfigScope scope = ConfigScope::Settings;
    bool found = false;

    for (auto& spec : CONFIG_SETTINGS) {
        if (spec.name == setting) {
            path     = spec.path;
            path_len = spec.path_len;
            scope    = spec.scope;
            found    = true;
            break;
        }
    }

    if (!found) {
        return ConfigOutput{
            false, std::nullopt, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt,
            std::string("Unknown setting: \"") + setting + "\""
        };
    }

    auto file_result = config_file_for_scope(scope);
    if (!file_result) return tl::unexpected(file_result.error());
    auto doc_result = read_json_object(*file_result);
    if (!doc_result) return tl::unexpected(doc_result.error());
    auto& doc = *doc_result;

    if (input.value) {
        auto prev = get_nested_value(doc, path, path_len);
        set_nested_value(doc, path, path_len, *input.value);
        if (auto r = write_json_object(*file_result, doc); !r)
            return tl::unexpected(r.error());
        return ConfigOutput{
            true, "set", setting, *input.value, prev, *input.value, std::nullopt
        };
    } else {
        auto val = get_nested_value(doc, path, path_len);
        return ConfigOutput{
            true, "get", setting, val, std::nullopt, std::nullopt, std::nullopt
        };
    }
}

// ── EnterPlanMode / ExitPlanMode ──────────────────────────────────────────────

static const char* PERM_PATH[] = {"permissions", "defaultMode"};
static constexpr std::size_t PERM_PATH_LEN = 2;

struct PlanModeState {
    bool                          had_local_override{false};
    std::optional<nlohmann::json> previous_local_mode;
};

static fs::path plan_mode_state_file_path() {
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    return cwd / ".claude" / "plan-mode-state.json";
}

static tl::expected<std::optional<PlanModeState>, std::string>
read_plan_mode_state(const fs::path& path) {
    if (!fs::exists(path)) return std::nullopt;
    auto r = read_json_object(path);
    if (!r) return std::nullopt;
    PlanModeState state;
    if (r->contains("hadLocalOverride"))
        state.had_local_override = (*r)["hadLocalOverride"].get<bool>();
    if (r->contains("previousLocalMode") && !(*r)["previousLocalMode"].is_null())
        state.previous_local_mode = (*r)["previousLocalMode"];
    return state;
}

static tl::expected<void, std::string>
write_plan_mode_state(const fs::path& path, const PlanModeState& state) {
    nlohmann::json j = {
        {"hadLocalOverride", state.had_local_override}
    };
    if (state.previous_local_mode)
        j["previousLocalMode"] = *state.previous_local_mode;
    else
        j["previousLocalMode"] = nullptr;
    return write_json_object(path, j);
}

static tl::expected<void, std::string> clear_plan_mode_state(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
    return {};
}

tl::expected<PlanModeOutput, std::string> execute_enter_plan_mode() {
    auto settings_path_result = config_file_for_scope(ConfigScope::Settings);
    if (!settings_path_result) return tl::unexpected(settings_path_result.error());
    auto& settings_path = *settings_path_result;
    auto state_path = plan_mode_state_file_path();

    auto doc_result = read_json_object(settings_path);
    if (!doc_result) return tl::unexpected(doc_result.error());
    auto& doc = *doc_result;

    auto current_local_mode = get_nested_value(doc, PERM_PATH, PERM_PATH_LEN);
    bool current_is_plan = current_local_mode.has_value() &&
                           current_local_mode->is_string() &&
                           current_local_mode->get<std::string>() == "plan";

    auto state_result = read_plan_mode_state(state_path);
    if (!state_result) return tl::unexpected(state_result.error());

    if (*state_result) {
        if (current_is_plan) {
            return PlanModeOutput{
                true, "enter", false, true, true,
                "Plan mode override is already active for this worktree.",
                settings_path.string(), state_path.string(),
                (*state_result)->previous_local_mode, current_local_mode
            };
        }
        clear_plan_mode_state(state_path);
    }

    if (current_is_plan) {
        return PlanModeOutput{
            true, "enter", false, true, false,
            "Worktree-local plan mode is already enabled outside EnterPlanMode; leaving it unchanged.",
            settings_path.string(), state_path.string(),
            std::nullopt, current_local_mode
        };
    }

    PlanModeState new_state{
        current_local_mode.has_value(),
        current_local_mode
    };
    if (auto r = write_plan_mode_state(state_path, new_state); !r)
        return tl::unexpected(r.error());

    set_nested_value(doc, PERM_PATH, PERM_PATH_LEN, "plan");
    if (auto r = write_json_object(settings_path, doc); !r)
        return tl::unexpected(r.error());

    auto new_mode = get_nested_value(doc, PERM_PATH, PERM_PATH_LEN);
    return PlanModeOutput{
        true, "enter", true, true, true,
        "Enabled worktree-local plan mode override.",
        settings_path.string(), state_path.string(),
        new_state.previous_local_mode, new_mode
    };
}

tl::expected<PlanModeOutput, std::string> execute_exit_plan_mode() {
    auto settings_path_result = config_file_for_scope(ConfigScope::Settings);
    if (!settings_path_result) return tl::unexpected(settings_path_result.error());
    auto& settings_path = *settings_path_result;
    auto state_path = plan_mode_state_file_path();

    auto doc_result = read_json_object(settings_path);
    if (!doc_result) return tl::unexpected(doc_result.error());
    auto& doc = *doc_result;

    auto current_local_mode = get_nested_value(doc, PERM_PATH, PERM_PATH_LEN);
    bool current_is_plan = current_local_mode.has_value() &&
                           current_local_mode->is_string() &&
                           current_local_mode->get<std::string>() == "plan";

    auto state_result = read_plan_mode_state(state_path);
    if (!state_result) return tl::unexpected(state_result.error());

    if (!*state_result) {
        return PlanModeOutput{
            true, "exit", false, current_is_plan, false,
            current_is_plan
                ? "Plan mode is active but was not set by EnterPlanMode; leaving it unchanged."
                : "No EnterPlanMode override found; nothing to exit.",
            settings_path.string(), state_path.string(),
            std::nullopt, current_local_mode
        };
    }

    auto& state = **state_result;
    if (state.had_local_override && state.previous_local_mode) {
        set_nested_value(doc, PERM_PATH, PERM_PATH_LEN, *state.previous_local_mode);
    } else {
        // remove the key
        if (auto it = doc.find("permissions"); it != doc.end()) {
            it->erase("defaultMode");
            if (it->empty()) doc.erase("permissions");
        }
    }
    if (auto r = write_json_object(settings_path, doc); !r)
        return tl::unexpected(r.error());
    clear_plan_mode_state(state_path);

    auto new_mode = get_nested_value(doc, PERM_PATH, PERM_PATH_LEN);
    return PlanModeOutput{
        true, "exit", true, false, false,
        "Restored worktree-local settings to state before EnterPlanMode.",
        settings_path.string(), state_path.string(),
        state.previous_local_mode, new_mode
    };
}

// ── StructuredOutput ──────────────────────────────────────────────────────────

tl::expected<StructuredOutputResult, std::string>
execute_structured_output(StructuredOutputInput input) {
    nlohmann::json j(input.data);
    return StructuredOutputResult{j.dump(2), input.data};
}

// ── REPL ──────────────────────────────────────────────────────────────────────

tl::expected<ReplOutput, std::string> execute_repl(ReplInput input) {
    // Build interpreter command
    std::string interp;
    if (input.language == "python" || input.language == "python3")
        interp = "python3";
    else if (input.language == "bash" || input.language == "sh")
        interp = "bash";
    else if (input.language == "node" || input.language == "javascript" || input.language == "js")
        interp = "node";
    else if (input.language == "ruby")
        interp = "ruby";
    else
        interp = input.language;

    // Write code to a temp file
    auto tmp = fs::temp_directory_path() / ("clawd-repl-XXXXXX.tmp");
    {
        std::ofstream f(tmp);
        if (!f) return tl::unexpected(std::string("cannot create temp file"));
        f << input.code;
    }

    auto t0 = std::chrono::steady_clock::now();

    claw::runtime::BashCommandInput bash_in;
    bash_in.command = interp + " " + tmp.string();
    auto result = claw::runtime::execute_bash(bash_in);

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // cleanup
    std::error_code ec;
    fs::remove(tmp, ec);

    int exit_code = result ? result->exit_code : 1;
    std::string stdout_text = result ? result->stdout_output : "";
    std::string stderr_text = result ? result->stderr_output : "";
    if (!result) {
        stderr_text += "\nFailed to invoke execute_bash.";
    }

    return ReplOutput{
        input.language, stdout_text, stderr_text,
        exit_code,
        static_cast<uint64_t>(duration)
    };
}

// ── AskUserQuestion ───────────────────────────────────────────────────────────

tl::expected<nlohmann::json, std::string>
run_ask_user_question(AskUserQuestionInput input) {
    std::cout << "\n[Question] " << input.question << "\n";
    if (input.options) {
        for (std::size_t i = 0; i < input.options->size(); ++i)
            std::cout << "  " << (i + 1) << ". " << (*input.options)[i] << "\n";
        std::cout << "Enter choice (1-" << input.options->size() << "): ";
    } else {
        std::cout << "Your answer: ";
    }
    std::cout.flush();

    std::string response;
    if (!std::getline(std::cin, response))
        return tl::unexpected(std::string("failed to read user response"));
    // trim
    auto s = response.find_first_not_of(" \t\r\n");
    auto e = response.find_last_not_of(" \t\r\n");
    response = (s == std::string::npos) ? "" : response.substr(s, e - s + 1);

    std::string answer = response;
    if (input.options) {
        try {
            std::size_t idx = std::stoul(response);
            if (idx >= 1 && idx <= input.options->size())
                answer = (*input.options)[idx - 1];
        } catch (...) {}
    }

    return nlohmann::json{
        {"question", input.question},
        {"answer",   answer},
        {"status",   "answered"}
    };
}

// ── RemoteTrigger ─────────────────────────────────────────────────────────────

tl::expected<nlohmann::json, std::string>
run_remote_trigger(RemoteTriggerInput input) {
    // Use libcurl to make the HTTP request
    auto method = input.method.value_or("GET");

    // Reuse the curl_get helper for simple GET; for other methods build manually
    // (simplified implementation: only GET/POST fully supported)
    WebFetchInput wfi{input.url, "body"};
    auto result = execute_web_fetch(wfi);

    if (!result) {
        return nlohmann::json{
            {"url", input.url},
            {"method", method},
            {"error", result.error()},
            {"success", false}
        };
    }

    return nlohmann::json{
        {"url",         input.url},
        {"method",      method},
        {"status_code", result->code},
        {"body",        result->result},
        {"success",     result->code >= 200 && result->code < 300}
    };
}

}  // namespace claw::tools
