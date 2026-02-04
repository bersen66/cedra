# ==============================================================================
# CDR CMake Build Helpers
#
# This file defines a set of CMake functions that provide a lightweight
# abstraction layer similar to Bazel build rules.
#
# These functions follow a consistent naming convention and automatically
# prefix all generated targets with `cdr_`, while also providing convenient
# alias targets of the form `cdr::<name>`.
#
# Defined functions:
#   - cdr_cpp_library()     — define a C++ library (shared or header-only)
#   - cdr_cpp_test()        — define a unit test target (integrated with CTest)
#   - cdr_cpp_executable()  — define a standalone or benchmark executable
#
# All functions respect the following build-time options:
#   CDR_BUILD_TESTS   — enable or disable test targets
#   CDR_BUILD_BENCH   — enable or disable benchmark targets
#
# Dependencies:
#   include(GNUInstallDirs)
#   include(CMakeParseArguments)
#
# Example:
#   cdr_cpp_library(
#     NAME math
#     SRCS "math_utils.cc"
#     HDRS "math_utils.h"
#     DEPS cdr::core
#   )
#
#   cdr_cpp_test(
#     NAME math_test
#     SRCS "math_test.cc"
#     DEPS cdr::math GTest::gtest_main
#   )
#
#   cdr_cpp_executable(
#     NAME benchmark_runner
#     SRCS "bench_main.cc"
#     DEPS cdr::core benchmark::benchmark
#     BENCH
#   )
# ==============================================================================
include(GNUInstallDirs)
include(CMakeParseArguments)

# cdr_cpp_library()
#
# CMake function to create a C++ library in the CDR project style.
#
# Parameters:
#   NAME:        Name of the library (required)
#   HDRS:        List of header files (optional)
#   SRCS:        List of source files (.cc, .cpp, etc.)
#   DEPS:        List of dependent libraries to link against
#   COPTS:       List of private compile options
#
# Options:
#   PUBLIC:      Marks the library as publicly visible (for installation)
#   TESTONLY:    Builds the library only when CDR_BUILD_TESTS is enabled
#   BENCHONLY:   Builds the library only when CDR_BUILD_BENCH is enabled
#
# Behavior:
#   - Automatically prefixes target names with 'cdr_' (e.g., cdr_math).
#   - Creates alias target `cdr::<name>` for convenient linking.
#   - If no source files are provided, creates a header-only INTERFACE library.
#   - Includes default CDR include directories for both build and install interfaces.
#   - Automatically filters out header files from SRCS if accidentally listed.
#
# Note:
#   Header-only libraries are created as INTERFACE targets,
#   while others are built as SHARED libraries.
#
# Usage:
#   cdr_cpp_library(
#     NAME
#       math
#     HDRS
#       "math_utils.h"
#     SRCS
#       "math_utils.cc"
#     DEPS
#       cdr::core
#     COPTS
#       "-Wall" "-Wextra"
#     PUBLIC
#   )
function(cdr_cpp_library)
    set(options PUBLIC TESTONLY BENCHONLY)
    set(oneValueArgs NAME)
    set(multiValueArgs HDRS SRCS DEPS COPTS)
    cmake_parse_arguments(ARGS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(_NAME "${ARGS_NAME}")

    if (ARGS_NAME STREQUAL "")
        message(FATAL_ERROR "Name of library is required")
    endif()

    if (NOT ${CDR_BUILD_TESTS} AND ${ARGS_TESTONLY})
        return()
    endif()

    if (NOT ${CDR_BUILD_BENCH} AND ${ARGS_BENCHONLY})
        return()
    endif()

    set(CDR_SOURCES "${ARGS_SRCS}")
    foreach(src_file IN LISTS CDR_SOURCES)
        if(${src_file} MATCHES ".*\\.(h|inc|hpp|hh)")
            list(REMOVE_ITEM CDR_SOURCES "${src_file}")
        endif()
    endforeach()

    if(CDR_SOURCES STREQUAL "")
        set(CDR_LIB_IS_HEADER_ONLY 1)
    else()
        set(CDR_LIB_IS_HEADER_ONLY 0)
    endif()

    if (CDR_LIB_IS_HEADER_ONLY)
        add_library(${_NAME} INTERFACE)
        target_include_directories(${_NAME}
            INTERFACE
            "$<BUILD_INTERFACE:${CDR_COMMON_INCLUDE_DIRS}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        )
        target_link_libraries(${_NAME} INTERFACE ${ARGS_DEPS})
    else()
        add_library(${_NAME} SHARED ${CDR_SOURCES})
        target_include_directories(${_NAME}
            PUBLIC
            "$<BUILD_INTERFACE:${CDR_COMMON_INCLUDE_DIRS}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        )
        target_link_libraries(${_NAME} PUBLIC ${ARGS_DEPS})
        target_compile_options(${_NAME} PRIVATE ${ARGS_COPTS})
        set_property(TARGET ${_NAME} PROPERTY CXX_STANDARD ${CDR_CXX_STANDARD})
    endif()

    add_library(cdr::${ARGS_NAME} ALIAS ${_NAME})

    if (ARGS_PUBLIC AND CDR_ENABLE_INSTALL)
        # Install the target and associate it with the export set "CdrTargets"
        install(TARGETS ${_NAME}
            EXPORT CdrTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )

        # Install headers while preserving directory structure
        foreach(HDR ${ARGS_HDRS})
            # Get the directory of the current header relative to the project root
            file(RELATIVE_PATH REL_DIR "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")

            # Install to a matching path in the include directory
            install(FILES ${HDR} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${REL_DIR}")
        endforeach()
    endif()
endfunction()


# cdr_cpp_test()
#
# CMake function to define a unit test target in the CDR project style.
# This imitates Bazel's cc_test rule.
#
# Parameters:
#   NAME:   Name of the test target (required)
#   SRCS:   List of source files for the test binary
#   DEPS:   List of dependent libraries to link with the test
#   COPTS:  List of private compile options
#
# Behavior:
#   - Creates an executable target with the specified sources.
#   - Links it against the listed dependencies.
#   - Registers it as a CTest target with `add_test`.
#   - Skips creation if CDR_BUILD_TESTS is disabled.
#
# Usage:
#   cdr_cpp_test(
#     NAME
#       math_test
#     SRCS
#       "math_test.cc"
#     DEPS
#       cdr::math
#       GTest::gmock
#       GTest::gtest_main
#     COPTS
#       "-O0" "-g"
#   )
function(cdr_cpp_test)
    if (NOT ${CDR_BUILD_TESTS})
        return()
    endif()

    cmake_parse_arguments(
        ARGS
        ""
        "NAME"
        "SRCS;COPTS;DEPS"
        ${ARGN}
    )

    set(_NAME ${ARGS_NAME})
    if (${_NAME} STREQUAL "")
        message(FATAL_ERROR "Test must have name")
    endif()

    add_executable(${_NAME} ${ARGS_SRCS})
    target_compile_options(${_NAME} PRIVATE ${ARGS_COPTS})
    target_link_libraries(${_NAME} PUBLIC ${ARGS_DEPS})
    add_test(NAME ${_NAME} COMMAND ${_NAME})
    set_property(TARGET ${_NAME} PROPERTY CXX_STANDARD ${CDR_CXX_STANDARD})
endfunction()

# cdr_cpp_executable()
#
# CMake function to define an executable target in the CDR project style.
#
# Parameters:
#   NAME:   Name of the executable (required)
#   SRCS:   List of source files
#   HDRS:   List of header files (optional)
#   DEPS:   List of dependent libraries to link against
#   COPTS:  List of private compile options
#
# Options:
#   BENCH:  If specified, the target is built only when CDR_BUILD_BENCH is enabled.
#
# Behavior:
#   - Creates an executable with the given sources.
#   - Applies compile options and links dependencies.
#   - Skips creation if BENCH is specified and CDR_BUILD_BENCH is disabled.
#
# Note:
#   Future versions may include installation rules for executables.
#
# Usage:
#   cdr_cpp_executable(
#     NAME
#       benchmark_runner
#     SRCS
#       "bench_main.cc"
#     DEPS
#       cdr::core
#       benchmark::benchmark
#     COPTS
#       "-O3"
#     BENCH
#   )
function(cdr_cpp_executable)

    cmake_parse_arguments(
        ARGS
        "BENCH"
        "NAME"
        "SRCS;COPTS;HDRS;DEPS"
        ${ARGN}
    )

    set(_NAME ${ARGS_NAME})

    if ("${_NAME}" STREQUAL "")
        message(FATAL_ERROR "Name for executable must be provided")
    endif()

    if (NOT ${CDR_BUILD_BENCH} AND ${ARGS_BENCH})
        return()
    endif()

    add_executable(${_NAME} ${ARGS_SRCS})
    target_compile_options(${_NAME} PRIVATE ${ARGS_COPTS})
    target_link_libraries(${_NAME} PUBLIC ${ARGS_DEPS})
    set_property(TARGET ${_NAME} PROPERTY CXX_STANDARD ${CDR_CXX_STANDARD})

    if (CDR_ENABLE_INSTALL)
        install(TARGETS ${_NAME}
            EXPORT CdrTargets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            BUNDLE DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()

endfunction()
