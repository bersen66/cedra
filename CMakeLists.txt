cmake_minimum_required(VERSION 3.20)
project(cedra LANGUAGES CXX)

# We should not use directly CMAKE_CXX_STANDARD because
# it would override user variable value if library included
# using `add_subdirectory` command
set(CDR_CXX_STANDARD 20)

include(CTest)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(APPEND CDR_COMMON_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CdrHelpers)

option(CDR_BUILD_TESTS "If ON CDR will build all of cedra's own tests" ON)
option(CDR_BUILD_BENCH "If ON CDR will build all of cedra's own benchmarks" ON)

message(STATUS "Building tests: ${CDR_BUILD_TESTS}")
message(STATUS "Building benchmarks: ${CDR_BUILD_BENCH}")

# If project included using `add_subdirectory(cedra)`
# we don't need to perform proper installation
if(NOT CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    option(CDR_ENABLE_INSTALL "Enable installing" OFF)
else()
    option(CDR_ENABLE_INSTALL "Enable installing" ON)
endif()

include(PrepareDependencies)
add_subdirectory(cdr)

if (CDR_ENABLE_INSTALL)
    include(CMakePackageConfigHelpers)

    install(EXPORT CdrTargets
        FILE CdrTargets.cmake
        NAMESPACE cdr::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cedra
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CdrConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/CdrConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cedra
    )

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/CdrConfigVersion.cmake"
        VERSION 0.1.0
        COMPATIBILITY AnyNewerVersion
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/CdrConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/CdrConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cedra
    )
endif()
