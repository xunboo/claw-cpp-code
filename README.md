# Claw C++ Code

```
 ██████╗██╗      █████╗ ██╗    ██╗
██╔════╝██║     ██╔══██╗██║    ██║
██║     ██║     ███████║██║ █╗ ██║
██║     ██║     ██╔══██║██║███╗██║
╚██████╗███████╗██║  ██║╚███╔███╔╝
 ╚═════╝╚══════╝╚═╝  ╚═╝ ╚══╝╚══╝ Code
```

A complete **C++20** port of the [claw-code](https://github.com/instructkr/claw-code)
Rust workspace — a clean-room reimplementation of the Claude Code agent harness.
Every crate in the Rust source tree is mapped 1-to-1 to a C++ static library or
executable, preserving the same function signatures, control flow, and module
boundaries.

## Highlights

- **150 source files, ~40 000 lines of C++20** across 9 modules
- Builds with **Visual Studio 2022** (MSVC 14.44+) on Windows 11
- C++20 features for the best performance
- CMake 3.25+ mono-repo with FetchContent for all dependencies (no vcpkg needed)
- Windows-native: BCrypt for crypto, `CreateProcess` for shell execution, UTF-8 console output
- **All 48 tool dispatch entries** match the Rust source 1-to-1
- Real Anthropic API calls via `AnthropicClient` (set `ANTHROPIC_API_KEY` to use)

## Repository layout

```
claw-cpp-code/
  CMakeLists.txt              Top-level CMake (delegates to src/)
  README.md                   This file
  build/                      VS 2022 solution (generated)
  src/
    CMakeLists.txt             Mono-repo root: global flags, FetchContent, sub-projects
    runtime/                   Core engine — session, MCP, config, hooks,
                               permissions, bash, file-ops, OAuth, sandbox, policy
    api/                       HTTP client for Anthropic / OpenAI-compat providers, SSE
    plugins/                   Plugin registry, manager, types, tools, hooks
    tools/                     Tool registry, specs, executor (web, agent, misc)
    commands/                  Slash-command registry and dispatch
    rusty-claude-cli/          CLI app: args, init, input, render, REPL + tests
    telemetry/                 Lightweight metrics collection
    compat-harness/            Compatibility shim and bootstrap helpers
    mock-anthropic-service/    Mock HTTP server for integration tests
```

## Crate-to-module mapping

| Rust crate | C++ target | Type | Lines |
|---|---|---|---|
| `runtime` | `claw_runtime` | Static lib | 17 593 |
| `api` | `claw_api` | Static lib | 4 258 |
| `plugins` | `claw_plugins` | Static lib | 3 303 |
| `tools` | `claw_tools` | Static lib | 4 397 |
| `commands` | `commands` | Static lib | 3 296 |
| `rusty-claude-cli` | `claw_lib` + `claw.exe` | Lib + EXE | 4 572 |
| `telemetry` | `claw_telemetry` | Static lib | 725 |
| `compat-harness` | `compat-harness` | Static lib | 859 |
| `mock-anthropic-service` | `mock-anthropic-service.exe` | EXE | 1 055 |
| **Total** | | | **40 058** |

## Dependencies (all fetched automatically)

| Library | Version | Purpose |
|---|---|---|
| [tl::expected](https://github.com/TartanLlama/expected) | 1.1+ | C++20 backport of `std::expected` |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11+ | JSON parsing and serialization |
| [libcurl](https://github.com/curl/curl) | 8.19.0 | HTTP client (Anthropic API, web tools) |

No vcpkg, Conan, or manual library installation required. CMake FetchContent
downloads, builds, and links everything from source on first configure.

## Build instructions

### Prerequisites

- **Visual Studio 2022** with C++ desktop workload (MSVC v14.44+)
- **CMake 3.25+** (bundled with VS 2022)
- **Git** (for FetchContent to clone dependencies)

### Build

```powershell
cd claw-cpp-code
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Debug
```

First build takes ~5 minutes (fetches and compiles curl from source).
Subsequent builds are incremental.

### Run

```powershell
# Show version
.\bin\Release\claw.exe --version

# Show help
.\bin\Release\claw.exe --help

# Print status snapshot
.\bin\Release\claw.exe status

# Print bootstrap plan
.\bin\Release\claw.exe bootstrap-plan

# OAuth login flow
.\bin\Release\claw.exe login

# Initialize project (creates CLAUDE.md, .claude/, .gitignore entries)
.\bin\Release\claw.exe init

# List agents / skills / MCP servers
.\bin\Release\claw.exe agents
.\bin\Release\claw.exe skills
.\bin\Release\claw.exe mcp

# One-shot prompt (requires ANTHROPIC_API_KEY)
set ANTHROPIC_API_KEY=sk-ant-...
.\bin\Release\claw.exe -p "explain what this project does"

# Start interactive REPL
.\bin\Release\claw.exe
```

### Run tests

```powershell
.\bin\Release\test_args.exe
.\bin\Release\test_init.exe
.\bin\Release\test_render.exe
```

## Tool executor dispatch

All 48 tools from the Rust source are dispatched with full parity:

| Category | Tools | Runtime wiring |
|---|---|---|
| File / bash | `bash`, `read_file`, `write_file`, `edit_file`, `glob_search`, `grep_search`, `PowerShell` | `claw::runtime` (real execution) |
| Task | `TaskCreate`, `TaskGet`, `TaskList`, `TaskStop`, `TaskUpdate`, `TaskOutput` | `TaskRegistry` singleton |
| Worker | `WorkerCreate/Get/Observe/ResolveTrust/AwaitReady/SendPrompt/Restart/Terminate` | `WorkerRegistry` singleton |
| Team / Cron | `TeamCreate`, `TeamDelete`, `CronCreate`, `CronDelete`, `CronList` | `TeamRegistry` / `CronRegistry` |
| LSP / MCP | `LSP`, `ListMcpResources`, `ReadMcpResource`, `McpAuth`, `MCP` | `LspRegistry` / `McpToolRegistry` |
| Web | `WebFetch`, `WebSearch` | libcurl HTTP client |
| Interactive | `TodoWrite`, `Skill`, `Agent`, `ToolSearch`, `NotebookEdit` | misc_tools / agent_tools |
| Session | `Sleep`, `SendUserMessage`/`Brief`, `Config`, `REPL`, `AskUserQuestion` | misc_tools |
| Plan | `EnterPlanMode`, `ExitPlanMode`, `StructuredOutput` | misc_tools |
| Other | `RemoteTrigger`, `TestingPermission` | HTTP client / stub |

## Conversion guidelines

| Rust | C++ |
|---|---|
| `Result<T, E>` | `tl::expected<T, E>` |
| `Option<T>` | `std::optional<T>` |
| `Vec<T>` | `std::vector<T>` |
| `enum` (with data) | `std::variant` |
| `enum` (simple) | `enum class` |
| `trait` | Abstract base class |
| `Arc<T>` | `std::shared_ptr<T>` |
| `Box<dyn Fn>` | `std::function` |
| `serde_json::Value` | `nlohmann::json` |
| `async fn` (tokio) | Synchronous / `std::thread` + `std::future` |
| `&str` / `String` | `std::string_view` / `std::string` |
| Filesystem paths | `std::filesystem::path` |

## Platform support

| Platform | Status |
|---|---|
| Windows 11 x64 (MSVC) | Primary target, fully tested |
| Linux (GCC/Clang) | Compiles with POSIX code paths; not CI-tested yet |
| macOS | Expected to work via Clang; not tested |

## Relationship to the original project

This C++ port is derived from the **Rust rewrite** at
[instructkr/claw-code](https://github.com/instructkr/claw-code) (`dev/rust` branch),
which is itself a clean-room reimplementation of the Claude Code agent harness.

- This repository is **not affiliated with or endorsed by Anthropic**
- No proprietary Claude Code source material is included
- The Rust source is the authoritative reference for behaviour
