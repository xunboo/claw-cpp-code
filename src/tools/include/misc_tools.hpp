#pragma once
#include <tl/expected.hpp>

#include <nlohmann/json.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace claw::tools {

// ── TodoWrite ─────────────────────────────────────────────────────────────────

enum class TodoStatus { Pending, InProgress, Completed };

struct TodoItem {
    std::string content;
    std::string active_form;
    TodoStatus  status{TodoStatus::Pending};
};

void to_json(nlohmann::json& j, const TodoItem& t);
void from_json(const nlohmann::json& j, TodoItem& t);
void to_json(nlohmann::json& j, TodoStatus s);
void from_json(const nlohmann::json& j, TodoStatus& s);

struct TodoWriteInput {
    std::vector<TodoItem> todos;
};

struct TodoWriteOutput {
    std::vector<TodoItem>     old_todos;
    std::vector<TodoItem>     new_todos;
    std::optional<bool>       verification_nudge_needed;
};

[[nodiscard]] tl::expected<TodoWriteOutput, std::string>
    execute_todo_write(TodoWriteInput input);

// ── Skill ─────────────────────────────────────────────────────────────────────

struct SkillInput {
    std::string                skill;
    std::optional<std::string> args;
};

struct SkillOutput {
    std::string                skill;
    std::string                path;
    std::optional<std::string> args;
    std::optional<std::string> description;
    std::string                prompt;
};

[[nodiscard]] tl::expected<SkillOutput, std::string>
    execute_skill(SkillInput input);

// ── ToolSearch ────────────────────────────────────────────────────────────────

struct ToolSearchInput {
    std::string  query;
    std::optional<std::size_t> max_results;
};

[[nodiscard]] struct ToolSearchOutput
    execute_tool_search(ToolSearchInput input);

// ── NotebookEdit ──────────────────────────────────────────────────────────────

enum class NotebookCellType { Code, Markdown };
enum class NotebookEditMode { Replace, Insert, Delete };

struct NotebookEditInput {
    std::string                       notebook_path;
    std::optional<std::string>        cell_id;
    std::optional<std::string>        new_source;
    std::optional<NotebookCellType>   cell_type;
    std::optional<NotebookEditMode>   edit_mode;
};

struct NotebookEditOutput {
    std::string                     new_source;
    std::optional<std::string>      cell_id;
    std::optional<NotebookCellType> cell_type;
    std::string                     language;
    std::string                     edit_mode;
    std::optional<std::string>      error;
    std::string                     notebook_path;
    std::string                     original_file;
    std::string                     updated_file;
};

[[nodiscard]] tl::expected<NotebookEditOutput, std::string>
    execute_notebook_edit(NotebookEditInput input);

// ── Sleep ─────────────────────────────────────────────────────────────────────

struct SleepInput  { uint64_t duration_ms{0}; };
struct SleepOutput { uint64_t duration_ms{0}; std::string message; };

[[nodiscard]] tl::expected<SleepOutput, std::string>
    execute_sleep(SleepInput input);

// ── SendUserMessage (Brief) ───────────────────────────────────────────────────

enum class BriefStatus { Normal, Proactive };

struct ResolvedAttachment {
    std::string path;
    uint64_t    size{0};
    bool        is_image{false};
};

struct BriefInput {
    std::string                        message;
    std::optional<std::vector<std::string>> attachments;
    BriefStatus                        status{BriefStatus::Normal};
};

struct BriefOutput {
    std::string                               message;
    std::optional<std::vector<ResolvedAttachment>> attachments;
    std::string                               sent_at;
};

[[nodiscard]] tl::expected<BriefOutput, std::string>
    execute_brief(BriefInput input);

// ── Config ────────────────────────────────────────────────────────────────────

struct ConfigInput {
    std::string                           setting;
    std::optional<nlohmann::json>         value; // string | bool | number
};

struct ConfigOutput {
    bool                          success{false};
    std::optional<std::string>    operation;
    std::optional<std::string>    setting;
    std::optional<nlohmann::json> value;
    std::optional<nlohmann::json> previous_value;
    std::optional<nlohmann::json> new_value;
    std::optional<std::string>    error;
};

[[nodiscard]] tl::expected<ConfigOutput, std::string>
    execute_config(ConfigInput input);

// ── EnterPlanMode / ExitPlanMode ──────────────────────────────────────────────

struct PlanModeOutput {
    bool                          success{false};
    std::string                   operation;
    bool                          changed{false};
    bool                          active{false};
    bool                          managed{false};
    std::string                   message;
    std::string                   settings_path;
    std::string                   state_path;
    std::optional<nlohmann::json> previous_local_mode;
    std::optional<nlohmann::json> current_local_mode;
};

[[nodiscard]] tl::expected<PlanModeOutput, std::string> execute_enter_plan_mode();
[[nodiscard]] tl::expected<PlanModeOutput, std::string> execute_exit_plan_mode();

// ── StructuredOutput ──────────────────────────────────────────────────────────

struct StructuredOutputInput {
    std::map<std::string, nlohmann::json> data;
};

struct StructuredOutputResult {
    std::string                           data_str;
    std::map<std::string, nlohmann::json> structured_output;
};

[[nodiscard]] tl::expected<StructuredOutputResult, std::string>
    execute_structured_output(StructuredOutputInput input);

// ── REPL ──────────────────────────────────────────────────────────────────────

struct ReplInput {
    std::string             code;
    std::string             language;
    std::optional<uint64_t> timeout_ms;
};

struct ReplOutput {
    std::string language;
    std::string stdout_text;
    std::string stderr_text;
    int         exit_code{0};
    uint64_t    duration_ms{0};
};

[[nodiscard]] tl::expected<ReplOutput, std::string>
    execute_repl(ReplInput input);

// ── AskUserQuestion ───────────────────────────────────────────────────────────

struct AskUserQuestionInput {
    std::string                        question;
    std::optional<std::vector<std::string>> options;
};

[[nodiscard]] tl::expected<nlohmann::json, std::string>
    run_ask_user_question(AskUserQuestionInput input);

// ── RemoteTrigger ─────────────────────────────────────────────────────────────

struct RemoteTriggerInput {
    std::string                    url;
    std::optional<std::string>     method;
    std::optional<nlohmann::json>  headers;
    std::optional<std::string>     body;
};

[[nodiscard]] tl::expected<nlohmann::json, std::string>
    run_remote_trigger(RemoteTriggerInput input);

}  // namespace claw::tools
