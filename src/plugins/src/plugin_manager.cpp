#include "plugin_manager.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <system_error>

#include <nlohmann/json.hpp>

namespace claw::plugins {

namespace {

// ── Utility helpers ──────────────────────────────────────────────────────────

uint64_t unix_time_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::string plugin_id_str(const std::string& name, std::string_view marketplace) {
    return name + "@" + std::string(marketplace);
}

std::string sanitize_plugin_id(const std::string& id) {
    std::string s = id;
    for (char& c : s)
        if (c == '/' || c == '\\' || c == '@' || c == ':') c = '-';
    return s;
}

// Returns true for "literal" shell commands (not relative/absolute file paths).
// Mirrors Rust is_literal_command().
bool is_literal_command(std::string_view entry) noexcept {
    return !entry.starts_with("./") && !entry.starts_with("../") &&
           !std::filesystem::path(entry).is_absolute();
}

// Resolve a hook/lifecycle/tool entry against the plugin root directory.
// Mirrors Rust resolve_hook_entry().
std::string resolve_hook_entry(const std::filesystem::path& root,
                               std::string_view entry) {
    if (is_literal_command(entry)) return std::string(entry);
    return (root / entry).string();
}

PluginHooks resolve_hooks(const std::filesystem::path& root,
                          const PluginHooks& hooks) {
    PluginHooks out;
    for (auto& e : hooks.pre_tool_use)
        out.pre_tool_use.push_back(resolve_hook_entry(root, e));
    for (auto& e : hooks.post_tool_use)
        out.post_tool_use.push_back(resolve_hook_entry(root, e));
    for (auto& e : hooks.post_tool_use_failure)
        out.post_tool_use_failure.push_back(resolve_hook_entry(root, e));
    return out;
}

PluginLifecycle resolve_lifecycle(const std::filesystem::path& root,
                                  const PluginLifecycle& lc) {
    PluginLifecycle out;
    for (auto& e : lc.init)     out.init.push_back(resolve_hook_entry(root, e));
    for (auto& e : lc.shutdown) out.shutdown.push_back(resolve_hook_entry(root, e));
    return out;
}

std::vector<PluginTool> resolve_tools(
    const std::filesystem::path& root,
    std::string_view pid,
    std::string_view pname,
    const std::vector<PluginToolManifest>& manifests) {

    std::vector<PluginTool> tools;
    for (auto& m : manifests) {
        tools.emplace_back(
            std::string(pid), std::string(pname),
            PluginToolDefinition{m.name, m.description, m.input_schema},
            resolve_hook_entry(root, m.command),
            m.args,
            m.required_permission,
            std::optional<std::filesystem::path>{root});
    }
    return tools;
}

// ── Manifest parsing ──────────────────────────────────────────────────────────

// Locate the plugin.json manifest within a plugin directory.
// Checks root/plugin.json first, then root/.claude-plugin/plugin.json.
// Mirrors Rust plugin_manifest_path().
tl::expected<std::filesystem::path, PluginError>
plugin_manifest_path(const std::filesystem::path& root) {
    auto direct   = root / MANIFEST_FILE_NAME;
    if (std::filesystem::exists(direct)) return direct;
    auto packaged = root / MANIFEST_RELATIVE_PATH;
    if (std::filesystem::exists(packaged)) return packaged;
    return tl::unexpected(PluginError::not_found(std::format(
        "plugin manifest not found at {} or {}",
        direct.string(), packaged.string())));
}

// Trim leading and trailing ASCII whitespace.
std::string trim(std::string_view sv) {
    const std::string_view ws = " \t\r\n";
    auto s = sv.find_first_not_of(ws);
    if (s == std::string_view::npos) return {};
    auto e = sv.find_last_not_of(ws);
    return std::string(sv.substr(s, e - s + 1));
}

// ── Manifest field validation ─────────────────────────────────────────────────

void validate_required_manifest_field(
    const char* field,
    const std::string& value,
    std::vector<PluginManifestValidationError>& errors) {

    if (trim(value).empty()) {
        PluginManifestValidationError e{};
        e.kind  = PluginManifestValidationErrorKind::EmptyField;
        e.field = field;
        errors.push_back(std::move(e));
    }
}

std::vector<PluginPermission> build_manifest_permissions(
    const std::vector<std::string>& raw,
    std::vector<PluginManifestValidationError>& errors) {

    std::set<std::string> seen;
    std::vector<PluginPermission> validated;

    for (auto& p : raw) {
        auto t = trim(p);
        if (t.empty()) {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::EmptyEntryField;
            e.entry_kind = "permission";
            e.field      = "value";
            errors.push_back(std::move(e));
            continue;
        }
        if (!seen.insert(t).second) {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::DuplicatePermission;
            e.permission = t;
            errors.push_back(std::move(e));
            continue;
        }
        auto perm = parse_plugin_permission(t);
        if (perm) {
            validated.push_back(*perm);
        } else {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::InvalidPermission;
            e.permission = t;
            errors.push_back(std::move(e));
        }
    }
    return validated;
}

// Validate a single command entry (hook, lifecycle, tool command, slash command).
// Mirrors Rust validate_command_entry().
void validate_command_entry(
    const std::filesystem::path& root,
    std::string_view entry,
    const char* kind,
    std::vector<PluginManifestValidationError>& errors) {

    auto t = trim(entry);
    if (t.empty()) {
        PluginManifestValidationError e{};
        e.kind       = PluginManifestValidationErrorKind::EmptyEntryField;
        e.entry_kind = kind;
        e.field      = "command";
        errors.push_back(std::move(e));
        return;
    }
    if (is_literal_command(entry)) return;

    std::filesystem::path path = std::filesystem::path(entry).is_absolute()
        ? std::filesystem::path(entry)
        : root / entry;

    if (!std::filesystem::exists(path)) {
        PluginManifestValidationError e{};
        e.kind       = PluginManifestValidationErrorKind::MissingPath;
        e.entry_kind = kind;
        e.path       = path;
        errors.push_back(std::move(e));
    } else if (!std::filesystem::is_regular_file(path)) {
        PluginManifestValidationError e{};
        e.kind       = PluginManifestValidationErrorKind::PathIsDirectory;
        e.entry_kind = kind;
        e.path       = path;
        errors.push_back(std::move(e));
    }
}

void validate_command_entries(
    const std::filesystem::path& root,
    const std::vector<std::string>& entries,
    const char* kind,
    std::vector<PluginManifestValidationError>& errors) {

    for (auto& e : entries)
        validate_command_entry(root, e, kind, errors);
}

// Mirrors Rust build_manifest_tools().
std::vector<PluginToolManifest> build_manifest_tools(
    const std::filesystem::path& root,
    std::vector<RawPluginToolManifest> tools,
    std::vector<PluginManifestValidationError>& errors) {

    std::set<std::string> seen;
    std::vector<PluginToolManifest> validated;

    for (auto& tool : tools) {
        auto name = trim(tool.name);
        if (name.empty()) {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::EmptyEntryField;
            e.entry_kind = "tool";
            e.field      = "name";
            errors.push_back(std::move(e));
            continue;
        }
        if (!seen.insert(name).second) {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::DuplicateEntry;
            e.entry_kind = "tool";
            e.name       = name;
            errors.push_back(std::move(e));
            continue;
        }
        if (trim(tool.description).empty()) {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::EmptyEntryField;
            e.entry_kind = "tool";
            e.field      = "description";
            e.name       = name;
            errors.push_back(std::move(e));
        }
        if (trim(tool.command).empty()) {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::EmptyEntryField;
            e.entry_kind = "tool";
            e.field      = "command";
            e.name       = name;
            errors.push_back(std::move(e));
        } else {
            validate_command_entry(root, tool.command, "tool", errors);
        }
        if (!tool.input_schema.is_object()) {
            PluginManifestValidationError e{};
            e.kind      = PluginManifestValidationErrorKind::InvalidToolInputSchema;
            e.tool_name = name;
            errors.push_back(std::move(e));
        }
        auto perm = parse_plugin_tool_permission(trim(tool.required_permission));
        if (!perm) {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::InvalidToolRequiredPermission;
            e.tool_name  = name;
            e.permission = trim(tool.required_permission);
            errors.push_back(std::move(e));
            continue;
        }
        validated.push_back(PluginToolManifest{
            name, tool.description, tool.input_schema,
            tool.command, tool.args, *perm});
    }
    return validated;
}

// Mirrors Rust build_manifest_commands().
std::vector<PluginCommandManifest> build_manifest_commands(
    const std::filesystem::path& root,
    std::vector<PluginCommandManifest> commands,
    std::vector<PluginManifestValidationError>& errors) {

    std::set<std::string> seen;
    std::vector<PluginCommandManifest> validated;

    for (auto& cmd : commands) {
        auto name = trim(cmd.name);
        if (name.empty()) {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::EmptyEntryField;
            e.entry_kind = "command";
            e.field      = "name";
            errors.push_back(std::move(e));
            continue;
        }
        if (!seen.insert(name).second) {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::DuplicateEntry;
            e.entry_kind = "command";
            e.name       = name;
            errors.push_back(std::move(e));
            continue;
        }
        if (trim(cmd.description).empty()) {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::EmptyEntryField;
            e.entry_kind = "command";
            e.field      = "description";
            e.name       = name;
            errors.push_back(std::move(e));
        }
        if (trim(cmd.command).empty()) {
            PluginManifestValidationError e{};
            e.kind       = PluginManifestValidationErrorKind::EmptyEntryField;
            e.entry_kind = "command";
            e.field      = "command";
            e.name       = name;
            errors.push_back(std::move(e));
        } else {
            validate_command_entry(root, cmd.command, "command", errors);
        }
        validated.push_back(std::move(cmd));
    }
    return validated;
}

// Mirrors Rust build_plugin_manifest().
tl::expected<PluginManifest, PluginError>
build_plugin_manifest(const std::filesystem::path& root, RawPluginManifest raw) {
    std::vector<PluginManifestValidationError> errors;

    validate_required_manifest_field("name",        raw.name,        errors);
    validate_required_manifest_field("version",     raw.version,     errors);
    validate_required_manifest_field("description", raw.description, errors);

    auto permissions = build_manifest_permissions(raw.permissions, errors);
    validate_command_entries(root, raw.hooks.pre_tool_use,          "hook",              errors);
    validate_command_entries(root, raw.hooks.post_tool_use,         "hook",              errors);
    validate_command_entries(root, raw.hooks.post_tool_use_failure, "hook",              errors);
    validate_command_entries(root, raw.lifecycle.init,              "lifecycle command", errors);
    validate_command_entries(root, raw.lifecycle.shutdown,          "lifecycle command", errors);
    auto tools    = build_manifest_tools(root,    std::move(raw.tools),    errors);
    auto commands = build_manifest_commands(root, std::move(raw.commands), errors);

    if (!errors.empty())
        return tl::unexpected(PluginError::manifest_validation(std::move(errors)));

    return PluginManifest{
        raw.name, raw.version, raw.description,
        std::move(permissions), raw.default_enabled,
        raw.hooks, raw.lifecycle,
        std::move(tools), std::move(commands)
    };
}

// Mirrors Rust load_manifest_from_path().
tl::expected<PluginManifest, PluginError>
load_manifest_from_path(const std::filesystem::path& root,
                        const std::filesystem::path& manifest_path) {
    std::ifstream f(manifest_path);
    if (!f)
        return tl::unexpected(PluginError::not_found(std::format(
            "plugin manifest not found at {}: cannot open",
            manifest_path.string())));
    std::string contents((std::istreambuf_iterator<char>(f)), {});
    RawPluginManifest raw;
    try {
        raw = nlohmann::json::parse(contents).get<RawPluginManifest>();
    } catch (std::exception& ex) {
        return tl::unexpected(PluginError::json(ex.what()));
    }
    return build_plugin_manifest(root, std::move(raw));
}

// Mirrors Rust load_manifest_from_directory().
tl::expected<PluginManifest, PluginError>
load_manifest_from_directory(const std::filesystem::path& root) {
    auto mp = plugin_manifest_path(root);
    if (!mp) return tl::unexpected(std::move(mp.error()));
    return load_manifest_from_path(root, *mp);
}

// Mirrors Rust load_plugin_definition().
tl::expected<PluginDefinition, PluginError>
load_plugin_definition(const std::filesystem::path& root,
                       PluginKind kind,
                       const std::string& source,
                       const char* marketplace) {
    auto manifest = load_manifest_from_directory(root);
    if (!manifest) return tl::unexpected(std::move(manifest.error()));

    PluginMetadata meta;
    meta.id              = plugin_id_str(manifest->name, marketplace);
    meta.name            = manifest->name;
    meta.version         = manifest->version;
    meta.description     = manifest->description;
    meta.kind            = kind;
    meta.source          = source;
    meta.default_enabled = manifest->default_enabled;
    meta.root            = root;

    auto hooks     = resolve_hooks(root, manifest->hooks);
    auto lifecycle = resolve_lifecycle(root, manifest->lifecycle);
    auto tools     = resolve_tools(root, meta.id, meta.name, manifest->tools);

    switch (kind) {
        case PluginKind::Builtin: {
            BuiltinPlugin p;
            p.metadata_ = meta; p.hooks_ = hooks;
            p.lifecycle_ = lifecycle; p.tools_ = tools;
            return PluginDefinition{std::move(p)};
        }
        case PluginKind::Bundled: {
            BundledPlugin p;
            p.metadata_ = meta; p.hooks_ = hooks;
            p.lifecycle_ = lifecycle; p.tools_ = tools;
            return PluginDefinition{std::move(p)};
        }
        default: {
            ExternalPlugin p;
            p.metadata_ = meta; p.hooks_ = hooks;
            p.lifecycle_ = lifecycle; p.tools_ = tools;
            return PluginDefinition{std::move(p)};
        }
    }
}

// Mirrors Rust discover_plugin_dirs(): lists subdirectories that contain a
// valid plugin manifest, sorted lexicographically.
tl::expected<std::vector<std::filesystem::path>, PluginError>
discover_plugin_dirs(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec) return {};
    std::vector<std::filesystem::path> paths;
    for (auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (entry.is_directory() && plugin_manifest_path(entry.path()))
            paths.push_back(entry.path());
    }
    std::ranges::sort(paths);
    return paths;
}

// Mirrors Rust resolve_local_source().
tl::expected<std::filesystem::path, PluginError>
resolve_local_source(std::string_view source) {
    std::filesystem::path p{source};
    if (std::filesystem::exists(p)) return p;
    return tl::unexpected(PluginError::not_found(
        std::format("plugin source `{}` was not found", source)));
}

// Mirrors Rust parse_install_source(): detect git URLs vs. local paths.
tl::expected<PluginInstallSource, PluginError>
parse_install_source(std::string_view source) {
    std::filesystem::path p{source};
    bool looks_like_git =
        source.starts_with("http://")  ||
        source.starts_with("https://") ||
        source.starts_with("git@")     ||
        (p.has_extension() &&
         [&]{
             auto ext = p.extension().string();
             // case-insensitive .git check
             std::string elower = ext;
             for (char& c : elower) c = static_cast<char>(std::tolower(c));
             return elower == ".git";
         }());

    if (looks_like_git)
        return GitUrlSource{std::string(source)};

    auto resolved = resolve_local_source(source);
    if (!resolved) return tl::unexpected(std::move(resolved.error()));
    return LocalPathSource{*resolved};
}

// Mirrors Rust materialize_source(): clones git repos; returns local path as-is.
tl::expected<std::filesystem::path, PluginError>
materialize_source(const PluginInstallSource& source,
                   const std::filesystem::path& temp_root) {
    std::error_code ec;
    std::filesystem::create_directories(temp_root, ec);
    if (ec)
        return tl::unexpected(PluginError{ec});

    return std::visit([&](auto&& s) -> tl::expected<std::filesystem::path, PluginError> {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, LocalPathSource>) {
            return s.path;
        } else {
            // Git clone --depth 1
            auto dest = temp_root / std::format("plugin-{}", unix_time_ms());
            std::string cmd = std::format(
                "git clone --depth 1 {} {}",
                s.url, dest.string());
            int ret = std::system(cmd.c_str());  // NOLINT(cert-env33-c)
            if (ret != 0)
                return tl::unexpected(PluginError::command_failed(
                    std::format("git clone failed for `{}`", s.url)));
            return dest;
        }
    }, source);
}

// Mirrors Rust copy_dir_all().
tl::expected<void, PluginError>
copy_dir_all(const std::filesystem::path& src,
             const std::filesystem::path& dst) {
    std::error_code ec;
    std::filesystem::create_directories(dst, ec);
    if (ec) return tl::unexpected(PluginError{ec});

    for (auto& entry : std::filesystem::directory_iterator(src, ec)) {
        if (ec) return tl::unexpected(PluginError{ec});
        auto target = dst / entry.path().filename();
        if (entry.is_directory()) {
            if (auto r = copy_dir_all(entry.path(), target); !r) return r;
        } else {
            std::filesystem::copy_file(
                entry.path(), target,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) return tl::unexpected(PluginError{ec});
        }
    }
    return {};
}

// Mirrors Rust update_settings_json() / write_enabled_state().
// Reads settings.json, upserts or removes the enabledPlugins/<plugin_id> key,
// then writes the file back with pretty-print indentation of 2.
tl::expected<void, PluginError>
update_settings_json(const std::filesystem::path& path,
                     std::string_view plugin_id,
                     std::optional<bool> enabled) {
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    // Read existing file (or start with empty object)
    nlohmann::json root = nlohmann::json::object();
    {
        std::ifstream f(path);
        if (f) {
            std::string s((std::istreambuf_iterator<char>(f)), {});
            bool non_empty = s.find_first_not_of(" \t\r\n") != std::string::npos;
            if (non_empty) {
                try { root = nlohmann::json::parse(s); }
                catch (...) { /* keep empty object */ }
            }
        }
    }

    if (!root.is_object())
        return tl::unexpected(PluginError::invalid_manifest(std::format(
            "settings file {} must contain a JSON object", path.string())));

    auto& ep = root["enabledPlugins"];
    if (!ep.is_object()) ep = nlohmann::json::object();

    if (enabled.has_value())
        ep[std::string(plugin_id)] = *enabled;
    else
        ep.erase(std::string(plugin_id));

    std::ofstream out(path);
    if (!out)
        return tl::unexpected(
            PluginError{std::make_error_code(std::errc::io_error)});
    out << root.dump(2);
    return {};
}

}  // anonymous namespace

// ─── PluginManager public API ─────────────────────────────────────────────────

PluginManager::PluginManager(PluginManagerConfig config)
    : config_(std::move(config)) {}

// Default bundled root: <executable-dir>/bundled (or cwd/bundled at runtime).
std::filesystem::path PluginManager::bundled_root() {
    return std::filesystem::current_path() / "bundled";
}

std::filesystem::path PluginManager::install_root() const {
    return config_.install_root.value_or(
        config_.config_home / "plugins" / "installed");
}

std::filesystem::path PluginManager::registry_path() const {
    return config_.registry_path.value_or(
        config_.config_home / "plugins" / REGISTRY_FILE_NAME);
}

std::filesystem::path PluginManager::settings_path() const {
    return config_.config_home / SETTINGS_FILE_NAME;
}

// Mirrors Rust PluginManager::plugin_registry().
tl::expected<PluginRegistry, PluginError>
PluginManager::plugin_registry() {
    auto report = plugin_registry_report();
    if (!report) return tl::unexpected(std::move(report.error()));
    return std::move(*report).into_registry();
}

// Mirrors Rust PluginManager::plugin_registry_report().
tl::expected<PluginRegistryReport, PluginError>
PluginManager::plugin_registry_report() {
    if (auto r = sync_bundled_plugins(); !r)
        return tl::unexpected(std::move(r.error()));

    PluginDiscovery discovery;
    for (auto& p : builtin_plugins())
        discovery.push_plugin(std::move(p));

    auto installed = discover_installed_plugins_with_failures();
    if (!installed) return tl::unexpected(std::move(installed.error()));
    discovery.extend(std::move(*installed));

    auto external = discover_external_directory_plugins_with_failures(discovery.plugins);
    if (!external) return tl::unexpected(std::move(external.error()));
    discovery.extend(std::move(*external));

    return build_registry_report(std::move(discovery));
}

// Mirrors Rust PluginManager::list_plugins().
tl::expected<std::vector<PluginSummary>, PluginError>
PluginManager::list_plugins() {
    auto reg = plugin_registry();
    if (!reg) return tl::unexpected(std::move(reg.error()));
    return reg->summaries();
}

// Mirrors Rust PluginManager::list_installed_plugins().
tl::expected<std::vector<PluginSummary>, PluginError>
PluginManager::list_installed_plugins() {
    auto reg = installed_plugin_registry();
    if (!reg) return tl::unexpected(std::move(reg.error()));
    return reg->summaries();
}

// Mirrors Rust PluginManager::discover_plugins().
tl::expected<std::vector<PluginDefinition>, PluginError>
PluginManager::discover_plugins() {
    auto reg = plugin_registry();
    if (!reg) return tl::unexpected(std::move(reg.error()));
    std::vector<PluginDefinition> defs;
    for (auto& p : reg->mutable_plugins())
        defs.push_back(std::move(p.definition));
    return defs;
}

// Mirrors Rust PluginManager::aggregated_hooks().
tl::expected<PluginHooks, PluginError>
PluginManager::aggregated_hooks() {
    auto reg = plugin_registry();
    if (!reg) return tl::unexpected(std::move(reg.error()));
    return reg->aggregated_hooks();
}

// Mirrors Rust PluginManager::aggregated_tools().
tl::expected<std::vector<PluginTool>, PluginError>
PluginManager::aggregated_tools() {
    auto reg = plugin_registry();
    if (!reg) return tl::unexpected(std::move(reg.error()));
    return reg->aggregated_tools();
}

// Mirrors Rust PluginManager::validate_plugin_source().
tl::expected<PluginManifest, PluginError>
PluginManager::validate_plugin_source(std::string_view source) {
    auto path = resolve_local_source(source);
    if (!path) return tl::unexpected(std::move(path.error()));
    return load_manifest_from_directory(*path);
}

// Mirrors Rust PluginManager::install().
tl::expected<InstallOutcome, PluginError>
PluginManager::install(std::string_view source) {
    auto install_source = parse_install_source(source);
    if (!install_source) return tl::unexpected(std::move(install_source.error()));

    auto temp_root = install_root() / ".tmp";
    auto staged = materialize_source(*install_source, temp_root);
    if (!staged) return tl::unexpected(std::move(staged.error()));

    bool cleanup = std::holds_alternative<GitUrlSource>(*install_source);
    auto manifest = load_manifest_from_directory(*staged);
    if (!manifest) return tl::unexpected(std::move(manifest.error()));

    std::string pid = plugin_id_str(manifest->name, EXTERNAL_MARKETPLACE);
    auto install_path = install_root() / sanitize_plugin_id(pid);

    std::error_code ec;
    if (std::filesystem::exists(install_path))
        std::filesystem::remove_all(install_path, ec);

    if (auto r = copy_dir_all(*staged, install_path); !r)
        return tl::unexpected(std::move(r.error()));

    if (cleanup) std::filesystem::remove_all(*staged, ec);

    auto now = unix_time_ms();
    InstalledPluginRecord record;
    record.kind               = PluginKind::External;
    record.id                 = pid;
    record.name               = manifest->name;
    record.version            = manifest->version;
    record.description        = manifest->description;
    record.install_path       = install_path;
    record.source             = *install_source;
    record.installed_at_unix_ms = now;
    record.updated_at_unix_ms   = now;

    auto reg = load_registry();
    if (!reg) return tl::unexpected(std::move(reg.error()));
    reg->plugins[pid] = record;
    if (auto r = store_registry(*reg); !r) return tl::unexpected(std::move(r.error()));
    if (auto r = write_enabled_state(pid, true); !r) return tl::unexpected(std::move(r.error()));
    config_.enabled_plugins[pid] = true;

    return InstallOutcome{pid, manifest->version, install_path};
}

// Mirrors Rust PluginManager::enable().
tl::expected<void, PluginError>
PluginManager::enable(std::string_view plugin_id) {
    if (auto r = ensure_known_plugin(plugin_id); !r) return r;
    if (auto r = write_enabled_state(plugin_id, true); !r) return r;
    config_.enabled_plugins[std::string(plugin_id)] = true;
    return {};
}

// Mirrors Rust PluginManager::disable().
tl::expected<void, PluginError>
PluginManager::disable(std::string_view plugin_id) {
    if (auto r = ensure_known_plugin(plugin_id); !r) return r;
    if (auto r = write_enabled_state(plugin_id, false); !r) return r;
    config_.enabled_plugins[std::string(plugin_id)] = false;
    return {};
}

// Mirrors Rust PluginManager::uninstall().
tl::expected<void, PluginError>
PluginManager::uninstall(std::string_view plugin_id) {
    auto reg = load_registry();
    if (!reg) return tl::unexpected(std::move(reg.error()));

    auto it = reg->plugins.find(std::string(plugin_id));
    if (it == reg->plugins.end())
        return tl::unexpected(PluginError::not_found(
            std::format("plugin `{}` is not installed", plugin_id)));

    // Bundled plugins cannot be uninstalled — only disabled.
    if (it->second.kind == PluginKind::Bundled) {
        return tl::unexpected(PluginError::command_failed(std::format(
            "plugin `{}` is bundled and managed automatically; disable it instead",
            plugin_id)));
    }

    std::error_code ec;
    if (std::filesystem::exists(it->second.install_path))
        std::filesystem::remove_all(it->second.install_path, ec);

    reg->plugins.erase(it);
    if (auto r = store_registry(*reg); !r) return r;
    if (auto r = write_enabled_state(plugin_id, std::nullopt); !r) return r;
    config_.enabled_plugins.erase(std::string(plugin_id));
    return {};
}

// Mirrors Rust PluginManager::update().
tl::expected<UpdateOutcome, PluginError>
PluginManager::update(std::string_view plugin_id) {
    auto reg = load_registry();
    if (!reg) return tl::unexpected(std::move(reg.error()));

    auto it = reg->plugins.find(std::string(plugin_id));
    if (it == reg->plugins.end())
        return tl::unexpected(PluginError::not_found(
            std::format("plugin `{}` is not installed", plugin_id)));

    InstalledPluginRecord record = it->second;
    auto temp_root = install_root() / ".tmp";
    auto staged = materialize_source(record.source, temp_root);
    if (!staged) return tl::unexpected(std::move(staged.error()));

    bool cleanup = std::holds_alternative<GitUrlSource>(record.source);
    auto manifest = load_manifest_from_directory(*staged);
    if (!manifest) return tl::unexpected(std::move(manifest.error()));

    std::error_code ec;
    if (std::filesystem::exists(record.install_path))
        std::filesystem::remove_all(record.install_path, ec);
    if (auto r = copy_dir_all(*staged, record.install_path); !r)
        return tl::unexpected(std::move(r.error()));
    if (cleanup) std::filesystem::remove_all(*staged, ec);

    std::string old_version       = record.version;
    record.version                = manifest->version;
    record.description            = manifest->description;
    record.updated_at_unix_ms     = unix_time_ms();
    reg->plugins[std::string(plugin_id)] = record;
    if (auto r = store_registry(*reg); !r) return tl::unexpected(std::move(r.error()));

    return UpdateOutcome{
        std::string(plugin_id),
        std::move(old_version),
        manifest->version,
        record.install_path
    };
}

// Mirrors Rust PluginManager::installed_plugin_registry_report().
tl::expected<PluginRegistryReport, PluginError>
PluginManager::installed_plugin_registry_report() {
    if (auto r = sync_bundled_plugins(); !r)
        return tl::unexpected(std::move(r.error()));
    auto discovered = discover_installed_plugins_with_failures();
    if (!discovered) return tl::unexpected(std::move(discovered.error()));
    return build_registry_report(std::move(*discovered));
}

// ─── Private helpers ──────────────────────────────────────────────────────────

// Mirrors Rust PluginManager::discover_installed_plugins_with_failures().
tl::expected<PluginDiscovery, PluginError>
PluginManager::discover_installed_plugins_with_failures() {
    auto reg = load_registry();
    if (!reg) return tl::unexpected(std::move(reg.error()));

    PluginDiscovery discovery;
    std::set<std::string>               seen_ids;
    std::set<std::filesystem::path>     seen_paths;
    std::vector<std::string>            stale_ids;

    // Phase 1: scan the install_root directory
    auto dirs = discover_plugin_dirs(install_root());
    if (!dirs) return tl::unexpected(std::move(dirs.error()));

    for (auto& install_path : *dirs) {
        // Look for a matching registry record
        InstalledPluginRecord* matched = nullptr;
        for (auto& [id, record] : reg->plugins)
            if (record.install_path == install_path) { matched = &record; break; }

        PluginKind  kind   = matched ? matched->kind : PluginKind::External;
        std::string source = matched
            ? describe_install_source(matched->source)
            : install_path.string();

        auto plugin = load_plugin_definition(
            install_path, kind, source, plugin_kind_marketplace(kind));
        if (plugin) {
            auto pid = plugin_metadata(*plugin).id;
            if (seen_ids.insert(pid).second) {
                seen_paths.insert(install_path);
                discovery.push_plugin(std::move(*plugin));
            }
        } else {
            discovery.push_failure(PluginLoadFailure{
                install_path, kind, source, std::move(plugin.error())});
        }
    }

    // Phase 2: check registry records whose install_path was not in the scan
    for (auto& [id, record] : reg->plugins) {
        if (seen_paths.count(record.install_path)) continue;

        // If the path is gone or lacks a manifest, mark as stale
        if (!std::filesystem::exists(record.install_path) ||
            !plugin_manifest_path(record.install_path)) {
            stale_ids.push_back(id);
            continue;
        }

        auto source = describe_install_source(record.source);
        auto plugin = load_plugin_definition(
            record.install_path, record.kind, source,
            plugin_kind_marketplace(record.kind));
        if (plugin) {
            auto pid = plugin_metadata(*plugin).id;
            if (seen_ids.insert(pid).second) {
                seen_paths.insert(record.install_path);
                discovery.push_plugin(std::move(*plugin));
            }
        } else {
            discovery.push_failure(PluginLoadFailure{
                record.install_path, record.kind, source,
                std::move(plugin.error())});
        }
    }

    // Remove stale registry entries and persist
    if (!stale_ids.empty()) {
        for (auto& sid : stale_ids) reg->plugins.erase(sid);
        if (auto r = store_registry(*reg); !r)
            return tl::unexpected(std::move(r.error()));
    }

    return discovery;
}

// Mirrors Rust PluginManager::discover_external_directory_plugins_with_failures().
tl::expected<PluginDiscovery, PluginError>
PluginManager::discover_external_directory_plugins_with_failures(
    const std::vector<PluginDefinition>& existing) {

    PluginDiscovery discovery;

    for (auto& dir : config_.external_dirs) {
        auto roots = discover_plugin_dirs(dir);
        if (!roots) return tl::unexpected(std::move(roots.error()));

        for (auto& root : *roots) {
            auto source = root.string();
            auto plugin = load_plugin_definition(
                root, PluginKind::External, source, EXTERNAL_MARKETPLACE);
            if (plugin) {
                auto pid = plugin_metadata(*plugin).id;
                // Skip if already discovered in installed or earlier in this loop
                bool duplicate = false;
                for (auto& e : existing)
                    if (plugin_metadata(e).id == pid) { duplicate = true; break; }
                if (!duplicate) {
                    for (auto& e : discovery.plugins)
                        if (plugin_metadata(e).id == pid) { duplicate = true; break; }
                }
                if (!duplicate) discovery.push_plugin(std::move(*plugin));
            } else {
                discovery.push_failure(PluginLoadFailure{
                    root, PluginKind::External, source, std::move(plugin.error())});
            }
        }
    }
    return discovery;
}

// Mirrors Rust PluginManager::sync_bundled_plugins().
// Copies bundled plugins from the bundled_root into the install_root and keeps
// the registry up-to-date.  Removes stale bundled entries that no longer exist
// in the source.
tl::expected<void, PluginError>
PluginManager::sync_bundled_plugins() {
    auto bundled = config_.bundled_root.value_or(PluginManager::bundled_root());

    auto bundled_dirs = discover_plugin_dirs(bundled);
    if (!bundled_dirs) return tl::unexpected(std::move(bundled_dirs.error()));

    auto reg = load_registry();
    if (!reg) return tl::unexpected(std::move(reg.error()));

    bool changed = false;
    auto inst_root = install_root();
    std::set<std::string> active_bundled_ids;

    for (auto& src_root : *bundled_dirs) {
        auto manifest = load_manifest_from_directory(src_root);
        if (!manifest) return tl::unexpected(std::move(manifest.error()));

        auto pid = plugin_id_str(manifest->name, BUNDLED_MARKETPLACE);
        active_bundled_ids.insert(pid);
        auto install_path = inst_root / sanitize_plugin_id(pid);

        auto now       = unix_time_ms();
        auto* existing = reg->plugins.count(pid) ? &reg->plugins.at(pid) : nullptr;

        bool installed_valid =
            std::filesystem::exists(install_path) &&
            load_manifest_from_directory(install_path).has_value();

        bool needs_sync =
            !existing ||
            existing->kind        != PluginKind::Bundled ||
            existing->version     != manifest->version   ||
            existing->name        != manifest->name      ||
            existing->description != manifest->description ||
            existing->install_path != install_path       ||
            !std::filesystem::exists(existing->install_path) ||
            !installed_valid;

        if (!needs_sync) continue;

        std::error_code ec;
        if (std::filesystem::exists(install_path))
            std::filesystem::remove_all(install_path, ec);
        if (auto r = copy_dir_all(src_root, install_path); !r) return r;

        auto installed_at = existing ? existing->installed_at_unix_ms : now;

        InstalledPluginRecord record;
        record.kind               = PluginKind::Bundled;
        record.id                 = pid;
        record.name               = manifest->name;
        record.version            = manifest->version;
        record.description        = manifest->description;
        record.install_path       = install_path;
        record.source             = PluginInstallSource{LocalPathSource{src_root}};
        record.installed_at_unix_ms = installed_at;
        record.updated_at_unix_ms   = now;
        reg->plugins[pid] = std::move(record);
        changed = true;
    }

    // Prune stale bundled entries that are no longer in the source
    std::vector<std::string> stale;
    for (auto& [id, rec] : reg->plugins)
        if (rec.kind == PluginKind::Bundled && !active_bundled_ids.count(id))
            stale.push_back(id);

    for (auto& id : stale) {
        std::error_code ec;
        auto& rec = reg->plugins.at(id);
        if (std::filesystem::exists(rec.install_path))
            std::filesystem::remove_all(rec.install_path, ec);
        reg->plugins.erase(id);
        changed = true;
    }

    if (changed) {
        if (auto r = store_registry(*reg); !r) return r;
    }
    return {};
}

// Mirrors Rust PluginManager::is_enabled().
// External plugins default to disabled; builtin/bundled use default_enabled.
bool PluginManager::is_enabled(const PluginMetadata& metadata) const noexcept {
    auto it = config_.enabled_plugins.find(metadata.id);
    if (it != config_.enabled_plugins.end()) return it->second;
    switch (metadata.kind) {
        case PluginKind::External: return false;
        default:                   return metadata.default_enabled;
    }
}

// Mirrors Rust PluginManager::ensure_known_plugin().
tl::expected<void, PluginError>
PluginManager::ensure_known_plugin(std::string_view plugin_id) {
    auto reg = plugin_registry();
    if (!reg) return tl::unexpected(std::move(reg.error()));
    if (!reg->contains(plugin_id))
        return tl::unexpected(PluginError::not_found(std::format(
            "plugin `{}` is not installed or discoverable", plugin_id)));
    return {};
}

// Mirrors Rust PluginManager::load_registry().
tl::expected<InstalledPluginRegistry, PluginError>
PluginManager::load_registry() const {
    auto path = registry_path();
    std::ifstream f(path);
    if (!f) return InstalledPluginRegistry{};
    std::string s((std::istreambuf_iterator<char>(f)), {});
    if (s.find_first_not_of(" \t\r\n") == std::string::npos)
        return InstalledPluginRegistry{};
    try {
        return nlohmann::json::parse(s).get<InstalledPluginRegistry>();
    } catch (std::exception& ex) {
        return tl::unexpected(PluginError::json(ex.what()));
    }
}

// Mirrors Rust PluginManager::store_registry().
tl::expected<void, PluginError>
PluginManager::store_registry(const InstalledPluginRegistry& registry) const {
    auto path = registry_path();
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out)
        return tl::unexpected(
            PluginError{std::make_error_code(std::errc::io_error)});
    out << nlohmann::json(registry).dump(2);
    return {};
}

// Mirrors Rust PluginManager::write_enabled_state().
tl::expected<void, PluginError>
PluginManager::write_enabled_state(std::string_view plugin_id,
                                   std::optional<bool> enabled) {
    return update_settings_json(settings_path(), plugin_id, enabled);
}

// Mirrors Rust PluginManager::installed_plugin_registry().
tl::expected<PluginRegistry, PluginError>
PluginManager::installed_plugin_registry() {
    auto report = installed_plugin_registry_report();
    if (!report) return tl::unexpected(std::move(report.error()));
    return std::move(*report).into_registry();
}

// Mirrors Rust PluginManager::build_registry_report().
PluginRegistryReport
PluginManager::build_registry_report(PluginDiscovery discovery) const {
    std::vector<RegisteredPlugin> registered;
    registered.reserve(discovery.plugins.size());
    for (auto& p : discovery.plugins) {
        bool en = is_enabled(plugin_metadata(p));
        registered.emplace_back(std::move(p), en);
    }
    return PluginRegistryReport{
        PluginRegistry{std::move(registered)},
        std::move(discovery.failures)
    };
}

}  // namespace claw::plugins
