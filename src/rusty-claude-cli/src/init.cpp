// init.cpp -- C++20 port of init.rs
// Repository initialisation: detect_repo, render_init_claude_md, initialize_repo.
#include "init.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace claw {

namespace {

// ---- STARTER_CLAUDE_JSON ----
constexpr std::string_view STARTER_CLAUDE_JSON =
    "{\n"
    "  \"permissions\": {\n"
    "    \"defaultMode\": \"dontAsk\"\n"
    "  }\n"
    "}\n";

constexpr std::string_view GITIGNORE_COMMENT = "# Claw Code local artifacts";

const std::vector<std::string_view> GITIGNORE_ENTRIES = {
    ".claude/settings.local.json",
    ".claude/sessions/",
};

// ---- I/O helpers ----

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}

void write_file(const std::filesystem::path& p, std::string_view content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + p.string());
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// ---- Filesystem helpers matching Rust functions ----

/// Mirrors Rust's ensure_dir.
InitStatus ensure_dir(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) return InitStatus::Skipped;
    std::filesystem::create_directories(path);
    return InitStatus::Created;
}

/// Mirrors Rust's write_file_if_missing.
InitStatus write_file_if_missing(const std::filesystem::path& path,
                                  std::string_view content) {
    if (std::filesystem::exists(path)) return InitStatus::Skipped;
    write_file(path, content);
    return InitStatus::Created;
}

/// Mirrors Rust's ensure_gitignore_entries.
InitStatus ensure_gitignore_entries(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        // Create a fresh .gitignore.
        std::ostringstream oss;
        oss << GITIGNORE_COMMENT << "\n";
        for (auto& e : GITIGNORE_ENTRIES) oss << e << "\n";
        write_file(path, oss.str());
        return InitStatus::Created;
    }

    std::string existing = read_file(path);
    std::vector<std::string> lines;
    {
        std::istringstream ss(existing);
        std::string line;
        while (std::getline(ss, line)) lines.push_back(line);
    }

    bool changed = false;
    auto has_line = [&](std::string_view needle) {
        return std::any_of(lines.begin(), lines.end(),
            [&](const std::string& l){ return l == needle; });
    };

    if (!has_line(GITIGNORE_COMMENT)) {
        lines.emplace_back(GITIGNORE_COMMENT);
        changed = true;
    }
    for (auto& e : GITIGNORE_ENTRIES) {
        if (!has_line(e)) {
            lines.emplace_back(e);
            changed = true;
        }
    }
    if (!changed) return InitStatus::Skipped;

    std::ostringstream oss;
    for (auto& l : lines) oss << l << "\n";
    write_file(path, oss.str());
    return InitStatus::Updated;
}

// ---- join helper ----
template<class Container>
std::string join(const Container& c, std::string_view sep) {
    std::string out;
    bool first = true;
    for (const auto& e : c) {
        if (!first) out += sep;
        out += e;
        first = false;
    }
    return out;
}

// ---- Detection helpers ----

std::vector<std::string_view> detected_languages(const RepoDetection& d) {
    std::vector<std::string_view> langs;
    if (d.rust_workspace || d.rust_root) langs.push_back("Rust");
    if (d.python)                         langs.push_back("Python");
    if (d.typescript)                     langs.push_back("TypeScript");
    else if (d.package_json)              langs.push_back("JavaScript/Node.js");
    return langs;
}

std::vector<std::string_view> detected_frameworks(const RepoDetection& d) {
    std::vector<std::string_view> fw;
    if (d.nextjs) fw.push_back("Next.js");
    if (d.react)  fw.push_back("React");
    if (d.vite)   fw.push_back("Vite");
    if (d.nest)   fw.push_back("NestJS");
    return fw;
}

/// Mirrors Rust's verification_lines.
std::vector<std::string> verification_lines(const std::filesystem::path& cwd,
                                             const RepoDetection& d) {
    std::vector<std::string> lines;
    if (d.rust_workspace) {
        lines.push_back("- Run Rust verification from `rust/`: `cargo fmt`, "
            "`cargo clippy --workspace --all-targets -- -D warnings`, "
            "`cargo test --workspace`");
    } else if (d.rust_root) {
        lines.push_back("- Run Rust verification from the repo root: `cargo fmt`, "
            "`cargo clippy --workspace --all-targets -- -D warnings`, "
            "`cargo test --workspace`");
    }
    if (d.python) {
        if (std::filesystem::exists(cwd / "pyproject.toml"))
            lines.push_back("- Run the Python project checks declared in `pyproject.toml` "
                "(for example: `pytest`, `ruff check`, and `mypy` when configured).");
        else
            lines.push_back("- Run the repo's Python test/lint commands before shipping changes.");
    }
    if (d.package_json)
        lines.push_back("- Run the JavaScript/TypeScript checks from `package.json` before "
            "shipping changes (`npm test`, `npm run lint`, `npm run build`, or the repo equivalent).");
    if (d.tests_dir && d.src_dir)
        lines.push_back("- `src/` and `tests/` are both present; update both surfaces together "
            "when behavior changes.");
    return lines;
}

/// Mirrors Rust's repository_shape_lines.
std::vector<std::string> repository_shape_lines(const RepoDetection& d) {
    std::vector<std::string> lines;
    if (d.rust_dir)
        lines.push_back("- `rust/` contains the Rust workspace and active CLI/runtime implementation.");
    if (d.src_dir)
        lines.push_back("- `src/` contains source files that should stay consistent with "
            "generated guidance and tests.");
    if (d.tests_dir)
        lines.push_back("- `tests/` contains validation surfaces that should be reviewed alongside "
            "code changes.");
    return lines;
}

/// Mirrors Rust's framework_notes.
std::vector<std::string> framework_notes(const RepoDetection& d) {
    std::vector<std::string> lines;
    if (d.nextjs)
        lines.push_back("- Next.js detected: preserve routing/data-fetching conventions and "
            "verify production builds after changing app structure.");
    if (d.react && !d.nextjs)
        lines.push_back("- React detected: keep component behavior covered with focused tests "
            "and avoid unnecessary prop/API churn.");
    if (d.vite)
        lines.push_back("- Vite detected: validate the production bundle after changing "
            "build-sensitive configuration or imports.");
    if (d.nest)
        lines.push_back("- NestJS detected: keep module/provider boundaries explicit and "
            "verify controller/service wiring after refactors.");
    return lines;
}

} // anonymous namespace

// ---- Public API ----

/// Mirrors Rust's detect_repo.
RepoDetection detect_repo(const std::filesystem::path& cwd) {
    std::string pkg_json;
    auto pkg = cwd / "package.json";
    if (std::filesystem::is_regular_file(pkg)) {
        try { pkg_json = read_file(pkg); } catch (...) {}
        std::transform(pkg_json.begin(), pkg_json.end(), pkg_json.begin(),
            [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    }
    auto contains = [&](std::string_view s) {
        return pkg_json.find(s) != std::string::npos;
    };
    return RepoDetection{
        .rust_workspace = std::filesystem::is_regular_file(cwd / "rust" / "Cargo.toml"),
        .rust_root      = std::filesystem::is_regular_file(cwd / "Cargo.toml"),
        .python         = std::filesystem::is_regular_file(cwd / "pyproject.toml")
                       || std::filesystem::is_regular_file(cwd / "requirements.txt")
                       || std::filesystem::is_regular_file(cwd / "setup.py"),
        .package_json   = std::filesystem::is_regular_file(cwd / "package.json"),
        .typescript     = std::filesystem::is_regular_file(cwd / "tsconfig.json")
                       || contains("typescript"),
        .nextjs         = contains("\"next\""),
        .react          = contains("\"react\""),
        .vite           = contains("\"vite\""),
        .nest           = contains("@nestjs"),
        .src_dir        = std::filesystem::is_directory(cwd / "src"),
        .tests_dir      = std::filesystem::is_directory(cwd / "tests"),
        .rust_dir       = std::filesystem::is_directory(cwd / "rust"),
    };
}

/// Mirrors Rust's render_init_claude_md.
std::string render_init_claude_md(const std::filesystem::path& cwd) {
    const auto d = detect_repo(cwd);
    std::vector<std::string> lines;

    lines.emplace_back("# CLAUDE.md");
    lines.emplace_back("");
    lines.emplace_back(
        "This file provides guidance to Claw Code (clawcode.dev) when working with "
        "code in this repository.");
    lines.emplace_back("");

    lines.emplace_back("## Detected stack");
    auto langs = detected_languages(d);
    if (langs.empty())
        lines.emplace_back(
            "- No specific language markers were detected yet; document the primary "
            "language and verification commands once the project structure settles.");
    else
        lines.emplace_back("- Languages: " + join(langs, ", ") + ".");

    auto fws = detected_frameworks(d);
    if (fws.empty())
        lines.emplace_back("- Frameworks: none detected from the supported starter markers.");
    else
        lines.emplace_back("- Frameworks/tooling markers: " + join(fws, ", ") + ".");
    lines.emplace_back("");

    auto vlines = verification_lines(cwd, d);
    if (!vlines.empty()) {
        lines.emplace_back("## Verification");
        for (auto& l : vlines) lines.push_back(l);
        lines.emplace_back("");
    }

    auto slines = repository_shape_lines(d);
    if (!slines.empty()) {
        lines.emplace_back("## Repository shape");
        for (auto& l : slines) lines.push_back(l);
        lines.emplace_back("");
    }

    auto fnotes = framework_notes(d);
    if (!fnotes.empty()) {
        lines.emplace_back("## Framework notes");
        for (auto& l : fnotes) lines.push_back(l);
        lines.emplace_back("");
    }

    lines.emplace_back("## Working agreement");
    lines.emplace_back("- Prefer small, reviewable changes and keep generated bootstrap files "
        "aligned with actual repo workflows.");
    lines.emplace_back("- Keep shared defaults in `.claude.json`; reserve "
        "`.claude/settings.local.json` for machine-local overrides.");
    lines.emplace_back("- Do not overwrite existing `CLAUDE.md` content automatically; "
        "update it intentionally when repo workflows change.");
    lines.emplace_back("");

    // join with newlines (no trailing newline after the final "").
    std::ostringstream oss;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        oss << lines[i];
        if (i + 1 < lines.size()) oss << "\n";
    }
    return oss.str();
}

/// Mirrors Rust's initialize_repo.
InitReport initialize_repo(const std::filesystem::path& cwd) {
    InitReport report;
    report.project_root = cwd;

    report.artifacts.push_back({".claude/",   ensure_dir(cwd / ".claude")});
    report.artifacts.push_back({".claude.json",
        write_file_if_missing(cwd / ".claude.json", STARTER_CLAUDE_JSON)});
    report.artifacts.push_back({".gitignore",
        ensure_gitignore_entries(cwd / ".gitignore")});
    auto content = render_init_claude_md(cwd);
    report.artifacts.push_back({"CLAUDE.md",
        write_file_if_missing(cwd / "CLAUDE.md", content)});

    return report;
}

/// Mirrors Rust's InitReport::render.
std::string InitReport::render() const {
    std::ostringstream oss;
    oss << "Init\n";
    oss << "  Project          " << project_root.string() << "\n";
    for (auto& a : artifacts) {
        // left-pad artifact name to 16 chars.
        std::string padded = a.name;
        while (padded.size() < 16) padded += ' ';
        oss << "  " << padded << " " << init_status_label(a.status) << "\n";
    }
    oss << "  Next step        Review and tailor the generated guidance";
    return oss.str();
}

} // namespace claw
