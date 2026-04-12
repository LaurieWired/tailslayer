list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
find_package(Libm REQUIRED)

add_executable(trefi_probe trefi_probe.c)
target_link_libraries(trefi_probe PRIVATE libm::libm)

add_subdirectory(benchmark)
