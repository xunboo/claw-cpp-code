cmake_minimum_required(VERSION 3.25)
project(claw_runtime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Dependencies ──────────────────────────────────────────────────────────────

# nlohmann/json (fetched by top-level CMake or standalone)
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json QUIET)
    if(NOT nlohmann_json_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
            URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        )
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
endif()

# OpenSSL (for SHA256 in oauth.cpp and prompt.cpp) — optional on Windows
if(NOT TARGET OpenSSL::Crypto)
    find_package(OpenSSL QUIET)
endif()

# Threads
find_package(Threads REQUIRED)

# ─── Library target ───────────────────────────────────────────────────────────

set(RUNTIME_HEADERS
    include/bash.hpp
    include/bash_validation.hpp
    include/bootstrap.hpp
    include/compact.hpp
    include/config.hpp
    include/conversation.hpp
    include/file_ops.hpp
    include/green_contract.hpp
    include/hooks.hpp
    include/json_value.hpp
    include/lsp_client.hpp
    include/mcp.hpp
    include/mcp_client.hpp
    include/mcp_lifecycle_hardened.hpp
    include/mcp_stdio.hpp
    include/mcp_tool_bridge.hpp
    include/oauth.hpp
    include/permissions.hpp
    include/permission_enforcer.hpp
    include/plugin_lifecycle.hpp
    include/policy_engine.hpp
    include/prompt.hpp
    include/recovery_recipes.hpp
    include/remote.hpp
    include/sandbox.hpp
    include/session.hpp
    include/session_control.hpp
    include/sse.hpp
    include/stale_branch.hpp
    include/summary_compression.hpp
    include/task_packet.hpp
    include/task_registry.hpp
    include/team_cron_registry.hpp
    include/trust_resolver.hpp
    include/usage.hpp
    include/worker_boot.hpp
)

set(RUNTIME_SOURCES
    src/bash.cpp
    src/bash_validation.cpp
    src/bootstrap.cpp
    src/compact.cpp
    src/config.cpp
    src/conversation.cpp
    src/file_ops.cpp
    src/green_contract.cpp
    src/hooks.cpp
    src/json_value.cpp
    src/lsp_client.cpp
    src/mcp.cpp
    src/mcp_client.cpp
    src/mcp_lifecycle_hardened.cpp
    src/mcp_stdio.cpp
    src/mcp_tool_bridge.cpp
    src/oauth.cpp
    src/permissions.cpp
    src/permission_enforcer.cpp
    src/plugin_lifecycle.cpp
    src/policy_engine.cpp
    src/prompt.cpp
    src/recovery_recipes.cpp
    src/remote.cpp
    src/sandbox.cpp
    src/session.cpp
    src/session_control.cpp
    src/sse.cpp
    src/stale_branch.cpp
    src/summary_compression.cpp
    src/task_packet.cpp
    src/task_registry.cpp
    src/team_cron_registry.cpp
    src/trust_resolver.cpp
    src/usage.cpp
    src/worker_boot.cpp
)

add_library(claw_runtime STATIC ${RUNTIME_SOURCES} ${RUNTIME_HEADERS})

target_include_directories(claw_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(claw_runtime
    PUBLIC
        nlohmann_json::nlohmann_json
        Threads::Threads
)
if(TARGET OpenSSL::Crypto)
    target_link_libraries(claw_runtime PUBLIC OpenSSL::Crypto)
endif()

# ─── Compiler options ─────────────────────────────────────────────────────────

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(claw_runtime PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
endif()

if(MSVC)
    target_compile_options(claw_runtime PRIVATE /W4 /WX- /FS)
endif()

# ─── Install ──────────────────────────────────────────────────────────────────

install(TARGETS claw_runtime ARCHIVE DESTINATION lib)
install(FILES ${RUNTIME_HEADERS} DESTINATION include/claw_runtime)

# ─── Optional: tests placeholder ──────────────────────────────────────────────

option(CLAW_RUNTIME_BUILD_TESTS "Build claw_runtime tests" OFF)
if(CLAW_RUNTIME_BUILD_TESTS)
    enable_testing()
    # Tests would be added here
    # add_subdirectory(tests)
endif()
