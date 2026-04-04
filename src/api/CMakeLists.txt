cmake_minimum_required(VERSION 3.20)
project(claw_api VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# C++20 globally
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(API_SOURCES
    src/error.cpp
    src/types.cpp
    src/sse.cpp
    src/prompt_cache.cpp
    src/client.cpp
    src/providers/mod.cpp
    src/providers/anthropic.cpp
    src/providers/openai_compat.cpp
)

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(claw_api STATIC ${API_SOURCES})

target_include_directories(claw_api
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# nlohmann/json  (header-only; fetched by top-level CMake or standalone)
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json QUIET)
    if(NOT nlohmann_json_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG        v3.12.0
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
endif()

target_link_libraries(claw_api PUBLIC nlohmann_json::nlohmann_json)

# libcurl  (used for HTTP transport in provider implementations)
if(NOT TARGET CURL::libcurl)
    find_package(CURL REQUIRED)
endif()
target_link_libraries(claw_api PUBLIC CURL::libcurl)

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(claw_api PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
    )
elseif(MSVC)
    target_compile_options(claw_api PRIVATE
        /W4 /wd4100  # C4100 = unreferenced formal parameter
    )
endif()

# ---------------------------------------------------------------------------
# Optional: install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS claw_api
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})