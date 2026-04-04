cmake_minimum_required(VERSION 3.25)
project(claw CXX)

# ---- C++20 standard ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Compiler warnings ----
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# -----------------------------------------------------------------------
# nlohmann/json (fetched by top-level CMake or standalone)
# -----------------------------------------------------------------------
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.12 QUIET)
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

# -----------------------------------------------------------------------
# Library target (all modules except main.cpp)
# Separating library from executable makes it easy to link tests.
# -----------------------------------------------------------------------
add_library(claw_lib STATIC
    src/args.cpp
    src/app.cpp
    src/init.cpp
    src/input.cpp
    src/render.cpp
)

target_include_directories(claw_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(claw_lib PUBLIC
    nlohmann_json::nlohmann_json
)

# -----------------------------------------------------------------------
# Executable target  (mirrors [[bin]] name = "claw" in Cargo.toml)
# -----------------------------------------------------------------------
add_executable(claw src/main.cpp)
target_link_libraries(claw PRIVATE claw_lib)

# -----------------------------------------------------------------------
# Test executables
# -----------------------------------------------------------------------
enable_testing()
foreach(test_name test_args test_init test_render)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE claw_lib)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# -----------------------------------------------------------------------
# Install
# -----------------------------------------------------------------------
install(TARGETS claw RUNTIME DESTINATION bin)