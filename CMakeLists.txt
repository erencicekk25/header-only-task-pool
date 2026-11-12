cmake_minimum_required(VERSION 3.14)

project(header_only_task_pool
    VERSION 1.0.0
    DESCRIPTION "Header-only, zero-dependency C++17 thread pool with std::future support"
    LANGUAGES CXX)

# Detect whether we are the top-level project or consumed via
# add_subdirectory() / FetchContent, so we do not build tests for consumers.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(TASKPOOL_IS_TOP_LEVEL ON)
else()
    set(TASKPOOL_IS_TOP_LEVEL OFF)
endif()

option(TASKPOOL_BUILD_TESTS "Build the unit tests" ${TASKPOOL_IS_TOP_LEVEL})
option(TASKPOOL_ENABLE_SANITIZERS "Build tests with AddressSanitizer + UBSan (GCC/Clang)" OFF)
option(TASKPOOL_ENABLE_TSAN "Build tests with ThreadSanitizer (GCC/Clang)" OFF)

if(TASKPOOL_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# The library itself: a pure INTERFACE target, nothing to compile.
# ---------------------------------------------------------------------------
add_library(taskpool INTERFACE)
add_library(taskpool::taskpool ALIAS taskpool)

target_include_directories(taskpool INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(taskpool INTERFACE cxx_std_17)
target_link_libraries(taskpool INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# Shared settings for the project's own executables.
# ---------------------------------------------------------------------------
function(taskpool_configure_target target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
        if(TASKPOOL_ENABLE_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
        if(TASKPOOL_ENABLE_TSAN)
            target_compile_options(${target} PRIVATE -fsanitize=thread)
            target_link_options(${target} PRIVATE -fsanitize=thread)
        endif()
    endif()
endfunction()

if(TASKPOOL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation: enables `find_package(taskpool CONFIG REQUIRED)`.
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS taskpool EXPORT taskpoolTargets)
install(EXPORT taskpoolTargets
    NAMESPACE taskpool::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/taskpool)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/taskpoolConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/taskpoolConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/taskpool)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/taskpoolConfigVersion.cmake
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/taskpoolConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/taskpoolConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/taskpool)
