# When built standalone, this is the root project.
# When included via the top-level CMakeLists.txt, the project() is already set.
if(NOT PROJECT_NAME OR PROJECT_NAME STREQUAL "Project")
    cmake_minimum_required(VERSION 3.25)
    project(claw-cpp-code
        VERSION     0.1.0
        LANGUAGES   CXX
        DESCRIPTION "Claw C++20 monorepo"
    )
endif()

# ===========================================================================
# Global defaults
# ===========================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put all binaries in a common output directory so executables can find DLLs
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# MSVC: sane defaults
if(MSVC)
    add_compile_options(/utf-8 /Zc:__cplusplus /permissive- /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _SILENCE_ALL_CXX23_DEPRECATION_WARNINGS)
endif()

# ===========================================================================
# Options
# ===========================================================================
option(CLAW_BUILD_TESTS   "Build unit tests"                    ON)
option(CLAW_BUILD_MOCK    "Build mock-anthropic-service exe"    ON)

# ===========================================================================
# Third-party dependencies (resolved once, shared by all sub-projects)
# ===========================================================================

# --- tl::expected (C++20 backport of std::expected) ----------------------
include(FetchContent)
FetchContent_Declare(
    tl_expected
    GIT_REPOSITORY https://github.com/TartanLlama/expected.git
    GIT_TAG        v1.3.0
)
set(EXPECTED_BUILD_TESTS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(tl_expected)

# --- nlohmann/json (header-only) -----------------------------------------
find_package(nlohmann_json 3.12 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL      https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- libcurl --------------------------------------------------------------
find_package(CURL QUIET)
if(NOT CURL_FOUND)
    message(STATUS "libcurl not found locally — fetching via FetchContent...")
    FetchContent_Declare(
        curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG        curl-8_19_0
        GIT_SHALLOW    TRUE
    )
    set(BUILD_CURL_EXE OFF CACHE INTERNAL "")
    set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
    set(CURL_USE_SCHANNEL ON CACHE INTERNAL "")
    set(CURL_USE_OPENSSL OFF CACHE INTERNAL "")
    set(BUILD_TESTING OFF CACHE INTERNAL "")
    set(CURL_DISABLE_INSTALL ON CACHE INTERNAL "")
    set(CURL_USE_LIBPSL OFF CACHE INTERNAL "")
    set(USE_NGHTTP2 OFF CACHE INTERNAL "")
    set(CURL_BROTLI OFF CACHE INTERNAL "")
    set(CURL_ZSTD OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(curl)
    # Create CURL::libcurl alias if not already present
    if(NOT TARGET CURL::libcurl)
        add_library(CURL::libcurl ALIAS libcurl)
    endif()
    set(CURL_FOUND TRUE)
endif()

# --- OpenSSL --------------------------------------------------------------
find_package(OpenSSL QUIET)
if(NOT OPENSSL_FOUND)
    message(WARNING "OpenSSL not found — runtime crate will not link. Install via vcpkg: vcpkg install openssl:x64-windows")
    if(NOT TARGET OpenSSL::Crypto)
        add_library(OpenSSL::Crypto INTERFACE IMPORTED)
    endif()
endif()

# --- Threads --------------------------------------------------------------
find_package(Threads REQUIRED)

# ===========================================================================
# Sub-projects  (order matters: dependencies before dependents)
# ===========================================================================

# Leaf libraries (no inter-crate deps)
add_subdirectory(telemetry)
add_subdirectory(compat-harness)

# Core libraries
add_subdirectory(runtime)
add_subdirectory(plugins)
add_subdirectory(tools)
add_subdirectory(api)

# Higher-level libraries
add_subdirectory(commands)

# Executables
add_subdirectory(rusty-claude-cli)

if(CLAW_BUILD_MOCK)
    add_subdirectory(mock-anthropic-service)
endif()

# ===========================================================================
# Inter-crate link dependencies
#
# The sub-crate CMakeLists.txt files were written standalone.  We wire the
# real targets together here so everything links correctly in the unified
# build.  Each target_link_libraries call is idempotent.
# ===========================================================================

# tl::expected for all targets (C++20 backport of std::expected)
foreach(_tgt claw_runtime claw_plugins claw_tools commands claw_api claw_telemetry compat-harness claw_lib)
    if(TARGET ${_tgt})
        target_link_libraries(${_tgt} PUBLIC tl::expected)
    endif()
endforeach()

# tools depends on runtime (for PermissionMode, PermissionEnforcer) and plugins
target_link_libraries(claw_tools    PUBLIC claw_runtime claw_plugins)

# commands depends on runtime and plugins (replaces the old stubs)
target_link_libraries(commands      PUBLIC claw_runtime claw_plugins)

# api depends on runtime (for OAuthTokenSet, config types) and telemetry (for RequestProfile)
target_link_libraries(claw_api      PUBLIC claw_runtime claw_telemetry)

# cli depends on everything
target_link_libraries(claw_lib PUBLIC
    claw_api
    claw_runtime
    claw_plugins
    claw_tools
    commands
    claw_telemetry
    compat-harness
)

# ===========================================================================
# Summary
# ===========================================================================
message(STATUS "")
message(STATUS "=== claw-cpp-code build configuration ===")
message(STATUS "  C++ standard : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Generator    : ${CMAKE_GENERATOR}")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install path : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Tests        : ${CLAW_BUILD_TESTS}")
message(STATUS "  Mock service : ${CLAW_BUILD_MOCK}")
message(STATUS "  nlohmann_json: ${nlohmann_json_VERSION}")
message(STATUS "  CURL         : ${CURL_VERSION_STRING}")
message(STATUS "  OpenSSL      : ${OPENSSL_VERSION}")
message(STATUS "==========================================")
message(STATUS "")
