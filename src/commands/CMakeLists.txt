cmake_minimum_required(VERSION 3.25)
project(commands VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# C++20 standard – required for: concepts, ranges, std::span, std::format,
# structured bindings, [[nodiscard]], constexpr/consteval improvements.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(commands STATIC
    src/commands.cpp
)

target_include_directories(commands
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# ---------------------------------------------------------------------------
# Compiler-specific warnings and diagnostics
# ---------------------------------------------------------------------------
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(commands PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter   # Some generated stubs leave params unused
    )
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(commands PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus         # Correctly report __cplusplus value
        /utf-8                  # Source and execution character set
    )
endif()

# ---------------------------------------------------------------------------
# Optional: nlohmann/json – used by future JSON-emitting helpers.
# Use find_package if available; otherwise skip.
# ---------------------------------------------------------------------------
find_package(nlohmann_json QUIET)
if (nlohmann_json_FOUND)
    target_link_libraries(commands PUBLIC nlohmann_json::nlohmann_json)
    target_compile_definitions(commands PRIVATE COMMANDS_HAVE_NLOHMANN_JSON=1)
endif()

# ---------------------------------------------------------------------------
# Runtime / plugins dependencies.
# When built as part of the unified project, the top-level CMakeLists.txt
# wires claw_runtime and claw_plugins via target_link_libraries.
# When built standalone, create INTERFACE stubs so the build still works.
# ---------------------------------------------------------------------------
if(NOT TARGET claw_runtime)
    add_library(runtime_stub INTERFACE)
    target_include_directories(runtime_stub INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_link_libraries(commands PUBLIC runtime_stub)
endif()

if(NOT TARGET claw_plugins)
    add_library(plugins_stub INTERFACE)
    target_include_directories(plugins_stub INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_link_libraries(commands PUBLIC plugins_stub)
endif()

# ---------------------------------------------------------------------------
# Tests  (enable with -DCOMMANDS_BUILD_TESTS=ON)
# ---------------------------------------------------------------------------
option(COMMANDS_BUILD_TESTS "Build unit tests for the commands library" OFF)

if (COMMANDS_BUILD_TESTS)
    enable_testing()

    # Use Google Test if available; otherwise fall back to a minimal harness.
    find_package(GTest QUIET)

    if (GTest_FOUND)
        add_executable(commands_tests
            tests/test_slash_commands.cpp
        )
        target_link_libraries(commands_tests PRIVATE commands GTest::gtest_main)
        include(GoogleTest)
        gtest_discover_tests(commands_tests)
    else()
        message(STATUS
            "GTest not found – skipping commands unit tests. "
            "Install googletest or set GTest_DIR.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Installation rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS commands
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
