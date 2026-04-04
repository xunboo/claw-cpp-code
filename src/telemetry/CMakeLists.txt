cmake_minimum_required(VERSION 3.22)
project(claw-telemetry VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json REQUIRED)
endif()

add_library(claw_telemetry STATIC
    src/telemetry.cpp
)

target_include_directories(claw_telemetry
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
)

target_link_libraries(claw_telemetry PUBLIC nlohmann_json::nlohmann_json)

target_compile_definitions(claw_telemetry
    PRIVATE TELEMETRY_PKG_VERSION="${PROJECT_VERSION}"
)

target_compile_options(claw_telemetry PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)
