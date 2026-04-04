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

- **150 source files, ~39 000 lines of C++20** across 9 modules
- Builds with **Visual Studio 2022** (MSVC 14.44+) on Windows 11
- No C++23 features — uses [`tl::expected`](https://github.com/TartanLlama/expected) as a drop-in for `std::expected`
- CMake 3.25+ mono-repo with FetchContent for third-party dependencies
- Windows-native: BCrypt for crypto, `CreateProcess` for shell execution, UTF-8 console output

## Repository layout

```
claw-cpp-code/
  CMakeLists.txt              Top-level CMake (delegates to src/)
  README.md                   This file
  claw-cpp-code.md            Detailed project plan & conversion guidelines
  build/                      VS 2022 solution (generated)
  library/                    Vendored / third-party headers
  src/
    CMakeLists.txt             Mono-repo root: global flags, FetchContent, sub-projects
    runtime/                   Core engine (17 500 LOC) — session, MCP, config, hooks,
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
| `runtime` | `claw_runtime` | Static lib | 17 534 |
| `api` | `claw_api` | Static lib | 4 205 |
| `plugins` | `claw_plugins` | Static lib | 3 302 |
| `tools` | `claw_tools` | Static lib | 4 012 |
| `commands` | `commands` | Static lib | 3 296 |
| `rusty-claude-cli` | `claw_lib` + `claw.exe` | Lib + EXE | 4 171 |
| `telemetry` | `claw_telemetry` | Static lib | 725 |
| `compat-harness` | `compat-harness` | Static lib | 857 |
| `mock-anthropic-service` | `mock-anthropic-service.exe` | EXE | 1 033 |

## Build instructions

### Prerequisites

- **Visual Studio 2022** with C++ desktop workload (MSVC v14.44+)
- **CMake 3.25+** (bundled with VS 2022)
- **Git** (for FetchContent to pull `tl::expected` and `nlohmann/json`)

Optional (enables full functionality):
- **libcurl** — `vcpkg install curl:x64-windows` (enables HTTP client in api module)
- **OpenSSL** — `vcpkg install openssl:x64-windows` (enables native SHA256 in OAuth)

### Build

```powershell
# From a Developer Command Prompt or PowerShell with cmake on PATH:
cd claw-cpp-code
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Debug
```

### Run

```powershell
# Show version
.\bin\Debug\claw.exe --version

# Show help
.\bin\Debug\claw.exe --help

# Print status snapshot
.\bin\Debug\claw.exe status

# Start interactive REPL
.\bin\Debug\claw.exe
```

### Run tests

```powershell
# Build and run the test executables
.\bin\Debug\test_args.exe
.\bin\Debug\test_init.exe
.\bin\Debug\test_render.exe
```

## Conversion guidelines

The C++ code follows these rules to stay faithful to the Rust source:

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
