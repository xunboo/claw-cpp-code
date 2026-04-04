#pragma once
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <variant>
#include <tl/expected.hpp>
#include <nlohmann/json.hpp>

namespace claw::runtime {

enum class LspActionKind {
    Definition,
    References,
    Hover,
    Completion,
    Diagnostics,
    Symbols,
    Rename,
};

[[nodiscard]] std::optional<LspActionKind> lsp_action_from_str(std::string_view s);
[[nodiscard]] std::string_view lsp_action_name(LspActionKind action);

struct LspDiagnostic {
    std::string path;
    std::size_t line{0};
    std::size_t character{0};
    std::string severity; // "error", "warning", "info", "hint"
    std::string message;
    std::optional<std::string> source;
};

struct LspLocation {
    std::string path;
    std::size_t line{0};
    std::size_t character{0};
};

struct LspHoverResult {
    std::string contents;
    std::optional<LspLocation> range_start;
};

struct LspCompletionItem {
    std::string label;
    std::optional<std::string> detail;
    std::optional<std::string> insert_text;
};

struct LspSymbol {
    std::string name;
    std::string kind;
    LspLocation location;
};

enum class LspServerStatus {
    Connected,
    Disconnected,
    Starting,
    Error,
};

struct LspServerState {
    std::string language;
    LspServerStatus status{LspServerStatus::Disconnected};
    std::optional<std::string> root_path;
    std::vector<std::string> capabilities;
    std::vector<LspDiagnostic> diagnostics;
};

// File extension → language mapping
[[nodiscard]] std::optional<std::string> language_for_extension(std::string_view ext);

class LspRegistry {
public:
    LspRegistry() = default;

    void register_server(std::string language, LspServerState state);
    [[nodiscard]] std::optional<LspServerState> get(std::string_view language) const;
    [[nodiscard]] std::optional<std::string> find_server_for_path(const std::string& path) const;
    [[nodiscard]] std::vector<LspServerState> list_servers() const;

    void add_diagnostics(std::string_view language, std::vector<LspDiagnostic> diags);
    [[nodiscard]] std::vector<LspDiagnostic> get_diagnostics(std::optional<std::string_view> path_filter = std::nullopt) const;
    void clear_diagnostics(std::optional<std::string_view> language = std::nullopt);

    void disconnect(std::string_view language);

    // Dispatch an LSP action; returns JSON result or placeholder
    [[nodiscard]] tl::expected<nlohmann::json, std::string>
        dispatch(std::string_view language, LspActionKind action, const nlohmann::json& params);

    [[nodiscard]] std::size_t size() const;

private:
    struct Inner {
        std::unordered_map<std::string, LspServerState> servers;
    };
    mutable std::mutex mutex_;
    Inner inner_;
};

} // namespace claw::runtime
