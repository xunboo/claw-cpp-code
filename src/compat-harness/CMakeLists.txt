cmake_minimum_required(VERSION 3.25)
project(compat-harness VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# C++20 baseline
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BUILD_TESTS  "Build Catch2 unit / integration tests" OFF)

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(compat-harness STATIC
    src/compat_harness.cpp
)

target_include_directories(compat-harness
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Compiler warnings
if (MSVC)
    target_compile_options(compat-harness PRIVATE /W4 /permissive-)
else()
    target_compile_options(compat-harness PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

# ---------------------------------------------------------------------------
# std::expected availability shim
#
# MSVC 19.36+ / GCC 12+ / Clang 16+ support <expected> natively.
# If your toolchain is older, uncomment the find_package below and link
# to the tl::expected header-only library instead:
#
#   find_package(tl-expected REQUIRED)
#   target_link_libraries(compat-harness PUBLIC tl::expected)
#   target_compile_definitions(compat-harness PUBLIC USE_TL_EXPECTED)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Optional: tests via Catch2
# ---------------------------------------------------------------------------
if (BUILD_TESTS)
    # Requires Catch2 v3 to be installed (e.g. via vcpkg or Conan).
    find_package(Catch2 3 REQUIRED)

    add_executable(compat-harness-tests
        src/compat_harness_test.cpp
    )

    target_link_libraries(compat-harness-tests
        PRIVATE
            compat-harness
            Catch2::Catch2WithMain
    )

    include(CTest)
    include(Catch)
    catch_discover_tests(compat-harness-tests)
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS compat-harness
    EXPORT  compat-harness-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT compat-harness-targets
    FILE       compat-harness-targets.cmake
    NAMESPACE  
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/compat-harness
)