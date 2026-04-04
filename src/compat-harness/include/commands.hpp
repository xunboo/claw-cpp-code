#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <span>

namespace claw {

// ---------------------------------------------------------------------------
// CommandSource
// ---------------------------------------------------------------------------

enum class CommandSource {
    Builtin,
    InternalOnly,
    FeatureGated,
};

// ---------------------------------------------------------------------------
// CommandManifestEntry
// ---------------------------------------------------------------------------

struct CommandManifestEntry {
    std::string   name;
    CommandSource source;

    [[nodiscard]] bool operator==(const CommandManifestEntry& other) const noexcept {
        return name == other.name && source == other.source;
    }
};

// ---------------------------------------------------------------------------
// CommandRegistry
// ---------------------------------------------------------------------------

class CommandRegistry {
public:
    CommandRegistry() = default;

    explicit CommandRegistry(std::vector<CommandManifestEntry> entries)
        : entries_(std::move(entries)) {}

    [[nodiscard]] std::span<const CommandManifestEntry> entries() const noexcept {
        return entries_;
    }

    [[nodiscard]] bool operator==(const CommandRegistry& other) const noexcept {
        return entries_ == other.entries_;
    }

private:
    std::vector<CommandManifestEntry> entries_;
};

// ---------------------------------------------------------------------------
// SlashCommandSpec  (compile-time static table entry)
// ---------------------------------------------------------------------------

struct SlashCommandSpec {
    std::string_view              name;
    std::string_view              summary;
    std::optional<std::string_view> argument_hint;
    bool                          resume_supported;
};

} // namespace claw