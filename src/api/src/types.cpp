// ---------------------------------------------------------------------------
// types.cpp  -  JSON serialisation / deserialisation for all message types
// ---------------------------------------------------------------------------

#include "types.hpp"
#include <stdexcept>
#include <string>

namespace claw::api {

// ToolDefinition
void to_json(nlohmann::json& j, const ToolDefinition& t) {
    j = {{"name", t.name}, {"input_schema", t.input_schema}};
    if (t.description) j["description"] = *t.description;
}
void from_json(const nlohmann::json& j, ToolDefinition& t) {
    j.at("name").get_to(t.name);
    j.at("input_schema").get_to(t.input_schema);
    if (j.contains("description") && !j["description"].is_null())
        t.description = j["description"].get<std::string>();
}

// ToolChoice
void to_json(nlohmann::json& j, const ToolChoice& tc) {
    switch (tc.kind) {
        case ToolChoice::Kind::Auto: j = {{"type","auto"}}; break;
        case ToolChoice::Kind::Any:  j = {{"type","any"}};  break;
        case ToolChoice::Kind::Tool: j = {{"type","tool"},{"name",tc.name}}; break;
    }
}
void from_json(const nlohmann::json& j, ToolChoice& tc) {
    auto t = j.at("type").get<std::string>();
    if (t=="auto") tc.kind=ToolChoice::Kind::Auto;
    else if(t=="any") tc.kind=ToolChoice::Kind::Any;
    else { tc.kind=ToolChoice::Kind::Tool; j.at("name").get_to(tc.name); }
}

// ToolResultContentBlock
void to_json(nlohmann::json& j, const ToolResultContentBlock& b) {
    if (b.kind==ToolResultContentBlock::Kind::Text) j={{"type","text"},{"text",b.text}};
    else j={{"type","json"},{"value",b.value}};
}
void from_json(const nlohmann::json& j, ToolResultContentBlock& b) {
    auto t=j.at("type").get<std::string>();
    if(t=="text"){b.kind=ToolResultContentBlock::Kind::Text;j.at("text").get_to(b.text);}
    else{b.kind=ToolResultContentBlock::Kind::Json;j.at("value").get_to(b.value);}
}

// InputContentBlock
void to_json(nlohmann::json& j, const InputContentBlock& b) {
    switch(b.kind){
        case InputContentBlock::Kind::Text: j={{"type","text"},{"text",b.text}}; break;
        case InputContentBlock::Kind::ToolUse: j={{"type","tool_use"},{"id",b.id},{"name",b.name},{"input",b.input}}; break;
        case InputContentBlock::Kind::ToolResult:
            j={{"type","tool_result"},{"tool_use_id",b.tool_use_id},{"content",b.content}};
            if(b.is_error) j["is_error"]=true; break;
    }
}
void from_json(const nlohmann::json& j, InputContentBlock& b) {
    auto t=j.at("type").get<std::string>();
    if(t=="text"){b.kind=InputContentBlock::Kind::Text;j.at("text").get_to(b.text);}
    else if(t=="tool_use"){b.kind=InputContentBlock::Kind::ToolUse;j.at("id").get_to(b.id);j.at("name").get_to(b.name);j.at("input").get_to(b.input);}
    else if(t=="tool_result"){b.kind=InputContentBlock::Kind::ToolResult;j.at("tool_use_id").get_to(b.tool_use_id);j.at("content").get_to(b.content);b.is_error=j.value("is_error",false);}
}

// InputMessage
InputMessage InputMessage::user_text(std::string text) {
    InputMessage m; m.role="user";
    m.content.push_back(InputContentBlock::text_block(std::move(text)));
    return m;
}
InputMessage InputMessage::user_tool_result(std::string tool_use_id, std::string ct, bool is_error) {
    InputMessage m; m.role="user";
    InputContentBlock b; b.kind=InputContentBlock::Kind::ToolResult;
    b.tool_use_id=std::move(tool_use_id); b.is_error=is_error;
    ToolResultContentBlock tb; tb.kind=ToolResultContentBlock::Kind::Text; tb.text=std::move(ct);
    b.content.push_back(std::move(tb)); m.content.push_back(std::move(b)); return m;
}
void to_json(nlohmann::json& j,const InputMessage& m){j={{"role",m.role},{"content",m.content}};}
void from_json(const nlohmann::json& j,InputMessage& m){j.at("role").get_to(m.role);j.at("content").get_to(m.content);}

// MessageRequest
void to_json(nlohmann::json& j,const MessageRequest& r){
    j={{"model",r.model},{"max_tokens",r.max_tokens},{"messages",r.messages}};
    if(r.system) j["system"]=*r.system;
    if(r.tools) j["tools"]=*r.tools;
    if(r.tool_choice) j["tool_choice"]=*r.tool_choice;
    if(r.stream) j["stream"]=true;
}
void from_json(const nlohmann::json& j,MessageRequest& r){
    j.at("model").get_to(r.model);j.at("max_tokens").get_to(r.max_tokens);j.at("messages").get_to(r.messages);
    if(j.contains("system")&&!j["system"].is_null()) r.system=j["system"].get<std::string>();
    if(j.contains("tools")&&!j["tools"].is_null()) r.tools=j["tools"].get<std::vector<ToolDefinition>>();
    if(j.contains("tool_choice")&&!j["tool_choice"].is_null()) r.tool_choice=j["tool_choice"].get<ToolChoice>();
    r.stream=j.value("stream",false);
}
// Usage
void to_json(nlohmann::json& j,const Usage& u){
    j={{"input_tokens",u.input_tokens},{"output_tokens",u.output_tokens}};
    if(u.cache_creation_input_tokens) j["cache_creation_input_tokens"]=u.cache_creation_input_tokens;
    if(u.cache_read_input_tokens)     j["cache_read_input_tokens"]=u.cache_read_input_tokens;
}
void from_json(const nlohmann::json& j,Usage& u){
    j.at("input_tokens").get_to(u.input_tokens); j.at("output_tokens").get_to(u.output_tokens);
    u.cache_creation_input_tokens=j.value("cache_creation_input_tokens",uint32_t{0});
    u.cache_read_input_tokens    =j.value("cache_read_input_tokens",    uint32_t{0});
}

// OutputContentBlock
void to_json(nlohmann::json& j,const OutputContentBlock& b){
    switch(b.kind){
        case OutputContentBlock::Kind::Text: j={{"type","text"},{"text",b.text}}; break;
        case OutputContentBlock::Kind::ToolUse: j={{"type","tool_use"},{"id",b.id},{"name",b.name},{"input",b.input}}; break;
        case OutputContentBlock::Kind::Thinking:
            j={{"type","thinking"},{"thinking",b.text}};
            j["signature"]=b.signature ? nlohmann::json(*b.signature) : nlohmann::json(nullptr); break;
        case OutputContentBlock::Kind::RedactedThinking:
            j={{"type","redacted_thinking"},{"data",b.data}}; break;
    }
}
void from_json(const nlohmann::json& j,OutputContentBlock& b){
    auto t=j.at("type").get<std::string>();
    if(t=="text"){b.kind=OutputContentBlock::Kind::Text;j.at("text").get_to(b.text);}
    else if(t=="tool_use"){b.kind=OutputContentBlock::Kind::ToolUse;j.at("id").get_to(b.id);j.at("name").get_to(b.name);j.at("input").get_to(b.input);}
    else if(t=="thinking"){b.kind=OutputContentBlock::Kind::Thinking;b.text=j.value("thinking",std::string{});if(j.contains("signature")&&!j["signature"].is_null())b.signature=j["signature"].get<std::string>();}
    else if(t=="redacted_thinking"){b.kind=OutputContentBlock::Kind::RedactedThinking;j.at("data").get_to(b.data);}
}

// MessageResponse
void to_json(nlohmann::json& j,const MessageResponse& r){
    j={{"id",r.id},{"type",r.kind},{"role",r.role},{"content",r.content},{"model",r.model},{"usage",r.usage}};
    j["stop_reason"]=r.stop_reason?nlohmann::json(*r.stop_reason):nlohmann::json(nullptr);
    j["stop_sequence"]=r.stop_sequence?nlohmann::json(*r.stop_sequence):nlohmann::json(nullptr);
    if(r.request_id) j["request_id"]=*r.request_id;
}
void from_json(const nlohmann::json& j,MessageResponse& r){
    j.at("id").get_to(r.id); r.kind=j.value("type",std::string{"message"});
    j.at("role").get_to(r.role); j.at("content").get_to(r.content);
    j.at("model").get_to(r.model); j.at("usage").get_to(r.usage);
    if(j.contains("stop_reason")&&!j["stop_reason"].is_null()) r.stop_reason=j["stop_reason"].get<std::string>();
    if(j.contains("stop_sequence")&&!j["stop_sequence"].is_null()) r.stop_sequence=j["stop_sequence"].get<std::string>();
    if(j.contains("request_id")&&!j["request_id"].is_null()) r.request_id=j["request_id"].get<std::string>();
}

// ContentBlockDelta
void to_json(nlohmann::json& j,const ContentBlockDelta& d){
    switch(d.kind){
        case ContentBlockDelta::Kind::TextDelta:      j={{"type","text_delta"},      {"text",d.text}}; break;
        case ContentBlockDelta::Kind::InputJsonDelta: j={{"type","input_json_delta"},{"partial_json",d.partial_json}}; break;
        case ContentBlockDelta::Kind::ThinkingDelta:  j={{"type","thinking_delta"},  {"thinking",d.text}}; break;
        case ContentBlockDelta::Kind::SignatureDelta:  j={{"type","signature_delta"}, {"signature",d.text}}; break;
    }
}
void from_json(const nlohmann::json& j,ContentBlockDelta& d){
    auto t=j.at("type").get<std::string>();
    if(t=="text_delta"){d.kind=ContentBlockDelta::Kind::TextDelta;j.at("text").get_to(d.text);}
    else if(t=="input_json_delta"){d.kind=ContentBlockDelta::Kind::InputJsonDelta;j.at("partial_json").get_to(d.partial_json);}
    else if(t=="thinking_delta"){d.kind=ContentBlockDelta::Kind::ThinkingDelta;j.at("thinking").get_to(d.text);}
    else if(t=="signature_delta"){d.kind=ContentBlockDelta::Kind::SignatureDelta;j.at("signature").get_to(d.text);}
}

// StreamEvent
void from_json(const nlohmann::json& j,StreamEvent& e){
    auto t=j.at("type").get<std::string>();
    if(t=="message_start"){e.kind=StreamEvent::Kind::MessageStart;j.at("message").get_to(e.message_start.message);}
    else if(t=="message_delta"){
        e.kind=StreamEvent::Kind::MessageDelta;
        auto& dj=j.at("delta");
        if(dj.contains("stop_reason")&&!dj["stop_reason"].is_null()) e.message_delta.delta.stop_reason=dj["stop_reason"].get<std::string>();
        if(dj.contains("stop_sequence")&&!dj["stop_sequence"].is_null()) e.message_delta.delta.stop_sequence=dj["stop_sequence"].get<std::string>();
        j.at("usage").get_to(e.message_delta.usage);
    }
    else if(t=="content_block_start"){e.kind=StreamEvent::Kind::ContentBlockStart;j.at("index").get_to(e.content_block_start.index);j.at("content_block").get_to(e.content_block_start.content_block);}
    else if(t=="content_block_delta"){e.kind=StreamEvent::Kind::ContentBlockDelta;j.at("index").get_to(e.content_block_delta.index);j.at("delta").get_to(e.content_block_delta.delta);}
    else if(t=="content_block_stop"){e.kind=StreamEvent::Kind::ContentBlockStop;j.at("index").get_to(e.content_block_stop.index);}
    else if(t=="message_stop"){e.kind=StreamEvent::Kind::MessageStop;}
}
void to_json(nlohmann::json& j,const StreamEvent& e){
    switch(e.kind){
        case StreamEvent::Kind::MessageStart: j={{"type","message_start"},{"message",e.message_start.message}}; break;
        case StreamEvent::Kind::MessageDelta:{
            nlohmann::json d;
            d["stop_reason"]=e.message_delta.delta.stop_reason?nlohmann::json(*e.message_delta.delta.stop_reason):nullptr;
            d["stop_sequence"]=e.message_delta.delta.stop_sequence?nlohmann::json(*e.message_delta.delta.stop_sequence):nullptr;
            j={{"type","message_delta"},{"delta",d},{"usage",e.message_delta.usage}}; break;
        }
        case StreamEvent::Kind::ContentBlockStart: j={{"type","content_block_start"},{"index",e.content_block_start.index},{"content_block",e.content_block_start.content_block}}; break;
        case StreamEvent::Kind::ContentBlockDelta: j={{"type","content_block_delta"},{"index",e.content_block_delta.index},{"delta",e.content_block_delta.delta}}; break;
        case StreamEvent::Kind::ContentBlockStop:  j={{"type","content_block_stop"}, {"index",e.content_block_stop.index}}; break;
        case StreamEvent::Kind::MessageStop:       j={{"type","message_stop"}}; break;
    }
}

} // namespace claw::api
