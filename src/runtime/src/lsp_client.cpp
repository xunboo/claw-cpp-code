#include "lsp_client.hpp"
#include <filesystem>
#include <format>
#include <algorithm>

namespace claw::runtime {

std::optional<LspActionKind> lsp_action_from_str(std::string_view s) {
    if (s == "definition" || s == "goto_definition") return LspActionKind::Definition;
    if (s == "references" || s == "find_references") return LspActionKind::References;
    if (s == "hover")       return LspActionKind::Hover;
    if (s == "completion")  return LspActionKind::Completion;
    if (s == "diagnostics") return LspActionKind::Diagnostics;
    if (s == "symbols")     return LspActionKind::Symbols;
    if (s == "rename")      return LspActionKind::Rename;
    return std::nullopt;
}

std::string_view lsp_action_name(LspActionKind action) {
    switch (action) {
        case LspActionKind::Definition:   return "definition";
        case LspActionKind::References:   return "references";
        case LspActionKind::Hover:        return "hover";
        case LspActionKind::Completion:   return "completion";
        case LspActionKind::Diagnostics:  return "diagnostics";
        case LspActionKind::Symbols:      return "symbols";
        case LspActionKind::Rename:       return "rename";
    }
    return "unknown";
}

std::optional<std::string> language_for_extension(std::string_view ext) {
    // Strip leading dot
    if (!ext.empty() && ext.front() == '.') ext.remove_prefix(1);

    if (ext == "rs")  return "rust";
    if (ext == "ts" || ext == "tsx") return "typescript";
    if (ext == "js" || ext == "jsx") return "javascript";
    if (ext == "py")  return "python";
    if (ext == "go")  return "go";
    if (ext == "java") return "java";
    if (ext == "c" || ext == "h") return "c";
    if (ext == "cpp" || ext == "hpp" || ext == "cc" || ext == "cxx") return "cpp";
    if (ext == "rb")  return "ruby";
    if (ext == "lua") return "lua";
    return std::nullopt;
}

void LspRegistry::register_server(std::string language, LspServerState state) {
    std::lock_guard lock(mutex_);
    inner_.servers.emplace(std::move(language), std::move(state));
}

std::optional<LspServerState> LspRegistry::get(std::string_view language) const {
    std::lock_guard lock(mutex_);
    auto it = inner_.servers.find(std::string(language));
    if (it == inner_.servers.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> LspRegistry::find_server_for_path(const std::string& path) const {
    std::filesystem::path p(path);
    auto ext = p.extension().string();
    return language_for_extension(ext);
}

std::vector<LspServerState> LspRegistry::list_servers() const {
    std::lock_guard lock(mutex_);
    std::vector<LspServerState> result;
    for (const auto& [lang, state] : inner_.servers) {
        result.push_back(state);
    }
    return result;
}

void LspRegistry::add_diagnostics(std::string_view language, std::vector<LspDiagnostic> diags) {
    std::lock_guard lock(mutex_);
    auto it = inner_.servers.find(std::string(language));
    if (it == inner_.servers.end()) return;
    for (auto& d : diags) it->second.diagnostics.push_back(std::move(d));
}

std::vector<LspDiagnostic> LspRegistry::get_diagnostics(std::optional<std::string_view> path_filter) const {
    std::lock_guard lock(mutex_);
    std::vector<LspDiagnostic> result;
    for (const auto& [lang, state] : inner_.servers) {
        for (const auto& d : state.diagnostics) {
            if (!path_filter.has_value() || d.path == *path_filter) {
                result.push_back(d);
            }
        }
    }
    return result;
}

void LspRegistry::clear_diagnostics(std::optional<std::string_view> language) {
    std::lock_guard lock(mutex_);
    if (language.has_value()) {
        auto it = inner_.servers.find(std::string(*language));
        if (it != inner_.servers.end()) it->second.diagnostics.clear();
    } else {
        for (auto& [lang, state] : inner_.servers) state.diagnostics.clear();
    }
}

void LspRegistry::disconnect(std::string_view language) {
    std::lock_guard lock(mutex_);
    auto it = inner_.servers.find(std::string(language));
    if (it != inner_.servers.end()) it->second.status = LspServerStatus::Disconnected;
}

tl::expected<nlohmann::json, std::string>
LspRegistry::dispatch(std::string_view language, LspActionKind action, const nlohmann::json& params) {
    std::lock_guard lock(mutex_);
    auto it = inner_.servers.find(std::string(language));
    if (it == inner_.servers.end()) {
        return tl::unexpected(std::format("no LSP server for language: {}", language));
    }
    if (it->second.status != LspServerStatus::Connected) {
        return tl::unexpected(std::format("LSP server for {} is not connected", language));
    }

    // Return cached diagnostics for diagnostics action
    if (action == LspActionKind::Diagnostics) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& d : it->second.diagnostics) {
            arr.push_back({{"path", d.path}, {"line", d.line}, {"severity", d.severity}, {"message", d.message}});
        }
        return arr;
    }

    // Placeholder for other actions (real implementation would send LSP requests)
    return nlohmann::json{{"placeholder", true}, {"action", std::string(lsp_action_name(action))}};
}

std::size_t LspRegistry::size() const {
    std::lock_guard lock(mutex_);
    return inner_.servers.size();
}

} // namespace claw::runtime
