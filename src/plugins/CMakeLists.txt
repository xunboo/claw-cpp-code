cmake_minimum_required(VERSION 3.22)
project(claw-plugins VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Dependencies ───────────────────────────────────────────────────────────────
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json REQUIRED)
endif()

# ── Library target ─────────────────────────────────────────────────────────────
add_library(claw_plugins
    src/plugin_error.cpp
    src/plugin_types.cpp
    src/plugin_tool.cpp
    src/plugin.cpp
    src/plugin_registry.cpp
    src/plugin_manager.cpp
    src/hooks.cpp
)

target_include_directories(claw_plugins
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
)

target_link_libraries(claw_plugins
    PUBLIC nlohmann_json::nlohmann_json
)

if(WIN32)
    target_compile_definitions(claw_plugins PRIVATE _WIN32_WINNT=0x0A00)
endif()

target_compile_options(claw_plugins PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
