cmake_minimum_required(VERSION 3.16)
project(tailslayer CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(tailslayer INTERFACE)
target_include_directories(tailslayer INTERFACE include)
target_compile_definitions(tailslayer INTERFACE
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:RelWithDebInfo>:NDEBUG>
    $<$<CONFIG:MinSizeRel>:NDEBUG>
)

add_executable(tailslayer_example tailslayer_example.cpp)
target_link_libraries(tailslayer_example PRIVATE tailslayer pthread)
