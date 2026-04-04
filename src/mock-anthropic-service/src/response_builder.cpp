#include "response_builder.hpp"
#include "scenario.hpp"

#include <chrono>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace claw::mock {

// ── Internal helpers ──────────────────────────────────────────────────────────

namespace {

std::string unique_message_id() {
    using namespace std::chrono;
    auto nanos = duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count();
    return std::format("msg_{}", nanos);
}

json usage_json(uint32_t input_tokens, uint32_t output_tokens) {
    return {{"input_tokens", input_tokens},
            {"cache_creation_input_tokens", 0},
            {"cache_read_input_tokens", 0},
            {"output_tokens", output_tokens}};
}

std::string http_response_str(std::string_view status, std::string_view content_type,
                               const std::string& body,
                               std::vector<std::pair<std::string_view,std::string_view>> extra) {
    std::string r;
    r.reserve(256 + body.size());
    r += std::format("HTTP/1.1 {}\r\ncontent-type: {}\r\n", status, content_type);
    for (auto& [n, v] : extra) r += std::format("{}: {}\r\n", n, v);
    r += std::format("content-length: {}\r\nconnection: close\r\n\r\n", body.size());
    r += body;
    return r;
}

void append_sse(std::string& buf, std::string_view event, const json& payload) {
    buf += std::format("event: {}\n", event);
    buf += std::format("data: {}\n", payload.dump());
    buf += '\n';
}

// ── Tool result extraction helpers ────────────────────────────────────────────

struct ToolResultInfo {
    std::string content;
    bool is_error;
};

// Represents a single content block in a message (simplified for extraction)
struct SimpleBlock {
    enum class Kind { Text, ToolUse, ToolResult } kind;
    std::string id, name, text, tool_use_id;
    bool is_error{false};
    std::string result_text;
};

// Detect PARITY_SCENARIO: token in the last user message of a JSON body
std::optional<Scenario> detect_scenario_impl(const json& req) {
    if (!req.contains("messages")) return std::nullopt;
    const auto& messages = req["messages"];
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (!it->contains("content")) continue;
        const auto& content = (*it)["content"];
        auto check_text = [](std::string_view text) -> std::optional<Scenario> {
            std::istringstream ss{std::string(text)};
            std::string token;
            while (ss >> token) {
                if (std::string_view sv = token; sv.starts_with(SCENARIO_PREFIX)) {
                    if (auto s = parse_scenario(sv.substr(SCENARIO_PREFIX.size())))
                        return s;
                }
            }
            return std::nullopt;
        };
        if (content.is_string()) {
            if (auto s = check_text(content.get<std::string>())) return s;
        } else if (content.is_array()) {
            for (auto jt = content.rbegin(); jt != content.rend(); ++jt) {
                if (jt->value("type", "") == "text")
                    if (auto s = check_text(jt->value("text", ""))) return s;
            }
        }
    }
    return std::nullopt;
}

// Get the most recent tool result text from messages
std::optional<ToolResultInfo> latest_tool_result(const json& req) {
    if (!req.contains("messages")) return std::nullopt;
    const auto& messages = req["messages"];
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (!it->contains("content")) continue;
        const auto& content = (*it)["content"];
        if (!content.is_array()) continue;
        for (auto jt = content.rbegin(); jt != content.rend(); ++jt) {
            if (jt->value("type", "") != "tool_result") continue;
            std::string text;
            bool is_error = jt->value("is_error", false);
            auto& c = (*jt)["content"];
            if (c.is_string()) {
                text = c.get<std::string>();
            } else if (c.is_array()) {
                for (auto& blk : c)
                    if (blk.value("type","") == "text")
                        text += blk.value("text","");
            }
            return ToolResultInfo{text, is_error};
        }
    }
    return std::nullopt;
}

// Get tool results keyed by tool name
std::unordered_map<std::string, ToolResultInfo> tool_results_by_name(const json& req) {
    std::unordered_map<std::string, std::string> id_to_name;
    std::unordered_map<std::string, ToolResultInfo> results;
    if (!req.contains("messages")) return results;
    const auto& messages = req["messages"];
    // First pass: collect tool_use ids -> names
    for (auto& msg : messages) {
        if (!msg.contains("content") || !msg["content"].is_array()) continue;
        for (auto& blk : msg["content"])
            if (blk.value("type","") == "tool_use")
                id_to_name[blk.value("id","")] = blk.value("name","");
    }
    // Second pass (reverse): collect tool_result
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (!it->contains("content") || !(*it)["content"].is_array()) continue;
        for (auto jt = (*it)["content"].rbegin(); jt != (*it)["content"].rend(); ++jt) {
            if (jt->value("type","") != "tool_result") continue;
            auto tool_use_id = jt->value("tool_use_id","");
            auto name_it = id_to_name.find(tool_use_id);
            const std::string& name = (name_it != id_to_name.end()) ? name_it->second : tool_use_id;
            std::string text;
            bool is_error = jt->value("is_error", false);
            auto& c = (*jt)["content"];
            if (c.is_string()) text = c.get<std::string>();
            else if (c.is_array())
                for (auto& b : c)
                    if (b.value("type","") == "text") text += b.value("text","");
            results.try_emplace(name, ToolResultInfo{text, is_error});
        }
    }
    return results;
}

std::string extract_read_content(const std::string& s) {
    try {
        auto j = json::parse(s);
        if (j.contains("file") && j["file"].contains("content")
                && j["file"]["content"].is_string())
            return j["file"]["content"].get<std::string>();
    } catch (...) {}
    return s;
}

std::size_t extract_num_matches(const std::string& s) {
    try {
        auto j = json::parse(s);
        if (j.contains("numMatches")) return j["numMatches"].get<std::size_t>();
    } catch (...) {}
    return 0;
}

std::string extract_file_path(const std::string& s) {
    try {
        auto j = json::parse(s);
        if (j.contains("filePath") && j["filePath"].is_string())
            return j["filePath"].get<std::string>();
    } catch (...) {}
    return s;
}

std::string extract_bash_stdout(const std::string& s) {
    try {
        auto j = json::parse(s);
        if (j.contains("stdout") && j["stdout"].is_string())
            return j["stdout"].get<std::string>();
    } catch (...) {}
    return s;
}

std::string extract_plugin_message(const std::string& s) {
    try {
        auto j = json::parse(s);
        if (j.contains("input") && j["input"].contains("message")
                && j["input"]["message"].is_string())
            return j["input"]["message"].get<std::string>();
    } catch (...) {}
    return s;
}

// ── SSE builders ──────────────────────────────────────────────────────────────

std::string streaming_text_sse() {
    std::string buf;
    append_sse(buf, "message_start", {{"type","message_start"},{"message",{
        {"id","msg_streaming_text"},{"type","message"},{"role","assistant"},
        {"content",json::array()},{"model",DEFAULT_MODEL},
        {"stop_reason",nullptr},{"stop_sequence",nullptr},
        {"usage",usage_json(11,0)}}}});
    append_sse(buf, "content_block_start", {{"type","content_block_start"},{"index",0},
        {"content_block",{{"type","text"},{"text",""}}}});
    append_sse(buf, "content_block_delta", {{"type","content_block_delta"},{"index",0},
        {"delta",{{"type","text_delta"},{"text","Mock streaming "}}}});
    append_sse(buf, "content_block_delta", {{"type","content_block_delta"},{"index",0},
        {"delta",{{"type","text_delta"},{"text","says hello from the parity harness."}}}});
    append_sse(buf, "content_block_stop", {{"type","content_block_stop"},{"index",0}});
    append_sse(buf, "message_delta", {{"type","message_delta"},
        {"delta",{{"stop_reason","end_turn"},{"stop_sequence",nullptr}}},
        {"usage",usage_json(11,8)}});
    append_sse(buf, "message_stop", {{"type","message_stop"}});
    return buf;
}

struct ToolUseSseSpec {
    std::string_view tool_id;
    std::string_view tool_name;
    std::vector<std::string_view> chunks;
};

std::string tool_uses_sse(const std::vector<ToolUseSseSpec>& tools) {
    std::string buf;
    std::string msg_id = tools.empty() ? "msg_tool_use"
        : std::format("msg_{}", tools.front().tool_id);
    append_sse(buf, "message_start", {{"type","message_start"},{"message",{
        {"id",msg_id},{"type","message"},{"role","assistant"},
        {"content",json::array()},{"model",DEFAULT_MODEL},
        {"stop_reason",nullptr},{"stop_sequence",nullptr},
        {"usage",usage_json(12,0)}}}});
    for (std::size_t i = 0; i < tools.size(); ++i) {
        const auto& tu = tools[i];
        append_sse(buf, "content_block_start", {{"type","content_block_start"},{"index",i},
            {"content_block",{{"type","tool_use"},{"id",tu.tool_id},
             {"name",tu.tool_name},{"input",json::object()}}}});
        for (auto chunk : tu.chunks)
            append_sse(buf, "content_block_delta", {{"type","content_block_delta"},{"index",i},
                {"delta",{{"type","input_json_delta"},{"partial_json",chunk}}}});
        append_sse(buf, "content_block_stop", {{"type","content_block_stop"},{"index",i}});
    }
    append_sse(buf, "message_delta", {{"type","message_delta"},
        {"delta",{{"stop_reason","tool_use"},{"stop_sequence",nullptr}}},
        {"usage",usage_json(12,4)}});
    append_sse(buf, "message_stop", {{"type","message_stop"}});
    return buf;
}

std::string tool_use_sse(std::string_view id, std::string_view name,
                          std::vector<std::string_view> chunks) {
    return tool_uses_sse({{id, name, std::move(chunks)}});
}

std::string final_text_sse(std::string_view text) {
    std::string buf;
    append_sse(buf, "message_start", {{"type","message_start"},{"message",{
        {"id",unique_message_id()},{"type","message"},{"role","assistant"},
        {"content",json::array()},{"model",DEFAULT_MODEL},
        {"stop_reason",nullptr},{"stop_sequence",nullptr},{"usage",usage_json(14,0)}}}});
    append_sse(buf, "content_block_start", {{"type","content_block_start"},{"index",0},
        {"content_block",{{"type","text"},{"text",""}}}});
    append_sse(buf, "content_block_delta", {{"type","content_block_delta"},{"index",0},
        {"delta",{{"type","text_delta"},{"text",text}}}});
    append_sse(buf, "content_block_stop", {{"type","content_block_stop"},{"index",0}});
    append_sse(buf, "message_delta", {{"type","message_delta"},
        {"delta",{{"stop_reason","end_turn"},{"stop_sequence",nullptr}}},
        {"usage",usage_json(14,7)}});
    append_sse(buf, "message_stop", {{"type","message_stop"}});
    return buf;
}

std::string final_text_sse_with_usage(std::string_view text,
                                       uint32_t input_tokens, uint32_t output_tokens) {
    std::string buf;
    append_sse(buf, "message_start", {{"type","message_start"},{"message",{
        {"id",unique_message_id()},{"type","message"},{"role","assistant"},
        {"content",json::array()},{"model",DEFAULT_MODEL},
        {"stop_reason",nullptr},{"stop_sequence",nullptr},
        {"usage",usage_json(input_tokens, 0)}}}});
    append_sse(buf, "content_block_start", {{"type","content_block_start"},{"index",0},
        {"content_block",{{"type","text"},{"text",""}}}});
    append_sse(buf, "content_block_delta", {{"type","content_block_delta"},{"index",0},
        {"delta",{{"type","text_delta"},{"text",text}}}});
    append_sse(buf, "content_block_stop", {{"type","content_block_stop"},{"index",0}});
    append_sse(buf, "message_delta", {{"type","message_delta"},
        {"delta",{{"stop_reason","end_turn"},{"stop_sequence",nullptr}}},
        {"usage",usage_json(input_tokens, output_tokens)}});
    append_sse(buf, "message_stop", {{"type","message_stop"}});
    return buf;
}

// ── Stream body dispatch ──────────────────────────────────────────────────────

std::string build_stream_body_impl(const json& req, Scenario scenario) {
    switch (scenario) {
    case Scenario::StreamingText:
        return streaming_text_sse();
    case Scenario::ReadFileRoundtrip:
        if (auto r = latest_tool_result(req))
            return final_text_sse(std::format("read_file roundtrip complete: {}",
                                              extract_read_content(r->content)));
        return tool_use_sse("toolu_read_fixture","read_file",
                            {R"({"path":"fixture.txt"})"});
    case Scenario::GrepChunkAssembly:
        if (auto r = latest_tool_result(req))
            return final_text_sse(std::format("grep_search matched {} occurrences",
                                              extract_num_matches(r->content)));
        return tool_use_sse("toolu_grep_fixture","grep_search",
                            {R"({"pattern":"par)",R"(ity","path":"fixture.txt")",
                             R"(,"output_mode":"count"})"});
    case Scenario::WriteFileAllowed:
        if (auto r = latest_tool_result(req))
            return final_text_sse(std::format("write_file succeeded: {}",
                                              extract_file_path(r->content)));
        return tool_use_sse("toolu_write_allowed","write_file",
                            {R"({"path":"generated/output.txt","content":"created by mock service\n"})"});
    case Scenario::WriteFileDenied:
        if (auto r = latest_tool_result(req))
            return final_text_sse(std::format("write_file denied as expected: {}", r->content));
        return tool_use_sse("toolu_write_denied","write_file",
                            {R"({"path":"generated/denied.txt","content":"should not exist\n"})"});
    case Scenario::MultiToolTurnRoundtrip: {
        auto results = tool_results_by_name(req);
        auto ri = results.find("read_file"), gi = results.find("grep_search");
        if (ri != results.end() && gi != results.end())
            return final_text_sse(std::format("multi-tool roundtrip complete: {} / {} occurrences",
                extract_read_content(ri->second.content),
                extract_num_matches(gi->second.content)));
        return tool_uses_sse({
            {"toolu_multi_read","read_file",{R"({"path":"fixture.txt"})"}},
            {"toolu_multi_grep","grep_search",
             {R"({"pattern":"par)",R"(ity","path":"fixture.txt","output_mode":"count"})"}},
        });
    }
    case Scenario::BashStdoutRoundtrip:
        if (auto r = latest_tool_result(req))
            return final_text_sse(std::format("bash completed: {}",
                                              extract_bash_stdout(r->content)));
        return tool_use_sse("toolu_bash_stdout","bash",
                            {R"({"command":"printf 'alpha from bash'","timeout":1000})"});
    case Scenario::BashPermissionPromptApproved:
        if (auto r = latest_tool_result(req)) {
            if (r->is_error)
                return final_text_sse(std::format("bash approval unexpectedly failed: {}",r->content));
            return final_text_sse(std::format("bash approved and executed: {}",
                                              extract_bash_stdout(r->content)));
        }
        return tool_use_sse("toolu_bash_prompt_allow","bash",
                            {R"({"command":"printf 'approved via prompt'","timeout":1000})"});
    case Scenario::BashPermissionPromptDenied:
        if (auto r = latest_tool_result(req))
            return final_text_sse(std::format("bash denied as expected: {}",r->content));
        return tool_use_sse("toolu_bash_prompt_deny","bash",
                            {R"({"command":"printf 'should not run'","timeout":1000})"});
    case Scenario::PluginToolRoundtrip:
        if (auto r = latest_tool_result(req))
            return final_text_sse(std::format("plugin tool completed: {}",
                                              extract_plugin_message(r->content)));
        return tool_use_sse("toolu_plugin_echo","plugin_echo",
                            {R"({"message":"hello from plugin parity"})"});
    case Scenario::AutoCompactTriggered:
        return final_text_sse_with_usage("auto compact parity complete.", 50000, 200);
    case Scenario::TokenCostReporting:
        return final_text_sse_with_usage("token cost reporting parity complete.", 1000, 500);
    }
    return {};
}

// ── Non-streaming JSON response dispatch ──────────────────────────────────────

json text_resp(std::string_view id, std::string_view text,
               uint32_t in=10, uint32_t out=6) {
    return {{"id",id},{"type","message"},{"role","assistant"},
            {"content",json::array({{{"type","text"},{"text",text}}})},
            {"model",DEFAULT_MODEL},{"stop_reason","end_turn"},
            {"usage",usage_json(in,out)}};
}

json tool_resp(std::string_view id,
               std::vector<std::tuple<std::string_view,std::string_view,json>> tools) {
    json content = json::array();
    for (auto& [tid, tname, tinput] : tools)
        content.push_back({{"type","tool_use"},{"id",tid},{"name",tname},{"input",tinput}});
    return {{"id",id},{"type","message"},{"role","assistant"},
            {"content",content},{"model",DEFAULT_MODEL},
            {"stop_reason","tool_use"},{"usage",usage_json(10,3)}};
}

json build_json_response_impl(const json& req, Scenario scenario) {
    switch (scenario) {
    case Scenario::StreamingText:
        return text_resp("msg_streaming_text","Mock streaming says hello from the parity harness.");
    case Scenario::ReadFileRoundtrip:
        if (auto r = latest_tool_result(req))
            return text_resp("msg_read_file_final",
                std::format("read_file roundtrip complete: {}",extract_read_content(r->content)));
        return tool_resp("msg_read_file_tool",{{"toolu_read_fixture","read_file",{{"path","fixture.txt"}}}});
    case Scenario::GrepChunkAssembly:
        if (auto r = latest_tool_result(req))
            return text_resp("msg_grep_final",
                std::format("grep_search matched {} occurrences",extract_num_matches(r->content)));
        return tool_resp("msg_grep_tool",{{"toolu_grep_fixture","grep_search",
            {{"pattern","parity"},{"path","fixture.txt"},{"output_mode","count"}}}});
    case Scenario::WriteFileAllowed:
        if (auto r = latest_tool_result(req))
            return text_resp("msg_write_allowed_final",
                std::format("write_file succeeded: {}",extract_file_path(r->content)));
        return tool_resp("msg_write_allowed_tool",{{"toolu_write_allowed","write_file",
            {{"path","generated/output.txt"},{"content","created by mock service\n"}}}});
    case Scenario::WriteFileDenied:
        if (auto r = latest_tool_result(req))
            return text_resp("msg_write_denied_final",
                std::format("write_file denied as expected: {}",r->content));
        return tool_resp("msg_write_denied_tool",{{"toolu_write_denied","write_file",
            {{"path","generated/denied.txt"},{"content","should not exist\n"}}}});
    case Scenario::MultiToolTurnRoundtrip: {
        auto results = tool_results_by_name(req);
        auto ri = results.find("read_file"), gi = results.find("grep_search");
        if (ri != results.end() && gi != results.end())
            return text_resp("msg_multi_tool_final",
                std::format("multi-tool roundtrip complete: {} / {} occurrences",
                    extract_read_content(ri->second.content),
                    extract_num_matches(gi->second.content)));
        return tool_resp("msg_multi_tool_start",{
            {"toolu_multi_read","read_file",{{"path","fixture.txt"}}},
            {"toolu_multi_grep","grep_search",{{"pattern","parity"},{"path","fixture.txt"},{"output_mode","count"}}},
        });
    }
    case Scenario::BashStdoutRoundtrip:
        if (auto r = latest_tool_result(req))
            return text_resp("msg_bash_stdout_final",
                std::format("bash completed: {}",extract_bash_stdout(r->content)));
        return tool_resp("msg_bash_stdout_tool",{{"toolu_bash_stdout","bash",
            {{"command","printf 'alpha from bash'"},{"timeout",1000}}}});
    case Scenario::BashPermissionPromptApproved:
        if (auto r = latest_tool_result(req)) {
            if (r->is_error)
                return text_resp("msg_bash_prompt_allow_error",
                    std::format("bash approval unexpectedly failed: {}",r->content));
            return text_resp("msg_bash_prompt_allow_final",
                std::format("bash approved and executed: {}",extract_bash_stdout(r->content)));
        }
        return tool_resp("msg_bash_prompt_allow_tool",{{"toolu_bash_prompt_allow","bash",
            {{"command","printf 'approved via prompt'"},{"timeout",1000}}}});
    case Scenario::BashPermissionPromptDenied:
        if (auto r = latest_tool_result(req))
            return text_resp("msg_bash_prompt_deny_final",
                std::format("bash denied as expected: {}",r->content));
        return tool_resp("msg_bash_prompt_deny_tool",{{"toolu_bash_prompt_deny","bash",
            {{"command","printf 'should not run'"},{"timeout",1000}}}});
    case Scenario::PluginToolRoundtrip:
        if (auto r = latest_tool_result(req))
            return text_resp("msg_plugin_tool_final",
                std::format("plugin tool completed: {}",extract_plugin_message(r->content)));
        return tool_resp("msg_plugin_tool_start",{{"toolu_plugin_echo","plugin_echo",
            {{"message","hello from plugin parity"}}}});
    case Scenario::AutoCompactTriggered:
        return text_resp("msg_auto_compact_triggered","auto compact parity complete.",50000,200);
    case Scenario::TokenCostReporting:
        return text_resp("msg_token_cost_reporting","token cost reporting parity complete.",1000,500);
    }
    return text_resp("msg_unknown","");
}

}  // anonymous namespace

// ── Public entry points ───────────────────────────────────────────────────────

std::optional<Scenario> detect_scenario(const json& req) {
    return detect_scenario_impl(req);
}

std::optional<Scenario> detect_scenario_from_json(const nlohmann::json& j) noexcept {
    try { return detect_scenario_impl(j); } catch (...) { return std::nullopt; }
}

std::string build_stream_body(const nlohmann::json& req, Scenario scenario) {
    return build_stream_body_impl(req, scenario);
}

nlohmann::json build_json_response(const nlohmann::json& req, Scenario scenario) {
    return build_json_response_impl(req, scenario);
}

std::string build_http_response(const json& req, Scenario scenario) {
    bool stream = req.value("stream", false);
    if (stream) {
        auto body = build_stream_body_impl(req, scenario);
        return http_response_str("200 OK", "text/event-stream", body,
                                 {{"x-request-id", request_id_for(scenario)}});
    }
    auto body = build_json_response_impl(req, scenario).dump();
    return http_response_str("200 OK", "application/json", body,
                             {{"request-id", request_id_for(scenario)}});
}

}  // namespace claw::mock
