cmake_minimum_required(VERSION 3.28)
project(tailslayer VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(tailslayer INTERFACE)
add_library(tailslayer::tailslayer ALIAS tailslayer)
target_sources(
    tailslayer
    INTERFACE
    FILE_SET HEADERS
    BASE_DIRS include
    FILES include/tailslayer/hedged_reader.hpp
)
target_link_libraries(tailslayer INTERFACE Threads::Threads)
target_compile_features(tailslayer INTERFACE cxx_std_17)

option(TAILSLAYER_BUILD_DISCOVERY "Build discovery tools" "${PROJECT_IS_TOP_LEVEL}")
if (TAILSLAYER_BUILD_DISCOVERY)
    add_subdirectory(discovery)
endif ()

option(TAILSLAYER_BUILD_EXAMPLE "Build example application" "${PROJECT_IS_TOP_LEVEL}")
if (TAILSLAYER_BUILD_EXAMPLE)
    add_executable(tailslayer_example tailslayer_example.cpp)
    target_link_libraries(tailslayer_example PRIVATE tailslayer::tailslayer)
endif ()

if (NOT CMAKE_SKIP_INSTALL_RULES)
    include(GNUInstallDirs)
    set(TAILSLAYER_INSTALL_CMAKEDIR "${CMAKE_INSTALL_DATADIR}/cmake/tailslayer"
        CACHE STRING "relative path below install root for CMake package files")
    mark_as_advanced(TAILSLAYER_INSTALL_CMAKEDIR)

    install(
        TARGETS tailslayer
        EXPORT tailslayer-targets
        FILE_SET HEADERS
        COMPONENT tailslayer-dev
    )
    install(
        EXPORT tailslayer-targets
        DESTINATION "${TAILSLAYER_INSTALL_CMAKEDIR}"
        NAMESPACE tailslayer::
        COMPONENT tailslayer-dev
    )

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        cmake/tailslayer-config.cmake.in cmake/tailslayer-config.cmake
        INSTALL_DESTINATION "${TAILSLAYER_INSTALL_CMAKEDIR}"
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/cmake/tailslayer-config-version.cmake"
        COMPATIBILITY SameMajorVersion
    )
    install(
        DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cmake/"
        DESTINATION "${TAILSLAYER_INSTALL_CMAKEDIR}"
        COMPONENT tailslayer-dev
    )
endif ()
