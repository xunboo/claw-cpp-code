#pragma once

#include <tl/expected.hpp>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "plugin_error.hpp"
#include "plugin_tool.hpp"
#include "plugin_types.hpp"

namespace claw::plugins {

// ─── IPlugin concept / abstract interface ─────────────────────────────────────
// Corresponds to Rust `trait Plugin`
class IPlugin {
public:
    virtual ~IPlugin() = default;
    [[nodiscard]] virtual const PluginMetadata&  metadata()  const noexcept = 0;
    [[nodiscard]] virtual const PluginHooks&     hooks()     const noexcept = 0;
    [[nodiscard]] virtual const PluginLifecycle& lifecycle() const noexcept = 0;
    [[nodiscard]] virtual std::span<const PluginTool> tools() const noexcept = 0;
    [[nodiscard]] virtual tl::expected<void, PluginError> validate()   const = 0;
    [[nodiscard]] virtual tl::expected<void, PluginError> initialize() const = 0;
    [[nodiscard]] virtual tl::expected<void, PluginError> shutdown()   const = 0;
};

// ─── BuiltinPlugin ────────────────────────────────────────────────────────────
struct BuiltinPlugin final : IPlugin {
    PluginMetadata  metadata_;
    PluginHooks     hooks_;
    PluginLifecycle lifecycle_;
    std::vector<PluginTool> tools_;

    [[nodiscard]] const PluginMetadata&  metadata()  const noexcept override { return metadata_;  }
    [[nodiscard]] const PluginHooks&     hooks()     const noexcept override { return hooks_;     }
    [[nodiscard]] const PluginLifecycle& lifecycle() const noexcept override { return lifecycle_; }
    [[nodiscard]] std::span<const PluginTool> tools() const noexcept override {
        return std::span{tools_};
    }
    [[nodiscard]] tl::expected<void, PluginError> validate()   const override { return {}; }
    [[nodiscard]] tl::expected<void, PluginError> initialize() const override { return {}; }
    [[nodiscard]] tl::expected<void, PluginError> shutdown()   const override { return {}; }
};

// ─── BundledPlugin ────────────────────────────────────────────────────────────
struct BundledPlugin final : IPlugin {
    PluginMetadata  metadata_;
    PluginHooks     hooks_;
    PluginLifecycle lifecycle_;
    std::vector<PluginTool> tools_;

    [[nodiscard]] const PluginMetadata&  metadata()  const noexcept override { return metadata_;  }
    [[nodiscard]] const PluginHooks&     hooks()     const noexcept override { return hooks_;     }
    [[nodiscard]] const PluginLifecycle& lifecycle() const noexcept override { return lifecycle_; }
    [[nodiscard]] std::span<const PluginTool> tools() const noexcept override {
        return std::span{tools_};
    }
    [[nodiscard]] tl::expected<void, PluginError> validate()   const override;
    [[nodiscard]] tl::expected<void, PluginError> initialize() const override;
    [[nodiscard]] tl::expected<void, PluginError> shutdown()   const override;
};

// ─── ExternalPlugin ───────────────────────────────────────────────────────────
struct ExternalPlugin final : IPlugin {
    PluginMetadata  metadata_;
    PluginHooks     hooks_;
    PluginLifecycle lifecycle_;
    std::vector<PluginTool> tools_;

    [[nodiscard]] const PluginMetadata&  metadata()  const noexcept override { return metadata_;  }
    [[nodiscard]] const PluginHooks&     hooks()     const noexcept override { return hooks_;     }
    [[nodiscard]] const PluginLifecycle& lifecycle() const noexcept override { return lifecycle_; }
    [[nodiscard]] std::span<const PluginTool> tools() const noexcept override {
        return std::span{tools_};
    }
    [[nodiscard]] tl::expected<void, PluginError> validate()   const override;
    [[nodiscard]] tl::expected<void, PluginError> initialize() const override;
    [[nodiscard]] tl::expected<void, PluginError> shutdown()   const override;
};

// ─── PluginDefinition (variant, equivalent to Rust enum) ─────────────────────
using PluginDefinition = std::variant<BuiltinPlugin, BundledPlugin, ExternalPlugin>;

// Helpers that dispatch through the variant — mirrors Rust `impl Plugin for PluginDefinition`
[[nodiscard]] const PluginMetadata&  plugin_metadata(const PluginDefinition& d) noexcept;
[[nodiscard]] const PluginHooks&     plugin_hooks(const PluginDefinition& d) noexcept;
[[nodiscard]] const PluginLifecycle& plugin_lifecycle(const PluginDefinition& d) noexcept;
[[nodiscard]] std::span<const PluginTool> plugin_tools(const PluginDefinition& d) noexcept;
[[nodiscard]] tl::expected<void, PluginError> plugin_validate(const PluginDefinition& d);
[[nodiscard]] tl::expected<void, PluginError> plugin_initialize(const PluginDefinition& d);
[[nodiscard]] tl::expected<void, PluginError> plugin_shutdown(const PluginDefinition& d);

// ─── PluginSummary ────────────────────────────────────────────────────────────
struct PluginSummary {
    PluginMetadata metadata;
    bool enabled{false};
};

// ─── PluginLoadFailure ────────────────────────────────────────────────────────
struct PluginLoadFailure {
    std::filesystem::path plugin_root;
    PluginKind kind{PluginKind::External};
    std::string source;
    std::unique_ptr<PluginError> error;

    PluginLoadFailure(std::filesystem::path root,
                      PluginKind k,
                      std::string src,
                      PluginError err);

    // Non-copyable because of unique_ptr; provide move
    PluginLoadFailure(PluginLoadFailure&&) noexcept = default;
    PluginLoadFailure& operator=(PluginLoadFailure&&) noexcept = default;
    PluginLoadFailure(const PluginLoadFailure&) = delete;
    PluginLoadFailure& operator=(const PluginLoadFailure&) = delete;

    [[nodiscard]] const PluginError& error_ref() const noexcept { return *error; }
    [[nodiscard]] std::string to_string() const;
};

// ─── RegisteredPlugin ─────────────────────────────────────────────────────────
class RegisteredPlugin {
public:
    RegisteredPlugin(PluginDefinition definition, bool enabled);

    [[nodiscard]] const PluginMetadata&       metadata()   const noexcept;
    [[nodiscard]] const PluginHooks&          hooks()      const noexcept;
    [[nodiscard]] std::span<const PluginTool> tools()      const noexcept;
    [[nodiscard]] bool                        is_enabled() const noexcept { return enabled_; }

    [[nodiscard]] tl::expected<void, PluginError> validate()   const;
    [[nodiscard]] tl::expected<void, PluginError> initialize() const;
    [[nodiscard]] tl::expected<void, PluginError> shutdown()   const;

    [[nodiscard]] PluginSummary summary() const;

    // Needed by PluginManager::discover_plugins
    PluginDefinition definition;

private:
    bool enabled_{false};
};

// ─── Builtin plugins factory ──────────────────────────────────────────────────
[[nodiscard]] std::vector<PluginDefinition> builtin_plugins();

}  // namespace claw::plugins
