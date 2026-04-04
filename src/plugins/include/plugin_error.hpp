#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace claw::plugins {

// Forward declarations
struct PluginLoadFailure;

enum class PluginManifestValidationErrorKind {
    EmptyField,
    EmptyEntryField,
    InvalidPermission,
    DuplicatePermission,
    DuplicateEntry,
    MissingPath,
    PathIsDirectory,
    InvalidToolInputSchema,
    InvalidToolRequiredPermission,
};

struct PluginManifestValidationError {
    PluginManifestValidationErrorKind kind;
    // Fields (populated depending on kind)
    const char* field{nullptr};       // EmptyField, EmptyEntryField
    const char* entry_kind{nullptr};  // EmptyEntryField, DuplicateEntry, MissingPath, PathIsDirectory
    std::string name;                 // EmptyEntryField (optional), DuplicateEntry
    std::string permission;           // InvalidPermission, DuplicatePermission
    std::filesystem::path path;       // MissingPath, PathIsDirectory
    std::string tool_name;            // InvalidToolInputSchema, InvalidToolRequiredPermission

    [[nodiscard]] std::string to_string() const;
};

enum class PluginErrorKind { Io, Json, ManifestValidation, LoadFailures, InvalidManifest, NotFound, CommandFailed };

class PluginError : public std::exception {
public:
    // Io error
    explicit PluginError(std::error_code io_error);
    // Json parse error
    static PluginError json(std::string msg);
    // Manifest validation
    static PluginError manifest_validation(std::vector<PluginManifestValidationError> errors);
    // Load failures
    static PluginError load_failures(std::vector<PluginLoadFailure> failures);
    // Simple string errors
    static PluginError invalid_manifest(std::string msg);
    static PluginError not_found(std::string msg);
    static PluginError command_failed(std::string msg);

    [[nodiscard]] PluginErrorKind kind() const noexcept { return kind_; }
    [[nodiscard]] const char* what() const noexcept override;

    // Accessors for typed payloads
    [[nodiscard]] const std::vector<PluginManifestValidationError>& validation_errors() const;
    [[nodiscard]] const std::vector<PluginLoadFailure>& load_failures_ref() const;

private:
    PluginError() = default;
    PluginErrorKind kind_{PluginErrorKind::Io};
    mutable std::string message_;
    std::vector<PluginManifestValidationError> validation_errors_;
    std::vector<PluginLoadFailure>* load_failures_ptr_{nullptr};  // owning
    std::shared_ptr<std::vector<PluginLoadFailure>> load_failures_storage_;
};

}  // namespace claw::plugins
