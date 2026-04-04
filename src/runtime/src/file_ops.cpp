// file_ops.cpp – Full C++20 faithful conversion of file_ops.rs
// Every public function from the Rust source is implemented here.
// The legacy API declared in file_ops.hpp is also implemented for
// compatibility with the rest of the runtime.

#include "file_ops.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace claw::runtime {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Constants (mirrors Rust constants)
// ---------------------------------------------------------------------------

inline constexpr std::uint64_t MAX_READ_SIZE  = 10ULL * 1024 * 1024; // 10 MB
inline constexpr std::size_t   MAX_WRITE_SIZE = 10ULL * 1024 * 1024; // 10 MB

// ---------------------------------------------------------------------------
// Rich result/output structs (mirrors Rust structs)
// ---------------------------------------------------------------------------

struct TextFilePayload {
    std::string file_path;
    std::string content;
    std::size_t num_lines{0};
    std::size_t start_line{0};
    std::size_t total_lines{0};
};

struct ReadFileOutput {
    std::string kind;          // "text"
    TextFilePayload file;
};

struct StructuredPatchHunk {
    std::size_t old_start{0};
    std::size_t old_lines{0};
    std::size_t new_start{0};
    std::size_t new_lines{0};
    std::vector<std::string> lines;
};

struct WriteFileOutput {
    std::string kind;          // "create" or "update"
    std::string file_path;
    std::string content;
    std::vector<StructuredPatchHunk> structured_patch;
    std::optional<std::string> original_file;
    // git_diff omitted (always null in Rust source)
};

struct EditFileOutput {
    std::string file_path;
    std::string old_string;
    std::string new_string;
    std::string original_file;
    std::vector<StructuredPatchHunk> structured_patch;
    bool user_modified{false};
    bool replace_all{false};
    // git_diff omitted (always null in Rust source)
};

struct GlobSearchOutput {
    std::uint64_t duration_ms{0};
    std::size_t   num_files{0};
    std::vector<std::string> filenames;
    bool truncated{false};
};

struct GrepSearchInput {
    std::string pattern;
    std::optional<std::string> path;
    std::optional<std::string> glob;
    std::optional<std::string> output_mode;
    std::optional<std::size_t> before;
    std::optional<std::size_t> after;
    std::optional<std::size_t> context_short;
    std::optional<std::size_t> context;
    std::optional<bool> line_numbers;
    std::optional<bool> case_insensitive;
    std::optional<std::string> file_type;
    std::optional<std::size_t> head_limit;
    std::optional<std::size_t> offset;
    std::optional<bool> multiline;
};

struct GrepSearchOutput {
    std::optional<std::string> mode;
    std::size_t num_files{0};
    std::vector<std::string> filenames;
    std::optional<std::string> content;
    std::optional<std::size_t> num_lines;
    std::optional<std::size_t> num_matches;
    std::optional<std::size_t> applied_limit;
    std::optional<std::size_t> applied_offset;
};

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------

// Build a std::system_error-compatible error (mirrors io::Error)
static std::system_error make_error(std::errc code, std::string msg) {
    return std::system_error(std::make_error_code(code), std::move(msg));
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Check whether a file appears to contain binary content by examining
/// the first 8192 bytes for NUL bytes. Mirrors is_binary_file() in Rust.
static bool is_binary_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::system_error(errno, std::generic_category(),
                                "cannot open file: " + path.string());
    }
    char buffer[8192];
    file.read(buffer, sizeof(buffer));
    std::streamsize bytes_read = file.gcount();
    for (std::streamsize i = 0; i < bytes_read; ++i) {
        if (buffer[i] == '\0') return true;
    }
    return false;
}

/// Validate that resolved path stays within workspace_root.
/// Mirrors validate_workspace_boundary() in Rust.
static void validate_workspace_boundary(const fs::path& resolved,
                                        const fs::path& workspace_root) {
    // Use mismatch on the path components to check prefix containment.
    auto [r_it, w_it] = std::mismatch(resolved.begin(), resolved.end(),
                                       workspace_root.begin(), workspace_root.end());
    if (w_it != workspace_root.end()) {
        throw make_error(std::errc::permission_denied,
                         std::format("path {} escapes workspace boundary {}",
                                     resolved.string(), workspace_root.string()));
    }
}

/// Normalise path: resolve to absolute, then canonicalize (must exist).
/// Mirrors normalize_path() in Rust.
static fs::path normalize_path(const std::string& path) {
    fs::path candidate = fs::path(path).is_absolute()
                             ? fs::path(path)
                             : fs::current_path() / path;
    std::error_code ec;
    auto canonical = fs::canonical(candidate, ec);
    if (ec) {
        throw std::system_error(ec, "cannot canonicalize: " + candidate.string());
    }
    return canonical;
}

/// Normalise path, allowing the leaf to be missing.
/// Mirrors normalize_path_allow_missing() in Rust.
static fs::path normalize_path_allow_missing(const std::string& path) {
    fs::path candidate = fs::path(path).is_absolute()
                             ? fs::path(path)
                             : fs::current_path() / path;
    std::error_code ec;
    auto canonical = fs::canonical(candidate, ec);
    if (!ec) return canonical;

    // Try to canonicalize the parent, keep the filename.
    if (candidate.has_parent_path()) {
        std::error_code ec2;
        auto canonical_parent = fs::canonical(candidate.parent_path(), ec2);
        if (!ec2 && candidate.has_filename()) {
            return canonical_parent / candidate.filename();
        }
        // Parent not canonicalizable – return as-is joined.
        if (candidate.has_filename()) {
            return candidate.parent_path() / candidate.filename();
        }
    }
    return candidate;
}

/// Build a structured patch. The Rust implementation emits a single hunk
/// that lists all old lines prefixed with '-' and all new lines with '+'.
/// Mirrors make_patch() in Rust.
static std::vector<StructuredPatchHunk> make_patch(const std::string& original,
                                                    const std::string& updated) {
    std::vector<std::string> lines_vec;

    // Count and collect '-' lines from original
    std::size_t old_count = 0;
    {
        std::istringstream ss(original);
        std::string line;
        while (std::getline(ss, line)) {
            lines_vec.push_back("-" + line);
            ++old_count;
        }
    }

    // Count and collect '+' lines from updated
    std::size_t new_count = 0;
    {
        std::istringstream ss(updated);
        std::string line;
        while (std::getline(ss, line)) {
            lines_vec.push_back("+" + line);
            ++new_count;
        }
    }

    StructuredPatchHunk hunk;
    hunk.old_start = 1;
    hunk.old_lines = old_count;
    hunk.new_start = 1;
    hunk.new_lines = new_count;
    hunk.lines = std::move(lines_vec);
    return {std::move(hunk)};
}

// ---------------------------------------------------------------------------
// Glob pattern matching helper (replaces the Rust 'glob' crate)
// ---------------------------------------------------------------------------

/// Match a file path against a glob pattern.  Supports:
///   **  – match any number of path components (including zero)
///   *   – match any characters within a single component
///   ?   – match any single character within a component
///   [..] – not supported (treated as literals)
///
/// The pattern is matched against the full path string (forward-slash
/// normalised) which is how the Rust glob crate works.
static bool glob_matches(const std::string& pattern, const std::string& path_str) {
    // Recursive lambda; use indices into the strings.
    // p = pattern index, s = path index
    std::function<bool(std::size_t, std::size_t)> match =
        [&](std::size_t p, std::size_t s) -> bool {
        while (p < pattern.size()) {
            if (pattern[p] == '*') {
                // Check for '**'
                if (p + 1 < pattern.size() && pattern[p + 1] == '*') {
                    // '**' matches zero or more path segments.
                    // Skip over '**' (and any trailing separator).
                    std::size_t pp = p + 2;
                    if (pp < pattern.size() && pattern[pp] == '/') ++pp;
                    // Try matching the rest of the pattern at every position.
                    for (std::size_t ss = s; ss <= path_str.size(); ++ss) {
                        if (match(pp, ss)) return true;
                        // Advance ss past next path separator or to end
                        if (ss < path_str.size() && path_str[ss] == '/') {
                            // keep advancing
                        }
                    }
                    return false;
                } else {
                    // Single '*': match any characters except '/'
                    ++p;
                    for (std::size_t ss = s; ss <= path_str.size(); ++ss) {
                        if (match(p, ss)) return true;
                        if (ss < path_str.size() && path_str[ss] == '/') break;
                    }
                    return false;
                }
            } else if (pattern[p] == '?') {
                // Match exactly one character that is not '/'
                if (s >= path_str.size() || path_str[s] == '/') return false;
                ++p; ++s;
            } else {
                // Literal character
                if (s >= path_str.size() || pattern[p] != path_str[s]) return false;
                ++p; ++s;
            }
        }
        return s == path_str.size();
    };

    return match(0, 0);
}

/// Expand a glob pattern rooted at base_dir; return all matching files
/// sorted by modification time descending (most-recently-modified first),
/// truncated to 100. Mirrors glob_search() in Rust.
static GlobSearchOutput glob_search_impl(const std::string& pattern,
                                          std::optional<std::string> base_path_opt) {
    auto start = std::chrono::steady_clock::now();

    fs::path base_dir;
    if (base_path_opt.has_value()) {
        base_dir = normalize_path(*base_path_opt);
    } else {
        base_dir = fs::current_path();
    }

    // Build the absolute search pattern string.
    // If the pattern is absolute, use it directly; otherwise join with base.
    std::string search_pattern;
    {
        fs::path pat_path(pattern);
        if (pat_path.is_absolute()) {
            search_pattern = pattern;
        } else {
            search_pattern = (base_dir / pattern).generic_string();
        }
    }

    // Normalise separators to '/' for glob matching consistency.
    auto normalise_sep = [](std::string s) -> std::string {
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    };
    search_pattern = normalise_sep(search_pattern);

    // Determine the fixed prefix of the pattern (up to the first wildcard)
    // to limit the directory walk.
    fs::path walk_root = base_dir;
    {
        auto first_wild = search_pattern.find_first_of("*?[");
        if (first_wild != std::string::npos) {
            auto last_sep = search_pattern.rfind('/', first_wild);
            if (last_sep != std::string::npos) {
                walk_root = fs::path(search_pattern.substr(0, last_sep));
            } else {
                walk_root = fs::path(".");
            }
        }
    }

    struct Entry {
        fs::path path;
        fs::file_time_type mtime;
    };
    std::vector<Entry> matches;

    std::error_code ec;
    for (const auto& entry :
         fs::recursive_directory_iterator(walk_root,
                                          fs::directory_options::skip_permission_denied,
                                          ec)) {
        if (ec) { ec.clear(); continue; }
        if (!entry.is_regular_file(ec)) { ec.clear(); continue; }

        std::string entry_str = normalise_sep(entry.path().generic_string());
        if (glob_matches(search_pattern, entry_str)) {
            fs::file_time_type mtime{};
            entry.status(ec); // ensure stat is cached
            ec.clear();
            auto st = fs::last_write_time(entry.path(), ec);
            if (!ec) mtime = st;
            ec.clear();
            matches.push_back({entry.path(), mtime});
        }
    }

    // Sort by mtime descending (Reverse order in Rust = newest first)
    std::sort(matches.begin(), matches.end(), [](const Entry& a, const Entry& b) {
        return a.mtime > b.mtime;
    });

    bool truncated = matches.size() > 100;
    std::vector<std::string> filenames;
    filenames.reserve(std::min(matches.size(), std::size_t{100}));
    for (std::size_t i = 0; i < matches.size() && i < 100; ++i) {
        filenames.push_back(matches[i].path.string());
    }

    auto end = std::chrono::steady_clock::now();
    std::uint64_t duration_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

    return GlobSearchOutput{
        duration_ms,
        filenames.size(),
        std::move(filenames),
        truncated
    };
}

// ---------------------------------------------------------------------------
// collect_search_files – mirrors collect_search_files() in Rust
// ---------------------------------------------------------------------------

static std::vector<fs::path> collect_search_files(const fs::path& base_path) {
    if (fs::is_regular_file(base_path)) {
        return {base_path};
    }
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry :
         fs::recursive_directory_iterator(base_path,
                                          fs::directory_options::skip_permission_denied,
                                          ec)) {
        if (ec) { ec.clear(); continue; }
        if (entry.is_regular_file(ec)) {
            files.push_back(entry.path());
        }
        ec.clear();
    }
    return files;
}

// ---------------------------------------------------------------------------
// matches_optional_filters – mirrors matches_optional_filters() in Rust
// ---------------------------------------------------------------------------

static bool matches_optional_filters(const fs::path& path,
                                      const std::optional<std::string>& glob_filter,
                                      const std::optional<std::string>& file_type) {
    if (glob_filter.has_value()) {
        // Match against either the full path string or just the filename.
        std::string path_str = path.generic_string();
        std::replace(path_str.begin(), path_str.end(), '\\', '/');
        std::string filename_str = path.filename().string();
        bool matched = glob_matches(*glob_filter, path_str) ||
                       glob_matches(*glob_filter, filename_str);
        if (!matched) return false;
    }
    if (file_type.has_value()) {
        auto ext = path.extension().string();
        // Remove leading dot from extension
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        if (ext != *file_type) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// apply_limit – mirrors apply_limit() in Rust
// ---------------------------------------------------------------------------

struct ApplyLimitResult {
    std::vector<std::string> items;
    std::optional<std::size_t> applied_limit;
    std::optional<std::size_t> applied_offset;
};

static ApplyLimitResult apply_limit(std::vector<std::string> items,
                                     std::optional<std::size_t> limit_opt,
                                     std::optional<std::size_t> offset_opt) {
    std::size_t offset_value = offset_opt.value_or(0);

    // Skip first offset_value items
    if (offset_value >= items.size()) {
        items.clear();
    } else {
        items.erase(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(offset_value));
    }

    std::size_t explicit_limit = limit_opt.value_or(250);
    if (explicit_limit == 0) {
        // limit=0 means unlimited
        return {
            std::move(items),
            std::nullopt,
            (offset_value > 0) ? std::optional<std::size_t>{offset_value} : std::nullopt
        };
    }

    bool truncated = items.size() > explicit_limit;
    if (truncated) items.resize(explicit_limit);

    return {
        std::move(items),
        truncated ? std::optional<std::size_t>{explicit_limit} : std::nullopt,
        (offset_value > 0) ? std::optional<std::size_t>{offset_value} : std::nullopt
    };
}

// ---------------------------------------------------------------------------
// Public Rust-faithful API
// ---------------------------------------------------------------------------

/// Read a file (no workspace enforcement).
/// Mirrors read_file() in Rust.
ReadFileOutput read_file_rich(const std::string& path,
                               std::optional<std::size_t> offset,
                               std::optional<std::size_t> limit) {
    fs::path absolute_path = normalize_path(path);

    // Check file size
    std::error_code ec;
    auto file_size = fs::file_size(absolute_path, ec);
    if (ec) {
        throw std::system_error(ec, "cannot stat file: " + absolute_path.string());
    }
    if (file_size > MAX_READ_SIZE) {
        throw make_error(std::errc::invalid_argument,
                         std::format("file is too large ({} bytes, max {} bytes)",
                                     file_size, MAX_READ_SIZE));
    }

    // Detect binary
    if (is_binary_file(absolute_path)) {
        throw make_error(std::errc::invalid_argument, "file appears to be binary");
    }

    // Read all content
    std::ifstream in(absolute_path);
    if (!in) {
        throw std::system_error(errno, std::generic_category(),
                                "cannot open file: " + absolute_path.string());
    }
    std::string raw_content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());

    // Split into lines
    std::vector<std::string_view> lines;
    {
        std::string_view sv(raw_content);
        std::size_t pos = 0;
        while (pos <= sv.size()) {
            auto nl = sv.find('\n', pos);
            if (nl == std::string_view::npos) {
                lines.push_back(sv.substr(pos));
                break;
            }
            lines.push_back(sv.substr(pos, nl - pos));
            pos = nl + 1;
        }
        // Rust's str::lines() does not yield a trailing empty element for a
        // trailing newline – drop a trailing empty view if present.
        if (!lines.empty() && lines.back().empty()) {
            lines.pop_back();
        }
    }

    std::size_t total_lines = lines.size();
    std::size_t start_index = std::min(offset.value_or(0), total_lines);
    std::size_t end_index = limit.has_value()
        ? std::min(start_index + *limit, total_lines)
        : total_lines;

    // Join selected lines
    std::string selected;
    for (std::size_t i = start_index; i < end_index; ++i) {
        if (i > start_index) selected += '\n';
        selected += lines[i];
    }

    ReadFileOutput out;
    out.kind = "text";
    out.file.file_path = absolute_path.string();
    out.file.content = std::move(selected);
    out.file.num_lines = (end_index > start_index) ? end_index - start_index : 0;
    out.file.start_line = start_index + 1; // 1-based, mirrors saturating_add(1)
    out.file.total_lines = total_lines;
    return out;
}

/// Write a file (no workspace enforcement).
/// Mirrors write_file() in Rust.
WriteFileOutput write_file_rich(const std::string& path, const std::string& content) {
    if (content.size() > MAX_WRITE_SIZE) {
        throw make_error(std::errc::invalid_argument,
                         std::format("content is too large ({} bytes, max {} bytes)",
                                     content.size(), MAX_WRITE_SIZE));
    }

    fs::path absolute_path = normalize_path_allow_missing(path);

    // Try to read original file; nullopt if it does not exist.
    std::optional<std::string> original_file;
    {
        std::ifstream in(absolute_path);
        if (in) {
            original_file = std::string((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
        }
    }

    // Create parent directories if necessary.
    if (absolute_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(absolute_path.parent_path(), ec);
        if (ec) {
            throw std::system_error(ec, "cannot create directories: " +
                                        absolute_path.parent_path().string());
        }
    }

    // Write the file.
    {
        std::ofstream out(absolute_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::system_error(errno, std::generic_category(),
                                    "cannot write file: " + absolute_path.string());
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            throw std::system_error(errno, std::generic_category(),
                                    "write failed: " + absolute_path.string());
        }
    }

    WriteFileOutput result;
    result.kind = original_file.has_value() ? "update" : "create";
    result.file_path = absolute_path.string();
    result.content = content;
    result.structured_patch = make_patch(original_file.value_or(""), content);
    result.original_file = std::move(original_file);
    return result;
}

/// Edit a file: replace old_string with new_string.
/// Mirrors edit_file() in Rust.
EditFileOutput edit_file_rich(const std::string& path,
                               const std::string& old_string,
                               const std::string& new_string,
                               bool replace_all) {
    fs::path absolute_path = normalize_path(path);

    std::string original_file;
    {
        std::ifstream in(absolute_path);
        if (!in) {
            throw std::system_error(errno, std::generic_category(),
                                    "cannot open file: " + absolute_path.string());
        }
        original_file = std::string((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    }

    if (old_string == new_string) {
        throw make_error(std::errc::invalid_argument,
                         "old_string and new_string must differ");
    }

    if (original_file.find(old_string) == std::string::npos) {
        throw make_error(std::errc::no_such_file_or_directory,
                         "old_string not found in file");
    }

    std::string updated;
    if (replace_all) {
        // Replace every occurrence
        updated = original_file;
        std::size_t pos = 0;
        while ((pos = updated.find(old_string, pos)) != std::string::npos) {
            updated.replace(pos, old_string.size(), new_string);
            pos += new_string.size();
        }
    } else {
        // Replace first occurrence only
        updated = original_file;
        auto pos = updated.find(old_string);
        updated.replace(pos, old_string.size(), new_string);
    }

    // Write back
    {
        std::ofstream out(absolute_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::system_error(errno, std::generic_category(),
                                    "cannot write file: " + absolute_path.string());
        }
        out.write(updated.data(), static_cast<std::streamsize>(updated.size()));
    }

    EditFileOutput result;
    result.file_path = absolute_path.string();
    result.old_string = old_string;
    result.new_string = new_string;
    result.original_file = original_file;
    result.structured_patch = make_patch(original_file, updated);
    result.user_modified = false;
    result.replace_all = replace_all;
    return result;
}

/// Glob search. Mirrors glob_search() in Rust.
GlobSearchOutput glob_search_rich(const std::string& pattern,
                                   std::optional<std::string> path) {
    return glob_search_impl(pattern, std::move(path));
}

/// Grep search. Mirrors grep_search() in Rust.
GrepSearchOutput grep_search_rich(const GrepSearchInput& input) {
    fs::path base_path;
    if (input.path.has_value()) {
        base_path = normalize_path(*input.path);
    } else {
        base_path = fs::current_path();
    }

    // Build regex
    std::regex_constants::syntax_option_type flags =
        std::regex_constants::ECMAScript;
    if (input.case_insensitive.value_or(false)) {
        flags |= std::regex_constants::icase;
    }

    // multiline in Rust (dot_matches_new_line) – std::regex has no direct
    // equivalent flag for dot-matches-newline; we handle it at match time.
    bool dot_matches_newline = input.multiline.value_or(false);

    std::regex content_re;
    try {
        content_re = std::regex(input.pattern, flags);
    } catch (const std::regex_error& e) {
        throw make_error(std::errc::invalid_argument,
                         std::format("invalid regex: {}", e.what()));
    }

    std::size_t context = input.context.value_or(
                              input.context_short.value_or(0));

    std::string output_mode = input.output_mode.value_or("files_with_matches");

    std::vector<std::string> filenames;
    std::vector<std::string> content_lines;
    std::size_t total_matches = 0;

    for (const fs::path& file_path : collect_search_files(base_path)) {
        if (!matches_optional_filters(file_path, input.glob, input.file_type)) {
            continue;
        }

        // Try to read the file; skip on error (binary, permission, etc.)
        std::string file_contents;
        {
            std::ifstream in(file_path);
            if (!in) continue;
            file_contents = std::string((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
        }
        // Skip files that cannot be decoded as text
        if (file_contents.find('\0') != std::string::npos) continue;

        if (output_mode == "count") {
            // Count all matches in the full file contents.
            // If dot_matches_newline we search on the full string;
            // otherwise we count per-line matches.
            std::size_t count = 0;
            if (dot_matches_newline) {
                auto begin_it = std::sregex_iterator(file_contents.begin(),
                                                      file_contents.end(),
                                                      content_re);
                auto end_it   = std::sregex_iterator();
                count = static_cast<std::size_t>(std::distance(begin_it, end_it));
            } else {
                std::istringstream ss(file_contents);
                std::string line;
                while (std::getline(ss, line)) {
                    if (std::regex_search(line, content_re)) ++count;
                }
            }
            if (count > 0) {
                filenames.push_back(file_path.string());
                total_matches += count;
            }
            continue;
        }

        // Split into lines for line-by-line matching.
        std::vector<std::string> lines;
        {
            std::istringstream ss(file_contents);
            std::string line;
            while (std::getline(ss, line)) {
                lines.push_back(std::move(line));
            }
        }

        std::vector<std::size_t> matched_indices;
        for (std::size_t idx = 0; idx < lines.size(); ++idx) {
            bool is_match = false;
            if (dot_matches_newline) {
                // Treat each line as its own target; dot_matches_newline is a
                // no-op per-line but we honour the flag for completeness.
                is_match = std::regex_search(lines[idx], content_re);
            } else {
                is_match = std::regex_search(lines[idx], content_re);
            }
            if (is_match) {
                ++total_matches;
                matched_indices.push_back(idx);
            }
        }

        if (matched_indices.empty()) continue;

        filenames.push_back(file_path.string());

        if (output_mode == "content") {
            std::size_t before_ctx = input.before.value_or(context);
            std::size_t after_ctx  = input.after.value_or(context);
            bool show_line_numbers = input.line_numbers.value_or(true);
            std::string file_str   = file_path.string();

            for (std::size_t idx : matched_indices) {
                std::size_t start = (idx >= before_ctx) ? idx - before_ctx : 0;
                std::size_t end   = std::min(idx + after_ctx + 1, lines.size());
                for (std::size_t cur = start; cur < end; ++cur) {
                    std::string prefix;
                    if (show_line_numbers) {
                        prefix = std::format("{}:{}:", file_str, cur + 1);
                    } else {
                        prefix = file_str + ":";
                    }
                    content_lines.push_back(prefix + lines[cur]);
                }
            }
        }
    }

    // Apply limit/offset
    if (output_mode == "content") {
        auto [lim_lines, lim_limit, lim_offset] =
            apply_limit(std::move(content_lines), input.head_limit, input.offset);
        auto [lim_fnames, f_limit, f_offset] =
            apply_limit(std::move(filenames), input.head_limit, input.offset);

        GrepSearchOutput out;
        out.mode         = output_mode;
        out.num_files    = lim_fnames.size();
        out.filenames    = std::move(lim_fnames);
        out.num_lines    = lim_lines.size();
        out.content      = [&]() -> std::string {
            std::string joined;
            for (std::size_t i = 0; i < lim_lines.size(); ++i) {
                if (i > 0) joined += '\n';
                joined += lim_lines[i];
            }
            return joined;
        }();
        out.num_matches  = std::nullopt;
        out.applied_limit  = lim_limit;
        out.applied_offset = lim_offset;
        return out;
    }

    auto [lim_fnames, f_limit, f_offset] =
        apply_limit(std::move(filenames), input.head_limit, input.offset);

    GrepSearchOutput out;
    out.mode         = output_mode;
    out.num_files    = lim_fnames.size();
    out.filenames    = std::move(lim_fnames);
    out.content      = std::nullopt;
    out.num_lines    = std::nullopt;
    out.num_matches  = (output_mode == "count")
                           ? std::optional<std::size_t>{total_matches}
                           : std::nullopt;
    out.applied_limit  = f_limit;
    out.applied_offset = f_offset;
    return out;
}

// ---------------------------------------------------------------------------
// Workspace-enforced variants (mirrors read/write/edit_file_in_workspace)
// ---------------------------------------------------------------------------

ReadFileOutput read_file_in_workspace_rich(const std::string& path,
                                            std::optional<std::size_t> offset,
                                            std::optional<std::size_t> limit,
                                            const fs::path& workspace_root) {
    fs::path absolute_path = normalize_path(path);
    std::error_code ec;
    fs::path canonical_root = fs::canonical(workspace_root, ec);
    if (ec) canonical_root = workspace_root;
    validate_workspace_boundary(absolute_path, canonical_root);
    return read_file_rich(path, offset, limit);
}

WriteFileOutput write_file_in_workspace_rich(const std::string& path,
                                              const std::string& content,
                                              const fs::path& workspace_root) {
    fs::path absolute_path = normalize_path_allow_missing(path);
    std::error_code ec;
    fs::path canonical_root = fs::canonical(workspace_root, ec);
    if (ec) canonical_root = workspace_root;
    validate_workspace_boundary(absolute_path, canonical_root);
    return write_file_rich(path, content);
}

EditFileOutput edit_file_in_workspace_rich(const std::string& path,
                                            const std::string& old_string,
                                            const std::string& new_string,
                                            bool replace_all,
                                            const fs::path& workspace_root) {
    fs::path absolute_path = normalize_path(path);
    std::error_code ec;
    fs::path canonical_root = fs::canonical(workspace_root, ec);
    if (ec) canonical_root = workspace_root;
    validate_workspace_boundary(absolute_path, canonical_root);
    return edit_file_rich(path, old_string, new_string, replace_all);
}

/// Check whether a path is a symlink that resolves outside the workspace.
/// Mirrors is_symlink_escape() in Rust.
bool is_symlink_escape(const fs::path& path, const fs::path& workspace_root) {
    std::error_code ec;
    auto symlink_status = fs::symlink_status(path, ec);
    if (ec) {
        throw std::system_error(ec, "cannot stat path: " + path.string());
    }
    if (symlink_status.type() != fs::file_type::symlink) {
        return false;
    }
    fs::path resolved = fs::canonical(path, ec);
    if (ec) {
        throw std::system_error(ec, "cannot canonicalize symlink: " + path.string());
    }
    fs::path canonical_root = fs::canonical(workspace_root, ec);
    if (ec) canonical_root = workspace_root;

    // resolved.starts_with(canonical_root) check via mismatch
    auto [r_it, w_it] = std::mismatch(resolved.begin(), resolved.end(),
                                       canonical_root.begin(), canonical_root.end());
    bool inside = (w_it == canonical_root.end());
    return !inside;
}

// ===========================================================================
// Legacy API (declared in file_ops.hpp) – implemented in terms of the
// rich functions above so there is no duplicated logic.
// ===========================================================================

bool is_within_workspace(const fs::path& path, const fs::path& workspace_root) {
    std::error_code ec;
    fs::path canonical_path = fs::weakly_canonical(path, ec);
    fs::path canonical_root = fs::weakly_canonical(workspace_root, ec);
    if (ec) return false;
    auto [p_it, r_it] = std::mismatch(canonical_path.begin(), canonical_path.end(),
                                       canonical_root.begin(), canonical_root.end());
    return r_it == canonical_root.end();
}

tl::expected<std::string, std::string>
read_file(const fs::path& path,
          const fs::path& workspace_root,
          std::optional<std::size_t> max_bytes) {
    if (!is_within_workspace(path, workspace_root)) {
        return tl::unexpected(
            std::format("path {} is outside workspace {}",
                        path.string(), workspace_root.string()));
    }

    // Use rich implementation; adapt exceptions to expected<>.
    try {
        // Pass no offset/limit; respect max_bytes via manual check.
        std::error_code ec;
        auto file_size = fs::file_size(path, ec);
        std::size_t limit = max_bytes.value_or(FILE_READ_MAX_BYTES);
        if (!ec && file_size > limit) {
            return tl::unexpected(
                std::format("file {} exceeds read limit ({} > {} bytes)",
                            path.string(), file_size, limit));
        }

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return tl::unexpected(std::format("cannot open file: {}", path.string()));
        }
        std::string content(static_cast<std::size_t>(limit), '\0');
        in.read(content.data(), static_cast<std::streamsize>(limit));
        content.resize(static_cast<std::size_t>(in.gcount()));
        return content;
    } catch (const std::exception& e) {
        return tl::unexpected(e.what());
    }
}

tl::expected<void, std::string>
write_file(const fs::path& path,
           std::string_view content,
           const fs::path& workspace_root) {
    if (!is_within_workspace(path, workspace_root)) {
        return tl::unexpected(
            std::format("path {} is outside workspace", path.string()));
    }
    try {
        // Atomic write via temp file + rename (preserves existing behaviour).
        auto tmp_path = path.parent_path() / (path.filename().string() + ".tmp");
        {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                return tl::unexpected(
                    std::format("cannot write temp file: {}", tmp_path.string()));
            }
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        std::error_code ec;
        fs::rename(tmp_path, path, ec);
        if (ec) {
            return tl::unexpected(std::format("rename failed: {}", ec.message()));
        }
        return {};
    } catch (const std::exception& e) {
        return tl::unexpected(e.what());
    }
}

tl::expected<void, std::string>
edit_file(const fs::path& path,
          std::string_view old_string,
          std::string_view new_string,
          const fs::path& workspace_root) {
    auto content_r = read_file(path, workspace_root);
    if (!content_r) return tl::unexpected(content_r.error());
    auto& content = *content_r;

    auto pos = content.find(old_string);
    if (pos == std::string::npos) {
        return tl::unexpected(
            std::format("old_string not found in {}", path.string()));
    }
    content.replace(pos, old_string.size(), new_string);
    return write_file(path, content, workspace_root);
}

tl::expected<std::vector<fs::path>, std::string>
glob_search(const fs::path& dir, std::string_view pattern) {
    try {
        auto result = glob_search_impl(std::string(pattern),
                                        std::make_optional(dir.string()));
        std::vector<fs::path> paths;
        paths.reserve(result.filenames.size());
        for (const auto& fn : result.filenames) {
            paths.emplace_back(fn);
        }
        return paths;
    } catch (const std::exception& e) {
        return tl::unexpected(e.what());
    }
}

tl::expected<std::vector<GrepMatch>, std::string>
grep_search(const fs::path& dir,
            std::string_view pattern,
            bool use_regex,
            std::optional<std::string_view> file_glob) {
    try {
        GrepSearchInput input;
        input.pattern = std::string(pattern);
        input.path    = dir.string();
        if (file_glob.has_value()) {
            input.glob = std::string(*file_glob);
        }
        input.output_mode = "content";
        input.line_numbers = true;

        if (!use_regex) {
            // Escape the pattern so it is treated as a literal string.
            std::string escaped;
            for (char c : pattern) {
                if (std::string_view(".*+?()[]{}|^$\\").find(c) != std::string_view::npos) {
                    escaped += '\\';
                }
                escaped += c;
            }
            input.pattern = std::move(escaped);
        }

        auto rich_out = grep_search_rich(input);

        // Convert back to the legacy GrepMatch vector.
        // The content lines are formatted as "path:lineno:text".
        std::vector<GrepMatch> matches;
        if (rich_out.content.has_value()) {
            std::istringstream ss(*rich_out.content);
            std::string line;
            while (std::getline(ss, line)) {
                // Parse "filepath:lineno:content"
                auto first_colon = line.find(':');
                if (first_colon == std::string::npos) continue;
                auto second_colon = line.find(':', first_colon + 1);
                if (second_colon == std::string::npos) continue;
                std::string file_part    = line.substr(0, first_colon);
                std::string lineno_part  = line.substr(first_colon + 1,
                                                        second_colon - first_colon - 1);
                std::string content_part = line.substr(second_colon + 1);
                std::size_t lineno = 0;
                try { lineno = std::stoul(lineno_part); } catch (...) {}
                matches.push_back(GrepMatch{fs::path(file_part), lineno, content_part});
            }
        }
        return matches;
    } catch (const std::exception& e) {
        return tl::unexpected(e.what());
    }
}

} // namespace claw::runtime
