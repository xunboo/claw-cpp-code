//
// compat_harness.cpp
// C++20 port of crates/compat-harness/src/lib.rs
//

#include "compat_harness.hpp"

#include <algorithm>
#include <cstdlib>
#include <tl/expected.hpp>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace claw {

// ===========================================================================
// Internal helpers (detail namespace)
// ===========================================================================

namespace detail {

// Read entire file into a string, or return an error_code.
static tl::expected<std::string, std::error_code>
read_file_to_string(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::in | std::ios::binary);
    if (!ifs) {
        return tl::unexpected(
            std::make_error_code(std::errc::no_such_file_or_directory));
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    if (ifs.fail() && !ifs.eof()) {
        return tl::unexpected(std::make_error_code(std::errc::io_error));
    }
    return ss.str();
}

// Call fn(line_view) for every line in src (strips '\r\n' and lone '\n').
template<typename Fn>
static void for_each_line(std::string_view src, Fn&& fn) {
    while (!src.empty()) {
        auto pos = src.find('\n');
        if (pos == std::string_view::npos) {
            fn(src);
            break;
        }
        auto line = src.substr(0, pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        fn(line);
        src.remove_prefix(pos + 1);
    }
}

[[nodiscard]] static constexpr bool sv_starts_with(std::string_view s,
                                                    std::string_view p) noexcept {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

[[nodiscard]] static constexpr bool sv_ends_with(std::string_view s,
                                                  std::string_view suf) noexcept {
    return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

[[nodiscard]] static constexpr std::string_view sv_trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
    return s;
}
// Parse a TypeScript import line and return the imported symbol names.
// Handles: import { Foo, Bar } from '...'  and  import Foo from '...'
[[nodiscard]] static std::vector<std::string>
imported_symbols(std::string_view line) {
    std::vector<std::string> result;
    if (!sv_starts_with(line, "import ")) return result;

    auto after_import = line.substr(7);
    auto from_pos     = after_import.find(" from ");
    auto before_from  = sv_trim(
        (from_pos != std::string_view::npos)
            ? after_import.substr(0, from_pos)
            : after_import);

    if (sv_starts_with(before_from, "{")) {
        auto inner = before_from;
        if (!inner.empty() && inner.front() == '{') inner.remove_prefix(1);
        if (!inner.empty() && inner.back()  == '}') inner.remove_suffix(1);
        inner = sv_trim(inner);
        while (!inner.empty()) {
            auto comma = inner.find(',');
            auto part  = sv_trim((comma != std::string_view::npos)
                                     ? inner.substr(0, comma)
                                     : inner);
            if (!part.empty()) {
                auto space = part.find_first_of(" \t");
                auto name  = (space != std::string_view::npos)
                                 ? part.substr(0, space)
                                 : part;
                if (!name.empty()) result.emplace_back(name);
            }
            if (comma == std::string_view::npos) break;
            inner.remove_prefix(comma + 1);
            inner = sv_trim(inner);
        }
        return result;
    }

    // Default import: first token before ','
    auto first = sv_trim(before_from);
    auto comma  = first.find(',');
    first = sv_trim((comma != std::string_view::npos)
                        ? first.substr(0, comma)
                        : first);
    if (!first.empty()) result.emplace_back(first);
    return result;
}

// Return the first identifier-like run (alphanumeric, '_', '-') in `line`.
[[nodiscard]] static std::optional<std::string>
first_identifier(std::string_view line) {
    std::string out;
    for (char ch : line) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-') {
            out.push_back(ch);
        } else if (!out.empty()) {
            break;
        }
    }
    if (out.empty()) return std::nullopt;
    return out;
}

// For `  name = feature(...)`, extract `name`.
[[nodiscard]] static std::optional<std::string>
first_assignment_identifier(std::string_view line) {
    auto trimmed = sv_trim(line);
    auto eq_pos  = trimmed.find('=');
    if (eq_pos == std::string_view::npos) return std::nullopt;
    return first_identifier(sv_trim(trimmed.substr(0, eq_pos)));
}

[[nodiscard]] static CommandRegistry
dedupe_commands(std::vector<CommandManifestEntry> entries) {
    std::vector<CommandManifestEntry> deduped;
    for (auto& entry : entries) {
        bool exists = std::any_of(deduped.begin(), deduped.end(),
            [&](const CommandManifestEntry& seen) {
                return seen.name == entry.name && seen.source == entry.source;
            });
        if (!exists) deduped.push_back(std::move(entry));
    }
    return CommandRegistry{std::move(deduped)};
}

[[nodiscard]] static ToolRegistry
dedupe_tools(std::vector<ToolManifestEntry> entries) {
    std::vector<ToolManifestEntry> deduped;
    for (auto& entry : entries) {
        bool exists = std::any_of(deduped.begin(), deduped.end(),
            [&](const ToolManifestEntry& seen) {
                return seen.name == entry.name && seen.source == entry.source;
            });
        if (!exists) deduped.push_back(std::move(entry));
    }
    return ToolRegistry{std::move(deduped)};
}

// Collect all candidate upstream repo roots, in priority order.
[[nodiscard]] static std::vector<std::filesystem::path>
upstream_repo_candidates(const std::filesystem::path& primary_root) {
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(primary_root);

    if (const char* env_val = std::getenv("CLAUDE_CODE_UPSTREAM")) {
        candidates.emplace_back(env_val);
    }

    auto current = primary_root;
    for (int depth = 0; depth < 4; ++depth) {
        auto parent = current.parent_path();
        if (parent == current) break;
        candidates.push_back(parent / "claw-code");
        candidates.push_back(parent / "clawd-code");
        current = parent;
    }

    candidates.push_back(primary_root / "reference-source" / "claw-code");
    candidates.push_back(primary_root / "vendor" / "claw-code");

    // Deduplicate, preserve order.
    std::vector<std::filesystem::path> deduped;
    for (auto& c : candidates) {
        if (std::find(deduped.begin(), deduped.end(), c) == deduped.end()) {
            deduped.push_back(std::move(c));
        }
    }
    return deduped;
}

[[nodiscard]] static std::filesystem::path
resolve_upstream_repo_root(const std::filesystem::path& primary_root) {
    for (auto& candidate : upstream_repo_candidates(primary_root)) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate / "src" / "commands.ts", ec)) {
            return candidate;
        }
    }
    return primary_root;
}

} // namespace detail

// ===========================================================================
// UpstreamPaths
// ===========================================================================

/*static*/
UpstreamPaths UpstreamPaths::from_repo_root(std::filesystem::path repo_root) {
    return UpstreamPaths{std::move(repo_root)};
}

/*static*/
UpstreamPaths UpstreamPaths::from_workspace_dir(const std::filesystem::path& workspace_dir) {
    std::error_code ec;
    auto canonical = std::filesystem::canonical(workspace_dir, ec);
    auto resolved  = ec ? workspace_dir : canonical;

    auto parent = resolved.parent_path();
    if (parent == resolved) {
        parent = resolved / "..";
    }

    auto repo_root = detail::resolve_upstream_repo_root(parent);
    return UpstreamPaths{std::move(repo_root)};
}

std::filesystem::path UpstreamPaths::commands_path() const {
    return repo_root_ / "src" / "commands.ts";
}

std::filesystem::path UpstreamPaths::tools_path() const {
    return repo_root_ / "src" / "tools.ts";
}

std::filesystem::path UpstreamPaths::cli_path() const {
    return repo_root_ / "src" / "entrypoints" / "cli.tsx";
}

// ===========================================================================
// extract_manifest
// ===========================================================================

tl::expected<ExtractedManifest, std::error_code>
extract_manifest(const UpstreamPaths& paths) {
    auto commands_src = detail::read_file_to_string(paths.commands_path());
    if (!commands_src) return tl::unexpected(commands_src.error());

    auto tools_src = detail::read_file_to_string(paths.tools_path());
    if (!tools_src) return tl::unexpected(tools_src.error());

    auto cli_src = detail::read_file_to_string(paths.cli_path());
    if (!cli_src) return tl::unexpected(cli_src.error());

    return ExtractedManifest{
        .commands  = extract_commands(*commands_src),
        .tools     = extract_tools(*tools_src),
        .bootstrap = extract_bootstrap_plan(*cli_src),
    };
}

// ===========================================================================
// extract_commands
// ===========================================================================

CommandRegistry extract_commands(std::string_view source) {
    std::vector<CommandManifestEntry> entries;
    bool in_internal_block = false;

    detail::for_each_line(source, [&](std::string_view raw_line) {
        auto line = detail::sv_trim(raw_line);

        if (detail::sv_starts_with(line,
                "export const INTERNAL_ONLY_COMMANDS = [")) {
            in_internal_block = true;
            return;
        }

        if (in_internal_block) {
            if (!line.empty() && line.front() == ']') {
                in_internal_block = false;
                return;
            }
            if (auto name = detail::first_identifier(line)) {
                entries.push_back({*name, CommandSource::InternalOnly});
            }
            return;
        }

        if (detail::sv_starts_with(line, "import ")) {
            for (auto& sym : detail::imported_symbols(line)) {
                entries.push_back({std::move(sym), CommandSource::Builtin});
            }
        }

        if (line.find("feature('") != std::string_view::npos &&
            line.find("./commands/") != std::string_view::npos) {
            if (auto name = detail::first_assignment_identifier(line)) {
                entries.push_back({*name, CommandSource::FeatureGated});
            }
        }
    });

    return detail::dedupe_commands(std::move(entries));
}

// ===========================================================================
// extract_tools
// ===========================================================================

ToolRegistry extract_tools(std::string_view source) {
    std::vector<ToolManifestEntry> entries;

    detail::for_each_line(source, [&](std::string_view raw_line) {
        auto line = detail::sv_trim(raw_line);

        if (detail::sv_starts_with(line, "import ") &&
            line.find("./tools/") != std::string_view::npos) {
            for (auto& sym : detail::imported_symbols(line)) {
                if (detail::sv_ends_with(sym, "Tool")) {
                    entries.push_back({std::move(sym), ToolSource::Base});
                }
            }
        }

        if (line.find("feature('") != std::string_view::npos &&
            line.find("Tool") != std::string_view::npos) {
            if (auto name = detail::first_assignment_identifier(line)) {
                if (detail::sv_ends_with(*name, "Tool") ||
                    detail::sv_ends_with(*name, "Tools")) {
                    entries.push_back({*name, ToolSource::Conditional});
                }
            }
        }
    });

    return detail::dedupe_tools(std::move(entries));
}

// ===========================================================================
// extract_bootstrap_plan
// ===========================================================================

BootstrapPlan extract_bootstrap_plan(std::string_view source) {
    std::vector<BootstrapPhase> phases;
    phases.push_back(BootstrapPhase::CliEntry);

    auto has = [&](std::string_view needle) {
        return source.find(needle) != std::string_view::npos;
    };

    if (has("--version"))            phases.push_back(BootstrapPhase::FastPathVersion);
    if (has("startupProfiler"))      phases.push_back(BootstrapPhase::StartupProfiler);
    if (has("--dump-system-prompt")) phases.push_back(BootstrapPhase::SystemPromptFastPath);
    if (has("--claude-in-chrome-mcp")) phases.push_back(BootstrapPhase::ChromeMcpFastPath);
    if (has("--daemon-worker"))      phases.push_back(BootstrapPhase::DaemonWorkerFastPath);
    if (has("remote-control"))       phases.push_back(BootstrapPhase::BridgeFastPath);
    if (has("args[0] === 'daemon'")) phases.push_back(BootstrapPhase::DaemonFastPath);
    if (has("args[0] === 'ps'") || has("args.includes('--bg')"))
                                     phases.push_back(BootstrapPhase::BackgroundSessionFastPath);
    if (has("args[0] === 'new' || args[0] === 'list' || args[0] === 'reply'"))
                                     phases.push_back(BootstrapPhase::TemplateFastPath);
    if (has("environment-runner"))   phases.push_back(BootstrapPhase::EnvironmentRunnerFastPath);
    phases.push_back(BootstrapPhase::MainRuntime);

    return BootstrapPlan::from_phases(std::move(phases));
}

} // namespace claw
