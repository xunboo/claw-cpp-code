cmake_minimum_required(VERSION 3.22)
project(claw-mock-anthropic-service VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json REQUIRED)
endif()

# ── library ──────────────────────────────────────────────────────────────────
add_library(claw_mock_anthropic_service STATIC
    src/scenario.cpp
    src/response_builder.cpp
    src/mock_anthropic_service.cpp
)

target_include_directories(claw_mock_anthropic_service
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
)

target_link_libraries(claw_mock_anthropic_service
    PUBLIC nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(claw_mock_anthropic_service PUBLIC ws2_32)
endif()

target_compile_options(claw_mock_anthropic_service PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)

# ── standalone executable ─────────────────────────────────────────────────────
add_executable(mock_anthropic_service_exe src/main.cpp)

target_link_libraries(mock_anthropic_service_exe
    PRIVATE claw_mock_anthropic_service
)

set_target_properties(mock_anthropic_service_exe PROPERTIES
    OUTPUT_NAME mock-anthropic-service
)

target_compile_options(mock_anthropic_service_exe PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)
