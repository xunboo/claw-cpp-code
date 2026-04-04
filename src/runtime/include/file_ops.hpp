#pragma once
#include <string>
#include <vector>
#include <optional>
#include <tl/expected.hpp>
#include <filesystem>

namespace claw::runtime {

inline constexpr std::size_t FILE_READ_MAX_BYTES = 1'000'000; // 1MB soft limit

struct GrepMatch {
    std::filesystem::path file;
    std::size_t line_number;
    std::string line_content;
};

// Read a file, respecting workspace boundary
[[nodiscard]] tl::expected<std::string, std::string>
    read_file(const std::filesystem::path& path,
              const std::filesystem::path& workspace_root,
              std::optional<std::size_t> max_bytes = std::nullopt);

// Write a file atomically (via temp file + rename)
[[nodiscard]] tl::expected<void, std::string>
    write_file(const std::filesystem::path& path,
               std::string_view content,
               const std::filesystem::path& workspace_root);

// Edit a file: replace old_string with new_string (first occurrence)
[[nodiscard]] tl::expected<void, std::string>
    edit_file(const std::filesystem::path& path,
              std::string_view old_string,
              std::string_view new_string,
              const std::filesystem::path& workspace_root);

// Glob search within a directory
[[nodiscard]] tl::expected<std::vector<std::filesystem::path>, std::string>
    glob_search(const std::filesystem::path& dir,
                std::string_view pattern);

// Grep search: regex or literal search
[[nodiscard]] tl::expected<std::vector<GrepMatch>, std::string>
    grep_search(const std::filesystem::path& dir,
                std::string_view pattern,
                bool use_regex = true,
                std::optional<std::string_view> file_glob = std::nullopt);

// Check that path is within workspace_root (prevents path traversal)
[[nodiscard]] bool is_within_workspace(const std::filesystem::path& path,
                                       const std::filesystem::path& workspace_root);

} // namespace claw::runtime
