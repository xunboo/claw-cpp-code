//
// compat_harness_test.cpp
// C++20 port of the #[cfg(test)] mod tests block in lib.rs
//
// Uses the Catch2 v3 test framework.  Add -DBUILD_TESTS=ON to cmake to build.
//

#include "compat_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Mirrors `fixture_paths()` in the Rust test module.
// Walks two levels up from __FILE__ to reach the workspace root, then
// delegates to UpstreamPaths::from_workspace_dir.
claw::UpstreamPaths fixture_paths() {
    auto here = std::filesystem::path{__FILE__}   // …/compat-harness/src/compat_harness_test.cpp
                    .parent_path()                 // …/compat-harness/src
                    .parent_path()                 // …/compat-harness
                    .parent_path();                // workspace root (approximate)
    return claw::UpstreamPaths::from_workspace_dir(here);
}

bool has_upstream_fixture(const claw::UpstreamPaths& paths) {
    std::error_code ec;
    return std::filesystem::is_regular_file(paths.commands_path(), ec) &&
           std::filesystem::is_regular_file(paths.tools_path(),    ec) &&
           std::filesystem::is_regular_file(paths.cli_path(),      ec);
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::in | std::ios::binary);
    std::string   content((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());
    return content;
}

template<typename Entry>
bool contains_name(std::span<const Entry> entries, std::string_view name) {
    return std::any_of(entries.begin(), entries.end(),
                       [&](const Entry& e) { return e.name == name; });
}

} // namespace

// ---------------------------------------------------------------------------
// Test: extracts non-empty manifests when upstream fixture is present
// ---------------------------------------------------------------------------
TEST_CASE("extracts non-empty manifests from upstream repo",
          "[compat_harness][integration]") {
    auto paths = fixture_paths();
    if (!has_upstream_fixture(paths)) {
        SKIP("upstream fixture not present");
    }

    auto result = claw::extract_manifest(paths);
    REQUIRE(result.has_value());

    CHECK(!result->commands.entries().empty());
    CHECK(!result->tools.entries().empty());
    CHECK(!result->bootstrap.phases().empty());
}

// ---------------------------------------------------------------------------
// Test: detects known upstream command symbols
// ---------------------------------------------------------------------------
TEST_CASE("detects known upstream command symbols",
          "[compat_harness][integration]") {
    auto paths = fixture_paths();
    if (!std::filesystem::is_regular_file(paths.commands_path())) {
        SKIP("commands.ts not present");
    }

    auto commands = claw::extract_commands(read_file(paths.commands_path()));
    auto entries  = commands.entries();

    CHECK(contains_name(entries, "addDir"));
    CHECK(contains_name(entries, "review"));
    CHECK(!contains_name(entries, "INTERNAL_ONLY_COMMANDS"));
}

// ---------------------------------------------------------------------------
// Test: detects known upstream tool symbols
// ---------------------------------------------------------------------------
TEST_CASE("detects known upstream tool symbols",
          "[compat_harness][integration]") {
    auto paths = fixture_paths();
    if (!std::filesystem::is_regular_file(paths.tools_path())) {
        SKIP("tools.ts not present");
    }

    auto tools   = claw::extract_tools(read_file(paths.tools_path()));
    auto entries = tools.entries();

    CHECK(contains_name(entries, "AgentTool"));
    CHECK(contains_name(entries, "BashTool"));
}

// ---------------------------------------------------------------------------
// Unit test: extract_commands parses a synthetic snippet correctly
// ---------------------------------------------------------------------------
TEST_CASE("extract_commands parses synthetic source", "[compat_harness][unit]") {
    constexpr std::string_view source = R"ts(
import { foo, bar } from './some-module'
import baz from './other-module'
export const INTERNAL_ONLY_COMMANDS = [
  internalCmd,
]
const gated = feature('./commands/gated-cmd')
)ts";

    auto registry = claw::extract_commands(source);
    auto entries  = registry.entries();

    CHECK(contains_name(entries, "foo"));
    CHECK(contains_name(entries, "bar"));
    CHECK(contains_name(entries, "baz"));
    CHECK(contains_name(entries, "internalCmd"));
    CHECK(!contains_name(entries, "INTERNAL_ONLY_COMMANDS"));
}

// ---------------------------------------------------------------------------
// Unit test: extract_tools parses a synthetic snippet correctly
// ---------------------------------------------------------------------------
TEST_CASE("extract_tools parses synthetic source", "[compat_harness][unit]") {
    constexpr std::string_view source = R"ts(
import { BashTool, GlobTool } from './tools/bash'
const conditionalTool = feature('x')
)ts";

    auto registry = claw::extract_tools(source);
    auto entries  = registry.entries();

    CHECK(contains_name(entries, "BashTool"));
    CHECK(contains_name(entries, "GlobTool"));
    CHECK(contains_name(entries, "conditionalTool"));
}

// ---------------------------------------------------------------------------
// Unit test: extract_bootstrap_plan detects phases from synthetic CLI source
// ---------------------------------------------------------------------------
TEST_CASE("extract_bootstrap_plan detects phases", "[compat_harness][unit]") {
    constexpr std::string_view source = R"tsx(
if (args.includes('--version')) { ... }
if (startupProfiler) { ... }
if (args.includes('--dump-system-prompt')) { ... }
if (args[0] === 'daemon') { ... }
if (args[0] === 'ps') { ... }
// main runtime
)tsx";

    auto plan   = claw::extract_bootstrap_plan(source);
    auto phases = plan.phases();

    using P = claw::BootstrapPhase;
    auto has_phase = [&](P p) {
        return std::find(phases.begin(), phases.end(), p) != phases.end();
    };

    CHECK(has_phase(P::CliEntry));
    CHECK(has_phase(P::FastPathVersion));
    CHECK(has_phase(P::StartupProfiler));
    CHECK(has_phase(P::SystemPromptFastPath));
    CHECK(has_phase(P::DaemonFastPath));
    CHECK(has_phase(P::BackgroundSessionFastPath));
    CHECK(has_phase(P::MainRuntime));
    // Not mentioned in the source:
    CHECK(!has_phase(P::ChromeMcpFastPath));
}

// ---------------------------------------------------------------------------
// Unit test: BootstrapPlan::from_phases deduplicates while preserving order
// ---------------------------------------------------------------------------
TEST_CASE("BootstrapPlan::from_phases deduplicates preserving order",
          "[bootstrap][unit]") {
    using P = claw::BootstrapPhase;
    auto plan = claw::BootstrapPlan::from_phases({
        P::CliEntry,
        P::FastPathVersion,
        P::CliEntry,
        P::MainRuntime,
        P::FastPathVersion,
    });

    auto phases = plan.phases();
    REQUIRE(phases.size() == 3);
    CHECK(phases[0] == P::CliEntry);
    CHECK(phases[1] == P::FastPathVersion);
    CHECK(phases[2] == P::MainRuntime);
}