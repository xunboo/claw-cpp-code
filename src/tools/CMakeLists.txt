cmake_minimum_required(VERSION 3.20)
project(claw_tools CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Dependencies ──────────────────────────────────────────────────────────────

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json REQUIRED)
endif()
if(NOT TARGET CURL::libcurl)
    find_package(CURL REQUIRED)
endif()

# ── Library ───────────────────────────────────────────────────────────────────

add_library(claw_tools STATIC
    src/tool_specs.cpp
    src/tool_registry.cpp
    src/tool_executor.cpp
    src/web_tools.cpp
    src/agent_tools.cpp
    src/misc_tools.cpp
    src/lane_completion.cpp
)

target_include_directories(claw_tools
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(claw_tools
    PUBLIC
        nlohmann_json::nlohmann_json
        CURL::libcurl
)

if(WIN32)
    target_link_libraries(claw_tools PUBLIC ws2_32)
endif()

# C++23 std::expected is available in MSVC 19.36+ / GCC 13+ / Clang 16+.
# For older toolchains, add a polyfill include path here:
# target_include_directories(claw_tools PRIVATE third_party/expected/include)
