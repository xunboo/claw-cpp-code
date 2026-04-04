// tests/test_init.cpp -- C++20 port of init.rs #[cfg(test)]
#include "../include/init.hpp"

using namespace claw;

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

static int g_passed = 0, g_failed = 0;
#define ASSERT_EQ(a, b) do { \
    if (!((a) == (b))) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #a " != " #b "\n"; \
        ++g_failed; \
    } else { ++g_passed; } \
} while(0)
#define ASSERT_TRUE(x) ASSERT_EQ(!!(x), true)
#define TEST(name) static void name(); \
    struct _Reg_##name { _Reg_##name(){ name(); } } _reg_##name; \
    static void name()

static std::filesystem::path temp_dir(const std::string& label) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto tmp = std::filesystem::temp_directory_path();
    return tmp / ("claw-cpp-init-" + label + "-" + std::to_string(now));
}

static std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p);
    return {std::istreambuf_iterator<char>(f), {}};
}

static std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0, pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) { ++count; pos += needle.size(); }
    return count;
}

TEST(initialize_repo_creates_expected_files_and_gitignore_entries) {
    auto root = temp_dir("create");
    std::filesystem::create_directories(root / "rust");
    std::ofstream(root / "rust" / "Cargo.toml") << "[workspace]\n";

    try {
        auto report = initialize_repo(root);
        auto rendered = report.render();

        ASSERT_TRUE(rendered.find(".claude/") != std::string::npos);
        ASSERT_TRUE(rendered.find(".claude.json") != std::string::npos);
        ASSERT_TRUE(rendered.find(".gitignore") != std::string::npos);
        ASSERT_TRUE(rendered.find("CLAUDE.md") != std::string::npos);

        ASSERT_TRUE(std::filesystem::is_directory(root / ".claude"));
        ASSERT_TRUE(std::filesystem::is_regular_file(root / ".claude.json"));
        ASSERT_TRUE(std::filesystem::is_regular_file(root / "CLAUDE.md"));

        auto claude_json = read_file(root / ".claude.json");
        ASSERT_TRUE(claude_json.find("dontAsk") != std::string::npos);

        auto gitignore = read_file(root / ".gitignore");
        ASSERT_TRUE(gitignore.find(".claude/settings.local.json") != std::string::npos);
        ASSERT_TRUE(gitignore.find(".claude/sessions/") != std::string::npos);

        auto claude_md = read_file(root / "CLAUDE.md");
        ASSERT_TRUE(claude_md.find("Languages: Rust.") != std::string::npos);
        ASSERT_TRUE(claude_md.find("cargo clippy") != std::string::npos);

    } catch (std::exception& e) {
        std::cerr << "FAIL initialize_repo_creates: " << e.what() << "\n"; ++g_failed;
    }
    std::filesystem::remove_all(root);
}

TEST(initialize_repo_is_idempotent_and_preserves_existing_files) {
    auto root = temp_dir("idempotent");
    std::filesystem::create_directories(root);
    std::ofstream(root / "CLAUDE.md") << "custom guidance\n";
    std::ofstream(root / ".gitignore") << ".claude/settings.local.json\n";

    try {
        auto first = initialize_repo(root);
        ASSERT_TRUE(first.render().find("skipped (already exists)") != std::string::npos);

        auto second = initialize_repo(root);
        auto r = second.render();
        ASSERT_TRUE(r.find("skipped (already exists)") != std::string::npos);

        ASSERT_EQ(read_file(root / "CLAUDE.md"), "custom guidance\n");

        auto gitignore = read_file(root / ".gitignore");
        ASSERT_EQ(count_occurrences(gitignore, ".claude/settings.local.json"), 1u);
        ASSERT_EQ(count_occurrences(gitignore, ".claude/sessions/"), 1u);

    } catch (std::exception& e) {
        std::cerr << "FAIL idempotent: " << e.what() << "\n"; ++g_failed;
    }
    std::filesystem::remove_all(root);
}

TEST(render_init_template_mentions_python_and_nextjs_markers) {
    auto root = temp_dir("template");
    std::filesystem::create_directories(root);
    std::ofstream(root / "pyproject.toml") << "[project]\nname = \"demo\"\n";
    std::ofstream(root / "package.json")
        << R"({"dependencies":{"next":"14.0.0","react":"18.0.0"},"devDependencies":{"typescript":"5.0.0"}})";

    try {
        auto rendered = render_init_claude_md(root);
        ASSERT_TRUE(rendered.find("Languages: Python, TypeScript.") != std::string::npos);
        ASSERT_TRUE(rendered.find("Frameworks/tooling markers: Next.js, React.") != std::string::npos);
        ASSERT_TRUE(rendered.find("pyproject.toml") != std::string::npos);
        ASSERT_TRUE(rendered.find("Next.js detected") != std::string::npos);

    } catch (std::exception& e) {
        std::cerr << "FAIL template: " << e.what() << "\n"; ++g_failed;
    }
    std::filesystem::remove_all(root);
}

int main() {
    std::cout << "\nTest results: " << g_passed << " passed, " << g_failed << " failed.\n";
    return (g_failed > 0) ? 1 : 0;
}