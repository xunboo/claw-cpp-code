#include "plugin_error.hpp"
#include "plugin.hpp"   // PluginLoadFailure

#include <format>

namespace claw::plugins {

// ─── PluginManifestValidationError ────────────────────────────────────────────

std::string PluginManifestValidationError::to_string() const {
    switch (kind) {
        case PluginManifestValidationErrorKind::EmptyField:
            return std::format("plugin manifest {} cannot be empty", field ? field : "");

        case PluginManifestValidationErrorKind::EmptyEntryField:
            // Mirrors Rust: if name is Some(name) && !name.is_empty() → include name
            if (!name.empty())
                return std::format("plugin {} `{}` {} cannot be empty",
                                   entry_kind ? entry_kind : "",
                                   name,
                                   field ? field : "");
            return std::format("plugin {} {} cannot be empty",
                               entry_kind ? entry_kind : "",
                               field ? field : "");

        case PluginManifestValidationErrorKind::InvalidPermission:
            return std::format(
                "plugin manifest permission `{}` must be one of read, write, or execute",
                permission);

        case PluginManifestValidationErrorKind::DuplicatePermission:
            return std::format("plugin manifest permission `{}` is duplicated", permission);

        case PluginManifestValidationErrorKind::DuplicateEntry:
            return std::format("plugin {} `{}` is duplicated",
                               entry_kind ? entry_kind : "", name);

        case PluginManifestValidationErrorKind::MissingPath:
            return std::format("{} path `{}` does not exist",
                               entry_kind ? entry_kind : "", path.string());

        case PluginManifestValidationErrorKind::PathIsDirectory:
            return std::format("{} path `{}` must point to a file",
                               entry_kind ? entry_kind : "", path.string());

        case PluginManifestValidationErrorKind::InvalidToolInputSchema:
            return std::format("plugin tool `{}` inputSchema must be a JSON object", tool_name);

        case PluginManifestValidationErrorKind::InvalidToolRequiredPermission:
            return std::format(
                "plugin tool `{}` requiredPermission `{}` must be read-only, "
                "workspace-write, or danger-full-access",
                tool_name, permission);
    }
    return {};
}

// ─── PluginError ──────────────────────────────────────────────────────────────

PluginError::PluginError(std::error_code io_error)
    : kind_(PluginErrorKind::Io), message_(io_error.message()) {}

PluginError PluginError::json(std::string msg) {
    PluginError e;
    e.kind_    = PluginErrorKind::Json;
    e.message_ = std::move(msg);
    return e;
}

PluginError PluginError::manifest_validation(std::vector<PluginManifestValidationError> errors) {
    PluginError e;
    e.kind_              = PluginErrorKind::ManifestValidation;
    e.validation_errors_ = std::move(errors);
    // Build message by joining all validation error strings with "; "
    bool first = true;
    for (auto& ve : e.validation_errors_) {
        if (!first) e.message_ += "; ";
        e.message_ += ve.to_string();
        first = false;
    }
    return e;
}

PluginError PluginError::load_failures(std::vector<PluginLoadFailure> failures) {
    PluginError e;
    e.kind_ = PluginErrorKind::LoadFailures;
    e.load_failures_storage_ =
        std::make_shared<std::vector<PluginLoadFailure>>(std::move(failures));
    bool first = true;
    for (auto& f : *e.load_failures_storage_) {
        if (!first) e.message_ += "; ";
        e.message_ += f.to_string();
        first = false;
    }
    return e;
}

PluginError PluginError::invalid_manifest(std::string msg) {
    PluginError e;
    e.kind_    = PluginErrorKind::InvalidManifest;
    e.message_ = std::move(msg);
    return e;
}

PluginError PluginError::not_found(std::string msg) {
    PluginError e;
    e.kind_    = PluginErrorKind::NotFound;
    e.message_ = std::move(msg);
    return e;
}

PluginError PluginError::command_failed(std::string msg) {
    PluginError e;
    e.kind_    = PluginErrorKind::CommandFailed;
    e.message_ = std::move(msg);
    return e;
}

const char* PluginError::what() const noexcept {
    return message_.c_str();
}

const std::vector<PluginManifestValidationError>& PluginError::validation_errors() const {
    return validation_errors_;
}

const std::vector<PluginLoadFailure>& PluginError::load_failures_ref() const {
    static const std::vector<PluginLoadFailure> empty;
    return load_failures_storage_ ? *load_failures_storage_ : empty;
}

}  // namespace claw::plugins
