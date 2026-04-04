#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <span>

namespace claw {

// ---------------------------------------------------------------------------
// ToolSource
// ---------------------------------------------------------------------------

enum class ToolSource {
    Base,
    Conditional,
};

// ---------------------------------------------------------------------------
// ToolManifestEntry
// ---------------------------------------------------------------------------

struct ToolManifestEntry {
    std::string name;
    ToolSource  source;

    [[nodiscard]] bool operator==(const ToolManifestEntry& other) const noexcept {
        return name == other.name && source == other.source;
    }
};

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------

class ToolRegistry {
public:
    ToolRegistry() = default;

    explicit ToolRegistry(std::vector<ToolManifestEntry> entries)
        : entries_(std::move(entries)) {}

    [[nodiscard]] std::span<const ToolManifestEntry> entries() const noexcept {
        return entries_;
    }

    [[nodiscard]] bool operator==(const ToolRegistry& other) const noexcept {
        return entries_ == other.entries_;
    }

private:
    std::vector<ToolManifestEntry> entries_;
};

} // namespace claw